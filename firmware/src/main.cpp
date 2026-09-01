#include <Arduino.h>
#include <WiFi.h>
#include <SD_MMC.h>
#include <esp_sleep.h>
#include <time.h>
#include <vector>
#include <math.h>
#include "user_config.h"
#include "i2c_bsp.h"
#include "pala_input.h"
#include "board_power_bsp.h"
#include "epaper_driver_bsp.h"
#include "pala_ui.h"
#include "pala_record.h"
#include "pala_net.h"
#include "pala_sync.h"
#include "logo_mn.h"

#define BAT_ADC_PIN 4
#define BAT_EMPTY_MV 3300
#define BAT_FULL_MV 4200

static board_power_bsp_t pwr(EPD_PWR_PIN, Audio_PWR_PIN, VBAT_PWR_PIN);
static I2cMasterBus* i2c = nullptr;
static epaper_driver_display* epd = nullptr;

enum State { ST_HOME, ST_MENU, ST_MAKE, ST_TAG, ST_TODO, ST_SETTINGS, ST_SET_WIFI, ST_SYNC, ST_STORAGE, ST_SET_IP, ST_VIEW_TAGS, ST_VIEW_LIST, ST_VIEW_NOTE, ST_HOWTO, ST_WALKTHROUGH, ST_EXTRA, ST_SYNCRATE, ST_FACTORY };
static State state = ST_HOME;
static State syncReturnTo = ST_HOME;

static uint32_t lastActivity = 0;
static uint32_t lastAutoCheck = 0;

/* Every menu is a list of choices with one of them highlighted. The top button
   moves the highlight, holding it chooses. `sel` is that highlight and `selMax`
   is how many choices the current screen has, so the wrap-around is one rule
   rather than one per screen. */
static int sel = 0;
static int selMax = 1;

static void selReset(int count) { sel = 0; selMax = count > 0 ? count : 1; }
static void selNext()           { sel = (sel + 1) % selMax; }
static void selPrev()           { sel = (sel + selMax - 1) % selMax; }

/* Rows visible at a time. At scale 2 a line of text is 16px tall, so five rows
   is what fits between the header and the footer hint. */
#define LIST_ROWS 5

/* Keep the highlighted row inside the window after the highlight moves. */
static void selEnsureVisible(int& top, int rows) {
  if (sel < top) top = sel;
  if (sel >= top + rows) top = sel - rows + 1;
  if (top < 0) top = 0;
}

/* Storage cleanup + factory reset both confirm on a second tap. */
static bool confirmFree = false;
static int factoryStage = 0;

/* The tour re-runs from Extra without clearing anything. */
static bool tourFromExtra = false;

static int recCount = 0;
static volatile bool syncCancel = false;
static String lastSavedName = "";

static const char* TAGS[5] = {"Work", "Projects", "Ideas", "Quotes", "Random"};
static String viewTag = "";
static std::vector<String> noteList;
static int listTop = 0;
static int noteRow = -1;
static bool confirmDelete = false;
static String noteTranscript = "";
static std::vector<String> transcriptLines;
static int transcriptPage = 0;


static void drawRestingScreen();

static void sleepNow() {
  drawRestingScreen();
  uiFlushFull();
  delay(300);
  pwr.POWEER_Audio_OFF();
  pwr.POWEER_EPD_OFF();
  /* The bottom button is the power button, so it is what wakes the device.
     The top one wakes it too - waking on the button you happen to press is
     kinder than making people learn which one is allowed to. */
  esp_sleep_enable_ext1_wakeup(
      (1ULL << BOOT_BUTTON_PIN) | (1ULL << PWR_BUTTON_PIN), ESP_EXT1_WAKEUP_ANY_LOW);
  esp_deep_sleep_start();
}

static bool soundOn() { return netGet("sound", "0") == "1"; }

static const uint32_t SYNC_OPTS[6] = {0, 1, 2, 4, 8, 24};

static uint32_t syncHours() {
  uint32_t h = netGetU32("syncHrs", SYNC_HOURS_DEFAULT);
  for (int i = 0; i < 6; i++) if (SYNC_OPTS[i] == h) return h;
  return SYNC_HOURS_DEFAULT;
}

static String syncRateLabel() {
  uint32_t h = syncHours();
  if (h == 0) return "off";
  return "every " + String(h) + "h";
}

static int batteryPct() {
  uint32_t raw = analogRead(BAT_ADC_PIN);
  uint32_t mv = raw * 3300 / 4095 * 2;
  if (mv > 4650) return -1;
  int pct = (int)(mv - BAT_EMPTY_MV) * 100 / (BAT_FULL_MV - BAT_EMPTY_MV);
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  return pct;
}

/* The resting screen carries one bar and nothing else. A charging device reads
   as full rather than showing a meaningless number. */
static void drawBatteryBar() {
  int pct = batteryPct();
  uiRect(20, 10, 160, 12);
  if (pct < 0) pct = 100;
  int w = 156 * pct / 100;
  if (w < 2) w = 2;
  uiFillRect(22, 12, w, 8, 0x00);
}

/* Battery bar at the top, script wordmark centred in what is left. Used for
   both the home screen and the deep-sleep screen: on e-paper the image costs
   nothing to hold, so what you leave behind is what the device looks like. */
static void drawRestingScreen() {
  uiFillRect(0, 0, 200, 200, 0xff);
  drawBatteryBar();
  uiBitmap((200 - LOGO_W) / 2, 84, LOGO_W, LOGO_H, LOGO_MN);
}

static String timestampName() {
  struct tm t;
  if (getLocalTime(&t, 300)) {
    char buf[40];
    snprintf(buf, sizeof(buf), "rec_%04d%02d%02d_%02d%02d%02d.wav",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
    return String(buf);
  }
  return "rec_" + String(millis()) + ".wav";
}

/* A note is a base name - rec_20260812_101200 - and up to three files beside
   it: .wav, .txt, .tag. Once FREE SPACE drops the audio the transcript still
   has to be readable, so nothing here may key on the .wav existing. */
static String notePath(const String& base, const char* ext) {
  return "/recordings/" + base + ext;
}

/* Cores disagree about whether name() is a basename or a full path, so reduce
   whatever came back to the last segment with the extension removed. */
static String baseOf(const String& fileName) {
  String n = fileName;
  int slash = n.lastIndexOf('/');
  if (slash >= 0) n = n.substring(slash + 1);
  int dot = n.lastIndexOf('.');
  if (dot > 0) n = n.substring(0, dot);
  return n;
}

static bool noteHasAudio(const String& base) { return SD_MMC.exists(notePath(base, ".wav")); }

static String tagOf(const String& base) {
  String t = notePath(base, ".tag");
  if (!SD_MMC.exists(t)) return "";
  File f = SD_MMC.open(t, "r");
  String v = f.readStringUntil('\n');
  f.close();
  v.trim();
  return v;
}

static void saveTag(const char* tag) {
  File f = SD_MMC.open(notePath(baseOf(lastSavedName), ".tag"), "w");
  if (f) { f.println(tag); f.close(); }
}

/* Distinct notes in directory order, counting a clip whose audio has been
   freed exactly once, via its surviving .txt. */
static void collectBases(std::vector<String>& out) {
  out.clear();
  File dir = SD_MMC.open("/recordings");
  if (!dir) return;
  File f;
  while ((f = dir.openNextFile())) {
    String n = String(f.name());
    f.close();
    String lower = n; lower.toLowerCase();
    if (!lower.endsWith(".wav") && !lower.endsWith(".txt")) continue;
    String b = baseOf(n);
    bool seen = false;
    for (size_t i = 0; i < out.size(); i++) if (out[i] == b) { seen = true; break; }
    if (!seen) out.push_back(b);
  }
  dir.close();
}

static void countRecordings() {
  std::vector<String> bases;
  collectBases(bases);
  recCount = bases.size();
}

static void refreshNoteList() {
  noteList.clear();
  std::vector<String> bases;
  collectBases(bases);
  for (int i = (int)bases.size() - 1; i >= 0; i--)
    if (tagOf(bases[i]) == viewTag) noteList.push_back(bases[i]);
}

static void loadTranscript(const String& base) {
  transcriptLines.clear();
  transcriptPage = 0;
  String t = notePath(base, ".txt");
  if (!SD_MMC.exists(t)) return;
  File f = SD_MMC.open(t, "r");
  String content = f.readString();
  f.close();
  int start = 0;
  while (start < (int)content.length() && transcriptLines.size() < 200) {
    int nl = content.indexOf('\n', start);
    if (nl < 0) nl = content.length();
    String line = content.substring(start, nl);
    line.trim();
    if (line.length()) transcriptLines.push_back(line);
    start = nl + 1;
  }
}

static void drawHome() {
  epd->EPD_Clear();
  drawRestingScreen();
  uiFlushFull();
}

static void drawMenu() {
  epd->EPD_Clear();
  uiTextCentered(10, "mono note mini", 1);
  uiRect(0, 26, 200, 1);
  uiRow(6, 36, 188, 36, "VIEW NOTES", 2, sel == 0);
  uiRow(6, 76, 188, 36, "MAKE NOTE",  2, sel == 1);
  uiRow(6, 116, 188, 36, "TO-DO",     2, sel == 2);
  uiRow(6, 156, 188, 36, "SETTINGS",  2, sel == 3);
  uiFlushFull();
}

struct TodoItem { String text; bool done; };
static std::vector<TodoItem> todos;
static int todoTop = 0;

static void todoLoad() {
  todos.clear();
  if (!SD_MMC.exists("/todo.txt")) return;
  File f = SD_MMC.open("/todo.txt", "r");
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (!line.length()) continue;
    TodoItem item;
    if (line.startsWith("[x] ")) { item.done = true; item.text = line.substring(4); }
    else if (line.startsWith("[ ] ")) { item.done = false; item.text = line.substring(4); }
    else { item.done = false; item.text = line; }
    todos.push_back(item);
  }
  f.close();
}

static void todoSave() {
  File f = SD_MMC.open("/todo.txt", "w");
  if (!f) return;
  for (auto& t : todos) {
    f.print(t.done ? "[x] " : "[ ] ");
    f.println(t.text);
  }
  f.close();
}

static void drawTodo() {
  epd->EPD_Clear();
  uiTextCentered(6, "TO-DO", 2);
  uiRect(0, 26, 200, 1);
  if (todos.empty()) uiTextCentered(96, "no jobs yet", 2);
  for (int i = 0; i < LIST_ROWS && todoTop + i < (int)todos.size(); i++) {
    int idx = todoTop + i;
    int y = 32 + i * 29;
    bool on = (idx == sel);
    if (on) uiFillRect(4, y, 192, 27, 0x00);
    uint8_t ink = on ? 0xff : 0x00;
    uiRect(10, y + 6, 15, 15, ink);
    if (todos[idx].done) uiFillRect(13, y + 9, 9, 9, ink);
    String t = todos[idx].text;
    if ((int)t.length() > 12) t = t.substring(0, 12);
    uiText(32, y + 6, t, 2, ink);
  }
  uiFillRect(0, 182, 200, 18, 0x00);
  uiTextCentered(187, "hold=tick  2tap=back", 1, 0xff);
  uiFlushFull();
}

#define MAKE_MAX_SECONDS 120

/* Timer, meter and hint all live in the partial-refresh region so the screen
   doesn't full-flash once a second while recording. */
/* Recording draws two frames in its whole life: a circle when it starts and a
   square when it stops. An e-paper panel ghosts and wears under repeated
   partial refreshes, and a running clock is the sort of thing that would
   repaint two hundred times for a two-minute note. The length is written into
   the file and shown in the note list afterwards, so nothing is lost by not
   animating it. */
static void drawMake() {
  epd->EPD_Clear();
  uiTextCentered(10, "RECORDING", 2);
  uiRect(0, 34, 200, 1);
  uiFillCircle(100, 108, 42, 0x00);
  uiTextCentered(166, "press bottom to stop", 1);
  uiFlushFull();
}

/* The square is the acknowledgement that the press landed - without it there
   is no feedback at all between stopping and the tag picker appearing. */
static void drawMakeStopped() {
  epd->EPD_Clear();
  uiTextCentered(10, "SAVED", 2);
  uiRect(0, 34, 200, 1);
  uiFillRect(64, 72, 72, 72, 0x00);
  uint32_t s = recSeconds();
  uiTextCentered(166, String(s / 60) + ":" + (s % 60 < 10 ? "0" : "") + String(s % 60), 2);
  uiFlushFull();
}

static void drawTagScreen() {
  epd->EPD_Clear();
  uiTextCentered(4, "SORT IT", 2);
  uiRect(0, 24, 200, 1);
  for (int i = 0; i < 5; i++)
    uiRow(6, 28 + i * 26, 188, 24, TAGS[i], 2, sel == i);
  uiRow(6, 158, 188, 22, "SKIP", 1, sel == 5);
  uiFillRect(0, 184, 200, 16, 0x00);
  uiTextCentered(188, "hold to file it", 1, 0xff);
  uiFlushFull();
}

static void drawHowTo() {
  epd->EPD_Clear();
  uiTextCentered(4, "HOW TO", 2);
  uiRect(0, 24, 200, 1);
  uiText(4, 30, "TOP button", 2);
  uiText(4, 50, " tap  = next", 1);
  uiText(4, 64, " hold = choose", 1);
  uiText(4, 78, " x2   = back", 1);
  uiText(4, 96, "BOTTOM button", 2);
  uiText(4, 116, " tap  = record/stop", 1);
  uiText(4, 130, " hold = scroll down", 1);
  uiText(4, 144, " 5s   = power off", 1);
  uiRect(0, 158, 200, 1);
  uiTextCentered(163, "Made by Atreya Patil", 1);
  uiTextCentered(176, "insp. PALA NOTE  (c) 2026", 1);
  uiFlushFull();
}

/* ---- guided tour -------------------------------------------------------
   Every screen is gated behind a deliberate hold of the top button, so the
   tour cannot be skipped by drumming on the buttons. A double-tap steps back.
   Step TOUR_SOUND is the exception - it wants an ON/OFF choice first. */

#define TOUR_STEPS 17
#define TOUR_SOUND 15
static int walkStep = 0;

static void drawTourStep(int step) {
  const char* title = "";
  const char* lines[7] = {0, 0, 0, 0, 0, 0, 0};
  switch (step) {
    case 0:
      title = "WELCOME";
      lines[0] = "Mono Note Mini";
      lines[1] = "";
      lines[2] = "A notebook you talk to.";
      lines[3] = "";
      lines[4] = "This tour shows every";
      lines[5] = "feature. Hold the TOP";
      lines[6] = "button to move on.";
      break;
    case 1:
      title = "THE BUTTONS";
      lines[0] = "Two buttons do it all.";
      lines[1] = "";
      lines[2] = "TOP  tap  = next";
      lines[3] = "     hold = choose";
      lines[4] = "     x2   = back";
      lines[5] = "BOT  tap  = record";
      lines[6] = "     hold = scroll down";
      break;
    case 2:
      title = "ON & OFF";
      lines[0] = "Press either button to";
      lines[1] = "wake it.";
      lines[2] = "";
      lines[3] = "Hold the BOTTOM button";
      lines[4] = "five seconds to switch";
      lines[5] = "it off. It also sleeps";
      lines[6] = "on its own when idle.";
      break;
    case 3:
      title = "THE MENU";
      lines[0] = "Four rows:";
      lines[1] = "";
      lines[2] = "VIEW NOTES   your notes";
      lines[3] = "MAKE NOTE    record";
      lines[4] = "TO-DO        checklist";
      lines[5] = "SETTINGS     the rest";
      break;
    case 4:
      title = "MAKE A NOTE";
      lines[0] = "Tap the BOTTOM button";
      lines[1] = "to start recording.";
      lines[2] = "";
      lines[3] = "Tap it again to stop";
      lines[4] = "and save.";
      lines[5] = "";
      lines[6] = "Two minutes per note.";
      break;
    case 5:
      title = "TAG IT";
      lines[0] = "Every note gets filed";
      lines[1] = "the moment you save it:";
      lines[2] = "";
      lines[3] = "Work      Projects";
      lines[4] = "Ideas     Quotes";
      lines[5] = "Random";
      lines[6] = "or skip at the bottom.";
      break;
    case 6:
      title = "VIEW NOTES";
      lines[0] = "Browse by tag. Each tag";
      lines[1] = "shows how many notes";
      lines[2] = "are filed under it.";
      lines[3] = "";
      lines[4] = "Hold BOTTOM to scroll,";
      lines[5] = "hold TOP to open one.";
      break;
    case 7:
      title = "PLAY & READ";
      lines[0] = "PLAY hears the note on";
      lines[1] = "the speaker.";
      lines[2] = "";
      lines[3] = "Hold BOTTOM to page on";
      lines[4] = "through the transcript.";
      lines[5] = "";
      lines[6] = "DELETE asks twice.";
      break;
    case 8:
      title = "TO-DO";
      lines[0] = "A checklist kept on the";
      lines[1] = "card as plain text.";
      lines[2] = "";
      lines[3] = "Hold TOP to tick a row";
      lines[4] = "off. Add or edit jobs";
      lines[5] = "from the web page.";
      break;
    case 9:
      title = "WI-FI";
      lines[0] = "Settings > Wi-Fi opens a";
      lines[1] = "hotspot from the device:";
      lines[2] = "";
      lines[3] = "  MonoNote-XXXX";
      lines[4] = "  key: record123";
      lines[5] = "";
      lines[6] = "Join it, browse the IP.";
      break;
    case 10:
      title = "SYNC NOW";
      lines[0] = "Settings > Sync now";
      lines[1] = "sends every clip that";
      lines[2] = "has no transcript yet";
      lines[3] = "to your own server.";
      lines[4] = "";
      lines[5] = "Tap the screen while it";
      lines[6] = "runs to stop early.";
      break;
    case 11:
      title = "AUTO-SYNC";
      lines[0] = "It also syncs on its own";
      lines[1] = "every 4 hours.";
      lines[2] = "";
      lines[3] = "Change the rate, or turn";
      lines[4] = "it off, in";
      lines[5] = "Settings > Extra.";
      break;
    case 12:
      title = "STORAGE";
      lines[0] = "Shows how much card is";
      lines[1] = "left.";
      lines[2] = "";
      lines[3] = "FREE SPACE deletes the";
      lines[4] = "audio of notes already";
      lines[5] = "transcribed. The text";
      lines[6] = "is always kept.";
      break;
    case 13:
      title = "MY ADDRESS";
      lines[0] = "Settings > IP joins your";
      lines[1] = "home Wi-Fi and shows the";
      lines[2] = "address to type in a";
      lines[3] = "browser.";
      lines[4] = "";
      lines[5] = "Drop your own site in";
      lines[6] = "/www on the card.";
      break;
    case 14:
      title = "EXTRA";
      lines[0] = "Settings > Extra holds:";
      lines[1] = "";
      lines[2] = "Redo tutorial";
      lines[3] = "Auto-sync rate";
      lines[4] = "Factory reset";
      lines[5] = "";
      lines[6] = "Redo brings you here.";
      break;
    case TOUR_SOUND:
      title = "SOUND";
      break;
    case 16:
      title = "READY";
      lines[0] = "That is every feature.";
      lines[1] = "";
      lines[2] = "Your notes live on the";
      lines[3] = "card as plain files.";
      lines[4] = "Nothing leaves the";
      lines[5] = "device unless you point";
      lines[6] = "it at your own server.";
      break;
  }

  epd->EPD_Clear();
  uiTextCentered(6, title, 1);
  uiRect(0, 18, 200, 1);

  if (step == TOUR_SOUND) {
    uiTextCentered(28, "A soft tick on each press.", 1);
    uiTextCentered(44, soundOn() ? "NOW: ON" : "NOW: OFF", 1);
    uiRow(10, 62, 84, 32, "ON",  2, sel == 0);
    uiRow(106, 62, 84, 32, "OFF", 2, sel == 1);
    uiTextCentered(104, "Tap TOP to switch,", 1);
    uiTextCentered(118, "hold TOP to carry on.", 1);
  } else {
    for (int i = 0; i < 7; i++)
      if (lines[i] && lines[i][0]) uiText(4, 26 + i * 15, lines[i], 1);
  }

  uiTextCentered(138, "STEP " + String(step + 1) + " / " + String(TOUR_STEPS), 1);

  if (step != TOUR_SOUND) {
    uiFillRect(0, 154, 200, 46, 0x00);
    uiTextCentered(160, step == TOUR_STEPS - 1 ? "HOLD TOP = FINISH"
                                               : "HOLD TOP = NEXT", 1, 0xff);
    if (step > 0) uiTextCentered(176, "DOUBLE-TAP = BACK", 1, 0xff);
  }
  uiFlushFull();
}

static void drawSettings() {
  epd->EPD_Clear();
  uiTextCentered(6, "SETTINGS", 2);
  uiRect(0, 26, 200, 1);
  static const char* ROWS[6] = {"WI-FI", "SYNC NOW", "STORAGE", "IP ADDRESS", "HOW TO", "EXTRA"};
  for (int i = 0; i < 6; i++) uiRow(6, 32 + i * 27, 188, 25, ROWS[i], 2, sel == i);
  uiFlushFull();
}

static void drawExtra() {
  epd->EPD_Clear();
  uiTextCentered(6, "EXTRA", 2);
  uiRect(0, 26, 200, 1);
  uiRow(6, 40, 188, 36, "REDO TOUR",  2, sel == 0);
  uiRow(6, 84, 188, 36, "SYNC RATE",  2, sel == 1);
  uiRow(6, 128, 188, 36, "RESET",     2, sel == 2);
  uiTextCentered(174, "double-tap = back", 1);
  uiFlushFull();
}

static void drawSyncRate() {
  uint32_t cur = syncHours();
  epd->EPD_Clear();
  uiTextCentered(4, "AUTO-SYNC", 2);
  uiRect(0, 24, 200, 1);
  const char* labels[6] = {"Off", "1h", "2h", "4h", "8h", "24h"};
  for (int i = 0; i < 6; i++) {
    int x = (i % 2 == 0) ? 6 : 102;
    int y = 30 + (i / 2) * 32;
    String lab = labels[i];
    if (SYNC_OPTS[i] == cur) lab = "*" + lab;   /* the one in force */
    uiRow(x, y, 92, 28, lab, 2, sel == i);
  }
  uiTextCentered(132, "* = current", 1);
  uiTextCentered(148, "more often drains", 1);
  uiTextCentered(162, "the battery faster", 1);
  uiFillRect(0, 182, 200, 18, 0x00);
  uiTextCentered(187, "hold=pick  2tap=back", 1, 0xff);
  uiFlushFull();
}

static void drawFactory() {
  epd->EPD_Clear();
  if (factoryStage == 0) {
    uiTextCentered(4, "RESET", 2);
    uiRect(0, 24, 200, 1);
    uiText(4, 30, "Erases the whole", 1);
    uiText(4, 44, "card: every note,", 1);
    uiText(4, 58, "transcript, tag and", 1);
    uiText(4, 72, "the to-do list. Also", 1);
    uiText(4, 86, "Wi-Fi, your server", 1);
    uiText(4, 100, "address and settings.", 1);
    uiText(4, 118, "Cannot be undone.", 1);
    uiRow(6, 134, 92, 26, "CANCEL", 2, sel == 0);
    uiRow(102, 134, 92, 26, "ERASE", 2, sel == 1);
  } else {
    uiTextCentered(20, "ARE YOU", 2);
    uiTextCentered(44, "SURE?", 2);
    uiRect(0, 70, 200, 1);
    uiTextCentered(82, "Last chance.", 2);
    uiTextCentered(106, "Everything goes.", 1);
    uiRow(6, 134, 92, 26, "KEEP IT", 2, sel == 0);
    uiRow(102, 134, 92, 26, "ERASE", 2, sel == 1);
  }

  uiFillRect(0, 184, 200, 16, 0x00);
  uiTextCentered(188, "< back", 1, 0xff);
  uiFlushFull();
}

/* A clip whose transcript already exists doesn't need its audio on the card
   any more - the words are the point. Count what could go. */
static void reclaimableStats(int& count, uint64_t& bytes) {
  count = 0;
  bytes = 0;
  std::vector<String> bases;
  collectBases(bases);
  for (size_t i = 0; i < bases.size(); i++) {
    if (!noteHasAudio(bases[i])) continue;
    if (!SD_MMC.exists(notePath(bases[i], ".txt"))) continue;
    File w = SD_MMC.open(notePath(bases[i], ".wav"), "r");
    if (w) { count++; bytes += w.size(); w.close(); }
  }
}

static int freeTranscribedAudio() {
  std::vector<String> bases, doomed;
  collectBases(bases);
  for (size_t i = 0; i < bases.size(); i++)
    if (noteHasAudio(bases[i]) && SD_MMC.exists(notePath(bases[i], ".txt")))
      doomed.push_back(notePath(bases[i], ".wav"));
  for (auto& p : doomed) SD_MMC.remove(p);
  countRecordings();
  return doomed.size();
}

static void drawStorage() {
  uint64_t total = SD_MMC.totalBytes();
  uint64_t used = SD_MMC.usedBytes();
  uint64_t freeB = total > used ? total - used : 0;
  int rc; uint64_t rb;
  reclaimableStats(rc, rb);
  epd->EPD_Clear();
  uiTextCentered(8, "STORAGE", 2);
  uiRect(0, 26, 200, 1);
  uiText(16, 34, "Total:  " + String(total / (1024ULL * 1024ULL)) + " MB", 1);
  uiText(16, 50, "Used:   " + String(used / (1024ULL * 1024ULL)) + " MB", 1);
  uiText(16, 66, "Free:   " + String(freeB / (1024ULL * 1024ULL)) + " MB", 1);
  int pct = total ? (int)(used * 100 / total) : 0;
  uiRect(20, 86, 160, 12);
  uiFillRect(22, 88, (int)(156 * pct / 100), 8, 0x00);
  uiTextCentered(106, String(pct) + "% used", 1);
  uiRect(0, 122, 200, 1);
  if (rc > 0) {
    uiTextCentered(130, String(rc) + " clips already synced", 1);
    uiTextCentered(144, "audio frees " + String((uint32_t)(rb / (1024ULL * 1024ULL))) + " MB", 1);
    uiRow(6, 156, 188, 24, confirmFree ? "SURE? HOLD" : "FREE SPACE", 2, true);
  } else {
    uiTextCentered(140, "nothing to free yet", 1);
    uiTextCentered(158, "transcripts are kept", 1);
    uiTextCentered(170, "when audio is removed", 1);
  }
  uiFillRect(0, 184, 200, 16, 0x00);
  uiTextCentered(188, "< back", 1, 0xff);
  uiFlushFull();
}

/* Depth-first delete. The tree is shallow (/recordings, /www) but a card the
   user has been dropping files onto by hand can be anything, so recurse. */
static void wipeDir(const String& path) {
  File dir = SD_MMC.open(path);
  if (!dir) return;
  if (!dir.isDirectory()) { dir.close(); return; }
  std::vector<String> files, dirs;
  File f;
  while ((f = dir.openNextFile())) {
    String n = String(f.name());
    if (!n.startsWith("/")) n = (path == "/" ? "/" : path + "/") + n;
    if (f.isDirectory()) dirs.push_back(n); else files.push_back(n);
    f.close();
  }
  dir.close();
  for (auto& n : files) SD_MMC.remove(n);
  for (auto& n : dirs) { wipeDir(n); SD_MMC.rmdir(n); }
}

static void doFactoryReset() {
  playStop();
  epd->EPD_Clear();
  uiTextCentered(80, "ERASING", 2);
  uiTextCentered(110, "do not unplug", 1);
  uiFlushFull();
  wipeDir("/");
  SD_MMC.mkdir("/recordings");
  SD_MMC.mkdir("/www");
  netClearAll();
  epd->EPD_Clear();
  uiTextCentered(70, "ERASED", 2);
  uiTextCentered(104, "out of the box again", 1);
  uiTextCentered(124, "restarting...", 1);
  uiFlushFull();
  delay(2000);
  ESP.restart();
}

static void drawWifiScreen(const String& ssid) {
  epd->EPD_Clear();
  uiTextCentered(14, "WI-FI SETUP", 2);
  uiRect(0, 34, 200, 1);
  uiTextCentered(56, "join this hotspot", 1);
  uiTextCentered(80, ssid, 1);
  uiTextCentered(100, "key: record123", 1);
  uiTextCentered(128, WiFi.softAPIP().toString(), 2);
  uiTextCentered(156, "enter network + password", 1);
  uiFillRect(0, 184, 200, 16, 0x00);
  uiTextCentered(188, "< back", 1, 0xff);
  uiFlushFull();
}

static void drawIpScreen() {
  epd->EPD_Clear();
  uiTextCentered(14, "MY ADDRESS", 2);
  uiRect(0, 34, 200, 1);
  uiTextCentered(60, "type this in a browser", 1);
  uiTextCentered(92, WiFi.localIP().toString(), 2);
  uiTextCentered(130, "your notes, on the web", 1);
  uiFillRect(0, 184, 200, 16, 0x00);
  uiTextCentered(188, "< back", 1, 0xff);
  uiFlushFull();
}

static void drawViewTags() {
  epd->EPD_Clear();
  uiTextCentered(6, "VIEW NOTES", 2);
  uiRect(0, 26, 200, 1);
  std::vector<String> bases;
  collectBases(bases);
  for (int i = 0; i < 5; i++) {
    int y = 32 + i * 29;
    int count = 0;
    for (size_t b = 0; b < bases.size(); b++) if (tagOf(bases[b]) == TAGS[i]) count++;
    bool on = (sel == i);
    if (on) uiFillRect(6, y, 188, 27, 0x00); else uiRect(6, y, 188, 27);
    uiText(12, y + 6, String(TAGS[i]), 2, on ? 0xff : 0x00);
    uiText(168, y + 6, String(count), 2, on ? 0xff : 0x00);
  }
  uiFillRect(0, 182, 200, 18, 0x00);
  uiTextCentered(187, "hold=open  2tap=back", 1, 0xff);
  uiFlushFull();
}

static void drawViewList() {
  epd->EPD_Clear();
  uiTextCentered(6, viewTag, 2);
  uiRect(0, 26, 200, 1);
  if (noteList.empty()) uiTextCentered(96, "nothing here yet", 2);
  for (int i = 0; i < LIST_ROWS && listTop + i < (int)noteList.size(); i++) {
    int idx = listTop + i;
    int y = 32 + i * 29;
    String n = noteList[idx];
    if (!noteHasAudio(n)) n = "*" + n;    /* text-only, audio was freed */
    if ((int)n.length() > 15) n = n.substring(n.length() - 15);
    bool on = (idx == sel);
    if (on) uiFillRect(6, y, 188, 27, 0x00);
    uiText(10, y + 6, n, 2, on ? 0xff : 0x00);
  }
  uiFillRect(0, 182, 200, 18, 0x00);
  uiTextCentered(187, "hold=open  2tap=back", 1, 0xff);
  uiFlushFull();
}

#define NOTE_LINES 5

static void drawViewNote() {
  epd->EPD_Clear();
  const String base = noteList[noteRow];
  const bool hasAudio = noteHasAudio(base);
  String n = base;
  if ((int)n.length() > 15) n = n.substring(n.length() - 15);
  uiText(4, 4, n, 2);
  uiRect(0, 24, 200, 1);
  if (transcriptLines.empty()) {
    uiTextCentered(60, "not transcribed", 2);
  } else {
    int pages = (transcriptLines.size() + NOTE_LINES - 1) / NOTE_LINES;
    int start = transcriptPage * NOTE_LINES;
    for (int i = 0; i < NOTE_LINES && start + i < (int)transcriptLines.size(); i++)
      uiText(4, 30 + i * 18, transcriptLines[start + i], 2);
    if (pages > 1)
      uiTextCentered(122, String(transcriptPage + 1) + "/" + String(pages), 1);
  }
  uiRow(6, 132, 188, 24, !hasAudio ? "AUDIO FREED" : (playActive() ? "STOP" : "PLAY"), 2, sel == 0);
  uiRow(6, 158, 188, 24, confirmDelete ? "SURE? HOLD" : "DELETE", 2, sel == 1);
  uiFillRect(0, 184, 200, 16, 0x00);
  uiTextCentered(188, "holds=pages  2tap=back", 1, 0xff);
  uiFlushFull();
}

static void drawSyncScreen(int done, int total, const String& name) {
  epd->EPD_Clear();
  uiTextCentered(20, "SYNCING", 2);
  uiRect(0, 48, 200, 1);
  uiTextCentered(80, String(done) + " / " + String(total), 2);
  String shortName = name.length() > 24 ? "..." + name.substring(name.length() - 21) : name;
  uiTextCentered(110, shortName, 1);
  uiTextCentered(170, "tap to cancel", 1);
  uiFlushFull();
}

static void showError(const String& msg) {
  epd->EPD_Clear();
  uiTextCentered(60, "ERROR", 2);
  uiTextCentered(100, msg, 1);
  uiTextCentered(150, "press = continue", 1);
  uiFlushFull();
  while (true) {
    if (inputPoll() & BTN_ANY_DOWN) { delay(300); break; }
    delay(20);
  }
}

static bool startRecording() {
  if (!recBegin()) {
    /* Almost always PSRAM: the 3.8 MB buffer is the only big allocation. */
    showError("no record buffer");
    state = ST_MENU;
    drawMenu();
    return false;
  }
  state = ST_MAKE;
  drawMake();
  return true;
}

static void finishRecording() {
  /* A mis-press can end here with nothing captured - say nothing was saved
     rather than showing the square, which means "saved". */
  if (recSeconds() == 0) {
    recDiscard();
    state = ST_MENU;
    drawMenu();
    return;
  }
  drawMakeStopped();
  delay(700);            /* let the square be seen before the tag picker */
  lastSavedName = timestampName();
  if (!recSave("/recordings/" + lastSavedName)) {
    state = ST_HOME;
    drawHome();
    showError("save failed - SD?");
    return;
  }
  countRecordings();
  state = ST_TAG;
  drawTagScreen();
}

static void syncAll(bool autoRun) {
  String api = netGet("api");
  if (!api.length()) {
    if (!autoRun) showError("set API in Wi-Fi page");
    return;
  }
  State prev = state;
  state = ST_SYNC;
  syncCancel = false;
  drawSyncScreen(0, 0, autoRun ? "daily auto-sync" : "connecting wifi...");
  if (!staConnect(20000)) {
    state = prev;
    if (prev == ST_SETTINGS) drawSettings();
    else drawHome();
    if (!autoRun) showError("wifi failed");
    return;
  }
  std::vector<String> bases, pending;
  collectBases(bases);
  for (size_t i = 0; i < bases.size(); i++)
    if (noteHasAudio(bases[i]) && !SD_MMC.exists(notePath(bases[i], ".txt")))
      pending.push_back(bases[i]);
  int done = 0;
  for (auto& base : pending) {
    if (syncCancel) break;
    drawSyncScreen(done, pending.size(), base);
    String text;
    if (transcribeFile(notePath(base, ".wav"), text)) {
      File out = SD_MMC.open(notePath(base, ".txt"), "w");
      if (out) { out.print(text); out.close(); }
      done++;
    }
    delay(200);
  }
  /* Second half: push the notes to the owner's repo. Transcription and
     publishing are one press because they are one intention - "make what is on
     this device visible" - and whichever half is not configured is skipped
     rather than treated as a failure. */
  if (!syncCancel && syncConfigured()) {
    drawSyncScreen(pending.size(), pending.size(), "publishing...");
    std::vector<String> allBases;
    collectBases(allBases);
    std::vector<SyncNote> out;
    for (auto& b : allBases) {
      SyncNote n;
      n.base = b;
      n.tag  = tagOf(b);
      n.secs = 0;
      File tf = SD_MMC.open(notePath(b, ".txt"), "r");
      if (tf) { n.transcript = tf.readString(); tf.close(); }
      out.push_back(n);
    }
    todoLoad();
    std::vector<SyncTodo> tds;
    for (auto& t : todos) tds.push_back({t.text, t.done});
    String perr;
    if (!syncPublish(out, tds, perr) && !autoRun) {
      staDisconnect();
      state = prev;
      if (prev == ST_SETTINGS) drawSettings(); else drawHome();
      showError("publish: " + perr);
      return;
    }
  }

  time_t now = time(nullptr);
  if (now > 1600000000) netSetU64("lastSync", (uint64_t)now);
  staDisconnect();
  state = prev;
  if (prev == ST_SETTINGS) drawSettings();
  else drawHome();
  if (!syncCancel && done == 0 && !autoRun) {
    /* full refresh: a partial one here has no base image behind it, which on
       the real panel leaves ghosting rather than text */
    uiTextCentered(188, "up to date", 1);
    uiFlushFull();
    delay(1500);
    if (prev == ST_SETTINGS) drawSettings();
    else drawHome();
  }
}

static bool autoSyncTriedThisBoot = false;

static void maybeAutoSync() {
  uint32_t hrs = syncHours();
  if (hrs == 0) return;
  if (!netGet("ssid").length() || !netGet("api").length()) return;
  time_t now = time(nullptr);
  if (now < 1600000000) {
    /* Clock isn't set yet - only NTP does that, and only a sync reaches NTP.
       Without this the schedule could never start itself after a cold boot. */
    if (autoSyncTriedThisBoot) return;
    autoSyncTriedThisBoot = true;
  } else {
    uint64_t last = netGetU64("lastSync", 0);
    if (now - (time_t)last < (time_t)hrs * 3600) return;
  }
  syncReturnTo = ST_HOME;
  state = ST_HOME;
  syncAll(true);
}

static void deleteCurrentNote() {
  const String base = noteList[noteRow];
  SD_MMC.remove(notePath(base, ".wav"));
  SD_MMC.remove(notePath(base, ".txt"));
  SD_MMC.remove(notePath(base, ".tag"));
  countRecordings();
  refreshNoteList();
  noteRow = -1;
  confirmDelete = false;
  state = ST_VIEW_LIST;
  listTop = 0;
  drawViewList();
}

void setup() {
  Serial.begin(115200);
  delay(200);
  analogSetAttenuation(ADC_11db);
  pwr.VBAT_POWER_ON();
  pwr.POWEER_EPD_ON();
  i2c = I2cMasterBus::requestInstance(ESP32_I2C_SCL_PIN, ESP32_I2C_SDA_PIN, ESP32_I2C_DEV_NUM);
  inputBegin();
  epd = new epaper_driver_display(EPD_WIDTH, EPD_HEIGHT,
      {EPD_CS_PIN, EPD_DC_PIN, EPD_RST_PIN, EPD_BUSY_PIN, EPD_MOSI_PIN, EPD_SCK_PIN, EPD_SPI_NUM, EPD_WIDTH * EPD_HEIGHT / 8});
  epd->EPD_Init();
  uiBegin(epd);

  if (!SD_MMC.setPins(SDMMC_CLK_PIN, SDMMC_CMD_PIN, SDMMC_D0_PIN)) showError("sd pins rejected");
  if (!SD_MMC.begin("/sdcard", true)) showError("no SD card");
  SD_MMC.mkdir("/recordings");
  SD_MMC.mkdir("/www");
  netBegin();
  pwr.POWEER_Audio_ON();
  audioReady();
  countRecordings();
  /* No first_boot_done key means this card/device has never been set up -
     a fresh unit, or one that was just factory reset. Run the tour. */
  if (!netHasKey("first_boot_done")) {
    tourFromExtra = false;
    walkStep = 0;
    state = ST_WALKTHROUGH;
    drawTourStep(walkStep);
  } else {
    drawHome();
  }
  lastActivity = millis();
  if (state != ST_WALKTHROUGH) maybeAutoSync();
}

void loop() {
  const uint16_t ev = inputPoll();
  if (ev & BTN_ANY_DOWN) lastActivity = millis();

  /* Holding the bottom button means "off" from anywhere. Recording is the one
     exception: losing a note because a thumb lingered would be the worst
     possible failure, so a recording has to be stopped deliberately first. */
  if ((ev & BTN_POWER_OFF) && state != ST_MAKE && state != ST_WALKTHROUGH) {
    sleepNow();
    return;
  }

  switch (state) {
    case ST_HOME:
      if (ev & (BTN_TOP_TAP | BTN_BOT_TAP | BTN_TOP_HOLD)) {
        if (soundOn()) beep();
        selReset(4);
        state = ST_MENU;
        drawMenu();
      }
      if (millis() - lastActivity > 30000 && !inputAnyHeld()) sleepNow();
      if (millis() - lastAutoCheck > 300000UL) { lastAutoCheck = millis(); maybeAutoSync(); }
      break;

    case ST_MENU:
      /* The bottom button records from anywhere in the menu - the point of the
         device is that catching a thought is one press, not four. */
      if (ev & BTN_BOT_TAP) { if (soundOn()) beep(); startRecording(); break; }
      if (ev & BTN_TOP_TAP)    { selNext(); drawMenu(); }
      if (ev & BTN_TOP_DOUBLE) { state = ST_HOME; drawHome(); break; }
      if (ev & BTN_TOP_HOLD) {
        if (soundOn()) beep();
        switch (sel) {
          case 0: viewTag = ""; selReset(5); state = ST_VIEW_TAGS; drawViewTags(); break;
          case 1: startRecording(); break;
          case 2: todoLoad(); todoTop = 0; selReset(todos.size()); state = ST_TODO; drawTodo(); break;
          case 3: selReset(6); state = ST_SETTINGS; drawSettings(); break;
        }
      }
      if (millis() - lastActivity > 60000 && !inputAnyHeld()) sleepNow();
      break;

    case ST_MAKE:
      /* Nothing to repaint while it runs - the screen was drawn when
         recording started and is not touched again until it stops. */
      if (recSeconds() >= MAKE_MAX_SECONDS) { finishRecording(); break; }
      /* The same button starts and stops, so a note costs one press each end. */
      if (ev & (BTN_BOT_TAP | BTN_TOP_HOLD | BTN_TOP_DOUBLE)) finishRecording();
      break;

    case ST_TODO:
      if (ev & BTN_TOP_DOUBLE) { selReset(4); state = ST_MENU; drawMenu(); break; }
      if (todos.empty()) break;
      if (ev & BTN_TOP_TAP)    { selNext(); selEnsureVisible(todoTop, LIST_ROWS); drawTodo(); }
      if (ev & BTN_TOP_REPEAT) { selPrev(); selEnsureVisible(todoTop, LIST_ROWS); drawTodo(); }
      if (ev & BTN_BOT_REPEAT) { selNext(); selEnsureVisible(todoTop, LIST_ROWS); drawTodo(); }
      if (ev & BTN_TOP_HOLD) {
        todos[sel].done = !todos[sel].done;
        todoSave();
        if (soundOn()) beep();
        drawTodo();
      }
      break;

    case ST_TAG:
      if (ev & BTN_TOP_TAP)  { selNext(); drawTagScreen(); }
      if (ev & BTN_TOP_HOLD) {
        if (sel < 5) saveTag(TAGS[sel]);
        if (soundOn()) beep();
        state = ST_HOME;
        drawHome();
      }
      if (ev & BTN_TOP_DOUBLE) { state = ST_HOME; drawHome(); }
      break;

    case ST_SETTINGS:
      if (ev & BTN_TOP_DOUBLE) { selReset(4); state = ST_MENU; drawMenu(); break; }
      if (ev & BTN_TOP_TAP)    { selNext(); drawSettings(); }
      if (ev & BTN_TOP_HOLD) {
        if (soundOn()) beep();
        switch (sel) {
          case 0: state = ST_SET_WIFI; drawWifiScreen(portalStart()); break;
          case 1: syncAll(false); break;
          case 2: confirmFree = false; state = ST_STORAGE; drawStorage(); break;
          case 3:
            drawSyncScreen(0, 0, "connecting wifi...");
            if (staConnect(20000)) { serverStartSta(); state = ST_SET_IP; drawIpScreen(); }
            else { showError("wifi failed"); drawSettings(); }
            break;
          case 4: state = ST_HOWTO; drawHowTo(); break;
          case 5: selReset(3); state = ST_EXTRA; drawExtra(); break;
        }
      }
      break;

    case ST_HOWTO:
      if (ev & (BTN_TOP_TAP | BTN_TOP_HOLD | BTN_TOP_DOUBLE)) {
        selReset(6); state = ST_SETTINGS; drawSettings();
      }
      break;

    case ST_EXTRA:
      if (ev & BTN_TOP_DOUBLE) { selReset(6); state = ST_SETTINGS; drawSettings(); break; }
      if (ev & BTN_TOP_TAP)    { selNext(); drawExtra(); }
      if (ev & BTN_TOP_HOLD) {
        if (soundOn()) beep();
        switch (sel) {
          case 0: tourFromExtra = true; walkStep = 0; sel = 0; state = ST_WALKTHROUGH; drawTourStep(walkStep); break;
          case 1: selReset(6); state = ST_SYNCRATE; drawSyncRate(); break;
          case 2: factoryStage = 0; selReset(2); state = ST_FACTORY; drawFactory(); break;
        }
      }
      break;

    case ST_SYNCRATE:
      if (ev & BTN_TOP_DOUBLE) { selReset(3); state = ST_EXTRA; drawExtra(); break; }
      if (ev & BTN_TOP_TAP)    { selNext(); drawSyncRate(); }
      if (ev & BTN_TOP_HOLD) {
        netSetU32("syncHrs", SYNC_OPTS[sel]);
        if (soundOn()) beep();
        drawSyncRate();
      }
      break;

    case ST_FACTORY:
      if (ev & BTN_TOP_DOUBLE) { selReset(3); state = ST_EXTRA; drawExtra(); break; }
      if (ev & BTN_TOP_TAP)    { selNext(); drawFactory(); }
      if (ev & BTN_TOP_HOLD) {
        if (sel == 0) { selReset(3); state = ST_EXTRA; drawExtra(); }
        else if (factoryStage == 0) { factoryStage = 1; selReset(2); drawFactory(); }
        else doFactoryReset();
      }
      break;

    case ST_WALKTHROUGH:
      /* Gated: only a deliberate hold moves on, so no screen gets skipped. */
      if (walkStep == TOUR_SOUND) {
        if (ev & BTN_TOP_TAP) { sel = sel ? 0 : 1; drawTourStep(walkStep); }
        if (ev & BTN_TOP_HOLD) {
          netSet("sound", sel == 0 ? "1" : "0");
          if (soundOn()) beep();
          walkStep++; sel = 0;
          drawTourStep(walkStep);
        }
      } else {
        if ((ev & BTN_TOP_DOUBLE) && walkStep > 0) {
          walkStep--; if (soundOn()) beep(); drawTourStep(walkStep);
        } else if (ev & BTN_TOP_HOLD) {
          walkStep++;
          if (walkStep >= TOUR_STEPS) {
            netSetBool("first_boot_done", true);
            if (tourFromExtra) { tourFromExtra = false; selReset(3); state = ST_EXTRA; drawExtra(); }
            else { state = ST_HOME; drawHome(); }
          } else {
            if (soundOn()) beep();
            drawTourStep(walkStep);
          }
        }
      }
      break;

    case ST_SET_WIFI:
      portalPoll();
      if (ev & BTN_TOP_DOUBLE) { portalStop(); selReset(6); state = ST_SETTINGS; drawSettings(); }
      break;

    case ST_SYNC:
      if (ev & (BTN_TOP_TAP | BTN_TOP_DOUBLE)) syncCancel = true;
      break;

    case ST_STORAGE:
      if (ev & BTN_TOP_DOUBLE) { confirmFree = false; selReset(6); state = ST_SETTINGS; drawSettings(); break; }
      if (ev & BTN_TOP_HOLD) {
        if (confirmFree) { freeTranscribedAudio(); confirmFree = false; if (soundOn()) beep(); }
        else confirmFree = true;
        drawStorage();
      }
      break;

    case ST_SET_IP:
      portalPoll();
      if (ev & BTN_TOP_DOUBLE) { portalStop(); staDisconnect(); selReset(6); state = ST_SETTINGS; drawSettings(); }
      break;

    case ST_VIEW_TAGS:
      if (ev & BTN_TOP_DOUBLE) { selReset(4); state = ST_MENU; drawMenu(); break; }
      if (ev & BTN_TOP_TAP)    { selNext(); drawViewTags(); }
      if (ev & BTN_TOP_HOLD) {
        if (soundOn()) beep();
        viewTag = TAGS[sel];
        refreshNoteList();
        listTop = 0; noteRow = -1;
        selReset(noteList.size());
        state = ST_VIEW_LIST;
        drawViewList();
      }
      break;

    case ST_VIEW_LIST:
      if (ev & BTN_TOP_DOUBLE) { selReset(5); state = ST_VIEW_TAGS; drawViewTags(); break; }
      if (noteList.empty()) break;
      if (ev & BTN_TOP_TAP)    { selNext(); selEnsureVisible(listTop, LIST_ROWS); drawViewList(); }
      if (ev & BTN_TOP_REPEAT) { selPrev(); selEnsureVisible(listTop, LIST_ROWS); drawViewList(); }
      if (ev & BTN_BOT_REPEAT) { selNext(); selEnsureVisible(listTop, LIST_ROWS); drawViewList(); }
      if (ev & BTN_TOP_HOLD) {
        if (soundOn()) beep();
        noteRow = sel;
        confirmDelete = false;
        transcriptPage = 0;
        loadTranscript(noteList[noteRow]);
        selReset(2);
        state = ST_VIEW_NOTE;
        drawViewNote();
      }
      break;

    case ST_VIEW_NOTE:
      playPoll();
      if (ev & BTN_TOP_DOUBLE) {
        playStop();
        selReset(noteList.size());
        sel = noteRow < 0 ? 0 : noteRow;
        state = ST_VIEW_LIST;
        drawViewList();
        break;
      }
      /* Holds page the transcript here - top back, bottom forward - because
         the two buttons on this screen are chosen by tapping, not holding. */
      if (ev & BTN_TOP_REPEAT) {
        if (transcriptPage > 0) { transcriptPage--; drawViewNote(); }
      }
      if (ev & BTN_BOT_REPEAT) {
        int pages = ((int)transcriptLines.size() + NOTE_LINES - 1) / NOTE_LINES;
        if (transcriptPage < pages - 1) { transcriptPage++; drawViewNote(); }
      }
      if (ev & BTN_TOP_TAP) { selNext(); confirmDelete = false; drawViewNote(); }
      if (ev & BTN_TOP_HOLD) {
        if (sel == 0) {
          if (playActive()) { playStop(); if (soundOn()) beep(); }
          else if (noteHasAudio(noteList[noteRow]) && playFile(notePath(noteList[noteRow], ".wav"))) {
            if (soundOn()) beep();
          }
          drawViewNote();
        } else {
          if (confirmDelete) deleteCurrentNote();
          else { confirmDelete = true; drawViewNote(); }
        }
      }
      break;
  }
  delay(10);
}
