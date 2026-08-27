#include <Arduino.h>
#include <WiFi.h>
#include <SD_MMC.h>
#include <esp_sleep.h>
#include <time.h>
#include <vector>
#include <math.h>
#include "user_config.h"
#include "i2c_bsp.h"
#include "ft6336_bsp.h"
#include "board_power_bsp.h"
#include "epaper_driver_bsp.h"
#include "pala_ui.h"
#include "pala_record.h"
#include "pala_net.h"

#define BAT_ADC_PIN 4
#define BAT_EMPTY_MV 3300
#define BAT_FULL_MV 4200

static board_power_bsp_t pwr(EPD_PWR_PIN, Audio_PWR_PIN, VBAT_PWR_PIN);
static I2cMasterBus* i2c = nullptr;
static I2cFt6336Dev* touch = nullptr;
static epaper_driver_display* epd = nullptr;

enum State { ST_HOME, ST_MENU, ST_MAKE, ST_TAG, ST_TODO, ST_SETTINGS, ST_SET_WIFI, ST_SYNC, ST_STORAGE, ST_SET_IP, ST_VIEW_TAGS, ST_VIEW_LIST, ST_VIEW_NOTE, ST_HOWTO, ST_WALKTHROUGH, ST_EXTRA, ST_SYNCRATE, ST_FACTORY };
static State state = ST_HOME;
static State syncReturnTo = ST_HOME;

static uint32_t touchDownAt = 0;
static int downX = -1, downY = -1, lastY = -1;
static bool longFired = false;
static bool pressed = false;
static uint32_t lastActivity = 0;
static uint32_t lastAutoCheck = 0;

/* Recording gesture: press-and-hold records and releasing saves, but a quick
   tap latches so a long note doesn't mean holding a finger down for 2 minutes. */
static bool makeArmed = false;    /* finger still down from the menu press */
static bool makeLatched = false;  /* tapped instead of held - runs until stopped */
static uint32_t makeDownAt = 0;

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

static int wave[16] = {2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2};
static int waveIdx = 0;

static void sleepNow() {
  uiFillRect(0, 0, 200, 200, 0xff);
  uiTextCentered(90, "mono note", 2);
  uiFlushFull();
  delay(300);
  pwr.POWEER_Audio_OFF();
  pwr.POWEER_EPD_OFF();
  esp_sleep_enable_ext1_wakeup(
      (1ULL << GPIO_NUM_0) | (1ULL << GPIO_NUM_18) | (1ULL << EPD_TP_INT_PIN), ESP_EXT1_WAKEUP_ANY_LOW);
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

static void drawBattery(int x, int y) {
  int pct = batteryPct();
  uiRect(x, y, 26, 13);
  uiFillRect(x + 26, y + 3, 2, 7);
  if (pct < 0) {
    uiText(x + 4, y + 3, "?", 1);
    return;
  }
  int w = 22 * pct / 100;
  if (w < 2) w = 2;
  uiFillRect(x + 2, y + 2, w, 9);
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

static String tagFilePath(const String& wavFile) {
  return "/recordings/" + wavFile.substring(1, wavFile.length() - 4) + ".tag";
}

static String tagOf(const String& wavName) {
  String t = tagFilePath(wavName);
  if (!SD_MMC.exists(t)) return "";
  File f = SD_MMC.open(t, "r");
  String v = f.readStringUntil('\n');
  f.close();
  v.trim();
  return v;
}

static void saveTag(const char* tag) {
  File f = SD_MMC.open(tagFilePath(lastSavedName), "w");
  if (f) { f.println(tag); f.close(); }
}

static void countRecordings() {
  recCount = 0;
  File dir = SD_MMC.open("/recordings");
  File f;
  while ((f = dir.openNextFile())) {
    String n = String(f.name());
    if (!n.startsWith("/")) n = "/" + n;
    if (n.endsWith(".wav")) recCount++;
    f.close();
  }
}

static void refreshNoteList() {
  noteList.clear();
  File dir = SD_MMC.open("/recordings");
  File f;
  std::vector<String> all;
  while ((f = dir.openNextFile())) {
    String n = String(f.name());
    if (!n.startsWith("/")) n = "/" + n;
    if (n.endsWith(".wav") && tagOf(n) == viewTag) all.push_back(n);
    f.close();
  }
  for (int i = all.size() - 1; i >= 0; i--) noteList.push_back(all[i]);
}

static void loadTranscript(const String& wavName) {
  transcriptLines.clear();
  transcriptPage = 0;
  String t = "/recordings/" + wavName.substring(1, wavName.length() - 4) + ".txt";
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
  drawBattery(158, 12);
  uiTextCentered(56, "mono", 2);
  uiTextCentered(76, "note mini", 1);
  uiRect(80, 96, 40, 2);
  uiTextCentered(116, "TAP TO UNLOCK", 1);
  uiTextCentered(186, "HOLD = BACK", 1);
  uiFlushFull();
}

static void drawMenu() {
  epd->EPD_Clear();
  uiTextCentered(10, "mono note mini", 1);
  uiRect(0, 26, 200, 1);
  uiRect(10, 36, 180, 36);  uiTextCentered(48, "VIEW NOTES", 1);
  uiRect(10, 76, 180, 36);  uiTextCentered(88, "MAKE NOTE", 1);
  uiRect(10, 116, 180, 36); uiTextCentered(128, "TO-DO", 1);
  uiRect(10, 156, 180, 36); uiTextCentered(168, "SETTINGS", 1);
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
  uiTextCentered(10, "TO-DO", 2);
  uiRect(0, 28, 200, 1);
  if (todos.empty()) uiTextCentered(100, "no jobs - add via web", 1);
  for (int i = 0; i < 6 && todoTop + i < (int)todos.size(); i++) {
    int idx = todoTop + i;
    int y = 36 + i * 24;
    bool sel = false;
    uiRect(8, y + 4, 14, 14);
    if (todos[idx].done) {
      uiFillRect(10, y + 6, 10, 10, 0x00);
      uiFillRect(11, y + 11, 8, 2, 0xff);
    }
    String t = todos[idx].text;
    if ((int)t.length() > 24) t = t.substring(0, 24);
    uiText(30, y + 6, t, 1);
  }
  uiFillRect(0, 184, 200, 16, 0x00);
  uiTextCentered(188, "< back   (tap = check off)", 1, 0xff);
  uiFlushFull();
}

#define MAKE_MAX_SECONDS 120

/* Timer, meter and hint all live in the partial-refresh region so the screen
   doesn't full-flash once a second while recording. */
static void paintMakeBody() {
  uint32_t s = recSeconds();
  String t = String(s / 60) + ":" + String(s % 60 < 10 ? "0" : "") + String(s % 60);
  uiFillRect(10, 44, 180, 60, 0x00);
  uiTextCentered(66, t, 3, 0xff);

  uiFillRect(0, 118, 200, 44, 0xff);
  for (int i = 0; i < 16; i++) {
    int h = wave[i] * 6 / 10 + 2;
    uiFillRect(24 + i * 10, 150 - h / 2, 6, h, 0x00);
  }

  uiFillRect(0, 164, 200, 24, 0xff);
  uiTextCentered(166, makeLatched ? "tap timer to stop" : "release to stop", 1);
  uiTextCentered(178, String(MAKE_MAX_SECONDS - (int)s) + "s left", 1);
}

static void drawMake() {
  epd->EPD_Clear();
  uiTextCentered(14, "MAKE NOTE", 1);
  uiRect(0, 30, 200, 1);
  paintMakeBody();
  uiFlushFull();
  uiFlushPartialPrepare();
}

static void updateMake() {
  paintMakeBody();
  uiFlushPartial();
}

static void drawTagScreen() {
  epd->EPD_Clear();
  uiTextCentered(8, "SORT IT", 2);
  const char* labels[5] = {"Work", "Projects", "Ideas", "Quotes", "Random"};
  uiRect(10, 36, 86, 34);   uiTextCenteredIn(10, 86, 47, labels[0]);
  uiRect(104, 36, 86, 34);  uiTextCenteredIn(104, 86, 47, labels[1]);
  uiRect(10, 78, 86, 34);   uiTextCenteredIn(10, 86, 89, labels[2]);
  uiRect(104, 78, 86, 34);  uiTextCenteredIn(104, 86, 89, labels[3]);
  uiRect(10, 120, 180, 34); uiTextCenteredIn(10, 180, 131, labels[4]);
  uiTextCentered(170, "tap a tag to file it", 1);
  uiTextCentered(186, "tap here to skip", 1);
  uiFlushFull();
}

static void drawHowTo() {
  epd->EPD_Clear();
  uiTextCentered(8, "HOW TO USE", 1);
  uiRect(0, 20, 200, 1);
  uiText(4, 26, "1 Tap home to unlock", 1);
  uiText(4, 40, "2 MAKE: hold to record,", 1);
  uiText(4, 52, "  release saves. Or tap", 1);
  uiText(4, 64, "  once, tap timer to end", 1);
  uiText(4, 78, "3 VIEW: tag > note > PLAY", 1);
  uiText(4, 92, "4 Hold 0.9s = back", 1);
  uiText(4, 106, "5 Sync now needs Wi-Fi", 1);
  uiText(4, 120, "6 Extra: redo this tour", 1);
  uiRect(0, 134, 200, 1);
  uiTextCentered(140, "CREDITS", 1);
  uiTextCentered(154, "Made by Atreya Patil", 1);
  uiTextCentered(168, "insp. PALA NOTE", 1);
  uiTextCentered(182, "(c) 2026", 1);
  uiFlushFull();
}

/* ---- guided tour -------------------------------------------------------
   Every screen is gated behind the NEXT button: taps anywhere else are
   ignored, so the tour can't be skipped by stabbing at the glass. Step
   TOUR_SOUND is the one exception - it demands an ON/OFF choice instead. */

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
      lines[5] = "feature. Tap NEXT to";
      lines[6] = "walk through it.";
      break;
    case 1:
      title = "GESTURES";
      lines[0] = "Three moves, everywhere:";
      lines[1] = "";
      lines[2] = "TAP        choose";
      lines[3] = "HOLD 0.9s  go back";
      lines[4] = "SWIPE      scroll";
      break;
    case 2:
      title = "HOME & SLEEP";
      lines[0] = "Tap the home screen to";
      lines[1] = "unlock it.";
      lines[2] = "";
      lines[3] = "It sleeps by itself when";
      lines[4] = "idle. A touch or either";
      lines[5] = "button wakes it again.";
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
      lines[0] = "HOLD MAKE NOTE to record,";
      lines[1] = "let go and it saves.";
      lines[2] = "";
      lines[3] = "Or tap it once and it";
      lines[4] = "keeps going until you";
      lines[5] = "tap the timer.";
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
      lines[4] = "Swipe to scroll,";
      lines[5] = "tap a note to open it.";
      break;
    case 7:
      title = "PLAY & READ";
      lines[0] = "PLAY hears the note on";
      lines[1] = "the speaker.";
      lines[2] = "";
      lines[3] = "Swipe to page through";
      lines[4] = "the transcript.";
      lines[5] = "";
      lines[6] = "DELETE asks twice.";
      break;
    case 8:
      title = "TO-DO";
      lines[0] = "A checklist kept on the";
      lines[1] = "card as plain text.";
      lines[2] = "";
      lines[3] = "Tap a row to check it";
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
    uiTextCentered(30, "A soft tick on each tap.", 1);
    uiTextCentered(48, soundOn() ? "NOW: ON" : "NOW: OFF", 1);
    uiRect(10, 66, 86, 34);  uiTextCenteredIn(10, 86, 79, "ON", 2);
    uiRect(104, 66, 86, 34); uiTextCenteredIn(104, 86, 79, "OFF", 2);
    uiTextCentered(112, "Pick one to carry on.", 1);
  } else {
    for (int i = 0; i < 7; i++)
      if (lines[i] && lines[i][0]) uiText(4, 26 + i * 14, lines[i], 1);
  }

  uiTextCentered(140, "STEP " + String(step + 1) + " / " + String(TOUR_STEPS), 1);

  if (step != TOUR_SOUND) {
    if (step > 0) { uiRect(6, 156, 52, 26); uiTextCenteredIn(6, 52, 165, "BACK"); }
    uiFillRect(64, 156, 130, 26, 0x00);
    int nx = 64 + (130 - uiTextWidth(step == TOUR_STEPS - 1 ? "FINISH" : "NEXT >", 1)) / 2;
    uiText(nx, 165, step == TOUR_STEPS - 1 ? "FINISH" : "NEXT >", 1, 0xff);
  }
  uiFlushFull();
}

static void drawSettings() {
  epd->EPD_Clear();
  uiTextCentered(8, "SETTINGS", 2);
  uiRect(0, 26, 200, 1);
  const char* rows[6] = {"1  Wi-Fi", "2  Sync now", "3  Storage",
                         "4  IP", "5  How to & Credits", "6  Extra"};
  for (int i = 0; i < 6; i++) {
    int y = 32 + i * 25;
    uiRect(10, y, 180, 23);
    uiText(18, y + 8, rows[i], 1);
  }
  uiFillRect(0, 184, 200, 16, 0x00);
  uiTextCentered(188, "< back", 1, 0xff);
  uiFlushFull();
}

static void drawExtra() {
  epd->EPD_Clear();
  uiTextCentered(8, "EXTRA", 2);
  uiRect(0, 26, 200, 1);
  uiRect(10, 36, 180, 36);
  uiText(18, 48, "1  Redo tutorial", 1);
  uiText(18, 60, "   see every feature", 1);
  uiRect(10, 80, 180, 36);
  uiText(18, 92, "2  Auto-sync rate", 1);
  uiText(18, 104, "   now: " + syncRateLabel(), 1);
  uiRect(10, 124, 180, 36);
  uiText(18, 136, "3  Factory reset", 1);
  uiText(18, 148, "   erases everything", 1);
  uiFillRect(0, 184, 200, 16, 0x00);
  uiTextCentered(188, "< back", 1, 0xff);
  uiFlushFull();
}

static void drawSyncRate() {
  uint32_t cur = syncHours();
  epd->EPD_Clear();
  uiTextCentered(6, "AUTO-SYNC", 2);
  uiTextCentered(28, "now: " + syncRateLabel(), 1);
  uiRect(0, 40, 200, 1);
  const char* labels[6] = {"Off", "1h", "2h", "4h", "8h", "24h"};
  for (int i = 0; i < 6; i++) {
    int x = (i % 2 == 0) ? 10 : 104;
    int y = 46 + (i / 2) * 34;
    if (SYNC_OPTS[i] == cur) {
      uiFillRect(x, y, 86, 30, 0x00);
      int tx = x + (86 - uiTextWidth(labels[i], 1)) / 2;
      uiText(tx, y + 12, labels[i], 1, 0xff);
    } else {
      uiRect(x, y, 86, 30);
      int tx = x + (86 - uiTextWidth(labels[i], 1)) / 2;
      uiText(tx, y + 12, labels[i], 1);
    }
  }
  uiTextCentered(154, "Syncing more often", 1);
  uiTextCentered(168, "drains the battery faster", 1);
  uiFillRect(0, 184, 200, 16, 0x00);
  uiTextCentered(188, "< back", 1, 0xff);
  uiFlushFull();
}

static void drawFactory() {
  epd->EPD_Clear();
  if (factoryStage == 0) {
    uiTextCentered(8, "FACTORY RESET", 1);
    uiRect(0, 20, 200, 1);
    uiText(4, 28, "Erases the whole card:", 1);
    uiText(4, 44, "every note, transcript,", 1);
    uiText(4, 56, "tag and the to-do list.", 1);
    uiText(4, 72, "Also clears Wi-Fi, your", 1);
    uiText(4, 84, "server address, sound", 1);
    uiText(4, 96, "and the sync rate.", 1);
    uiText(4, 112, "This cannot be undone.", 1);
    uiRect(10, 132, 86, 30);  uiTextCenteredIn(10, 86, 144, "CANCEL");
    uiRect(104, 132, 86, 30); uiTextCenteredIn(104, 86, 144, "ERASE");
  } else {
    uiTextCentered(24, "ARE YOU", 2);
    uiTextCentered(46, "SURE?", 2);
    uiRect(0, 72, 200, 1);
    uiTextCentered(84, "Last chance.", 1);
    uiTextCentered(100, "Everything on the card", 1);
    uiTextCentered(112, "goes, out of the box.", 1);
    uiRect(10, 132, 86, 30);  uiTextCenteredIn(10, 86, 144, "KEEP IT");
    uiFillRect(104, 132, 86, 30, 0x00);
    int tx = 104 + (86 - uiTextWidth("ERASE ALL", 1)) / 2;
    uiText(tx, 144, "ERASE ALL", 1, 0xff);
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
  File dir = SD_MMC.open("/recordings");
  File f;
  while ((f = dir.openNextFile())) {
    String n = String(f.name());
    if (!n.startsWith("/")) n = "/" + n;
    if (n.endsWith(".wav")) {
      String txt = "/recordings" + n.substring(0, n.length() - 4) + ".txt";
      if (SD_MMC.exists(txt)) { count++; bytes += f.size(); }
    }
    f.close();
  }
  dir.close();
}

static int freeTranscribedAudio() {
  std::vector<String> doomed;
  File dir = SD_MMC.open("/recordings");
  File f;
  while ((f = dir.openNextFile())) {
    String n = String(f.name());
    if (!n.startsWith("/")) n = "/" + n;
    if (n.endsWith(".wav")) {
      String txt = "/recordings" + n.substring(0, n.length() - 4) + ".txt";
      if (SD_MMC.exists(txt)) doomed.push_back("/recordings" + n);
    }
    f.close();
  }
  dir.close();
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
    if (confirmFree) {
      uiFillRect(10, 158, 180, 22, 0x00);
      uiTextCenteredIn(10, 180, 165, "TAP AGAIN TO FREE", 1, 0xff);
    } else {
      uiRect(10, 158, 180, 22);
      uiTextCenteredIn(10, 180, 165, "FREE SPACE");
    }
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
  uiTextCentered(10, "VIEW NOTES", 2);
  uiRect(0, 28, 200, 1);
  for (int i = 0; i < 5; i++) {
    int y = 38 + i * 27;
    int count = 0;
    File dir = SD_MMC.open("/recordings");
    File f;
    while ((f = dir.openNextFile())) {
      String n = String(f.name());
      if (!n.startsWith("/")) n = "/" + n;
      if (n.endsWith(".wav") && tagOf(n) == TAGS[i]) count++;
      f.close();
    }
    uiRect(10, y, 180, 23);
    uiText(20, y + 7, String(TAGS[i]), 1);
    String c = String(count);
    uiText(170, y + 7, c, 1);
  }
  uiFillRect(0, 184, 200, 16, 0x00);
  uiTextCentered(188, "< back", 1, 0xff);
  uiFlushFull();
}

static void drawViewList() {
  epd->EPD_Clear();
  uiTextCentered(10, viewTag, 2);
  uiRect(0, 28, 200, 1);
  if (noteList.empty()) uiTextCentered(100, "nothing here yet", 1);
  for (int i = 0; i < 6 && listTop + i < (int)noteList.size(); i++) {
    int idx = listTop + i;
    String n = noteList[idx].substring(1, noteList[idx].length() - 4);
    if ((int)n.length() > 22) n = n.substring(0, 22);
    if (idx == noteRow) {
      uiFillRect(4, 34 + i * 24, 192, 24, 0x00);
      uiText(8, 40 + i * 24, n, 1, 0xff);
    } else {
      uiText(8, 40 + i * 24, n, 1);
    }
  }
  uiFillRect(0, 184, 200, 16, 0x00);
  uiTextCentered(188, "< back   (swipe to scroll)", 1, 0xff);
  uiFlushFull();
}

static void drawViewNote() {
  epd->EPD_Clear();
  String n = noteList[noteRow].substring(1, noteList[noteRow].length() - 4);
  if ((int)n.length() > 24) n = n.substring(0, 24);
  uiText(4, 6, n, 1);
  uiRect(0, 20, 200, 1);
  if (transcriptLines.empty()) {
    uiTextCentered(80, "not transcribed yet", 1);
  } else {
    int pages = (transcriptLines.size() + 5) / 6;
    int start = transcriptPage * 6;
    for (int i = 0; i < 6 && start + i < (int)transcriptLines.size(); i++)
      uiText(4, 26 + i * 14, transcriptLines[start + i], 1);
    if (pages > 1) uiTextCentered(122, String(transcriptPage + 1) + "/" + String(pages), 1);
  }
  uiRect(10, 134, 180, 22);
  uiTextCentered(139, playActive() ? "STOP" : "PLAY", 1);
  uiRect(10, 160, 180, 22);
  uiTextCentered(165, confirmDelete ? "TAP AGAIN TO DELETE" : "DELETE", 1);
  uiFillRect(0, 184, 200, 16, 0x00);
  uiTextCentered(188, "< back  (swipe = pages)", 1, 0xff);
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
  uiTextCentered(150, "tap = continue", 1);
  uiFlushFull();
  while (true) {
    uint16_t x, y;
    if (touch->GetTouchPoint(&x, &y)) { delay(300); break; }
    delay(20);
  }
}

static bool startRecording() {
  if (!recBegin()) {
    showError("no record buffer");
    return false;
  }
  for (int i = 0; i < 16; i++) wave[i] = 2;
  waveIdx = 0;
  makeArmed = true;      /* finger is still down - hold mode until proven otherwise */
  makeLatched = false;
  makeDownAt = millis();
  state = ST_MAKE;
  drawMake();
  return true;
}

static void finishRecording() {
  makeArmed = false;
  makeLatched = false;
  /* A stab at the menu row can end here with nothing captured. */
  if (recSeconds() == 0) {
    recDiscard();
    state = ST_MENU;
    drawMenu();
    return;
  }
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
  std::vector<String> wavs;
  File dir = SD_MMC.open("/recordings");
  File f;
  while ((f = dir.openNextFile())) {
    String n = String(f.name());
    if (!n.startsWith("/")) n = "/" + n;
    if (n.endsWith(".wav")) {
      String txt = n.substring(0, n.length() - 4) + ".txt";
      if (!SD_MMC.exists("/recordings" + txt)) wavs.push_back(n);
    }
    f.close();
  }
  int done = 0;
  for (auto& n : wavs) {
    if (syncCancel) break;
    drawSyncScreen(done, wavs.size(), n);
    String text;
    if (transcribeFile("/recordings" + n, text)) {
      String txt = "/recordings/" + n.substring(1, n.length() - 4) + ".txt";
      File out = SD_MMC.open(txt, "w");
      if (out) { out.print(text); out.close(); }
      done++;
    }
    delay(200);
  }
  time_t now = time(nullptr);
  if (now > 1600000000) netSetU64("lastSync", (uint64_t)now);
  staDisconnect();
  state = prev;
  if (prev == ST_SETTINGS) drawSettings();
  else drawHome();
  if (!syncCancel && done == 0 && !autoRun) {
    uiTextCentered(188, "up to date", 1);
    uiFlushPartial();
    delay(2000);
    if (prev == ST_SETTINGS) drawSettings();
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

static void pollTouch(bool& tap, bool& longPress, bool& releaseEvent, int& tx, int& ty, int& swipe,
                      bool& downEvent) {
  tap = false; longPress = false; releaseEvent = false; swipe = 0; downEvent = false;
  uint16_t x, y;
  bool down = touch->GetTouchPoint(&x, &y);
  if (down && !pressed) {
    pressed = true;
    touchDownAt = millis();
    downX = x; downY = y; lastY = y;
    longFired = false;
    lastActivity = millis();
    downEvent = true;
    tx = downX; ty = downY;
  } else if (down && pressed) {
    lastActivity = millis();
    if (!longFired && millis() - touchDownAt > 900) {
      longFired = true;
      longPress = true;
    }
    if (abs(y - lastY) > 55) {
      swipe = (y < lastY) ? -1 : 1;
      lastY = y;
    }
  } else if (!down && pressed) {
    pressed = false;
    releaseEvent = true;
    uint32_t held = millis() - touchDownAt;
    int moved = abs((int)x - downX) + abs((int)y - downY);
    if (!longFired && held < 600 && moved < 40) {
      tap = true;
      tx = downX; ty = downY;
    }
  }
}

static void deleteCurrentNote() {
  String base = noteList[noteRow].substring(1, noteList[noteRow].length() - 4);
  SD_MMC.remove("/recordings/" + base + ".wav");
  SD_MMC.remove("/recordings/" + base + ".txt");
  SD_MMC.remove("/recordings/" + base + ".tag");
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
  touch = I2cFt6336Dev::requestInstance(i2c->Get_I2cBusHandle(), I2C_FT6336_DEV_Address, EPD_WIDTH, EPD_HEIGHT);
  touch->Ft6336_Reset(EPD_TP_RST_PIN);
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
  bool tap, longPress, releaseEvent, downEvent;
  int tx = -1, ty = -1, swipe;
  pollTouch(tap, longPress, releaseEvent, tx, ty, swipe, downEvent);

  switch (state) {
    case ST_HOME:
      if (tap) {
        if (soundOn()) beep();
        state = ST_MENU;
        drawMenu();
      }
      if (millis() - lastActivity > 30000 && !pressed) sleepNow();
      if (millis() - lastAutoCheck > 300000UL) { lastAutoCheck = millis(); maybeAutoSync(); }
      break;

    case ST_MENU:
      /* MAKE NOTE arms on touch-down so a hold starts capturing immediately. */
      if (downEvent && ty >= 76 && ty < 112) {
        if (soundOn()) beep();
        startRecording();
        break;
      }
      if (longPress || (tap && ty > 184)) { sleepNow(); break; }
      if (tap) {
        if (soundOn()) beep();
        if (ty >= 36 && ty < 72) {
          viewTag = "";
          state = ST_VIEW_TAGS;
          drawViewTags();
        } else if (ty >= 116 && ty < 152) {
          todoLoad();
          todoTop = 0;
          state = ST_TODO;
          drawTodo();
        } else if (ty >= 156 && ty < 192) {
          state = ST_SETTINGS;
          drawSettings();
        }
      }
      if (millis() - lastActivity > 60000 && !pressed) sleepNow();
      break;

    case ST_MAKE:
      recPoll();
      {
        static uint32_t lastTick = 0;
        if (millis() - lastTick > 500) {
          wave[waveIdx] = recLevel();
          waveIdx = (waveIdx + 1) % 16;
          updateMake();
          lastTick = millis();
        }
      }
      if (recSeconds() >= MAKE_MAX_SECONDS) { finishRecording(); break; }
      if (makeArmed && !pressed) {
        /* Finger left the glass. A quick flick means they tapped rather than
           held, so keep recording until they come back and stop it. */
        makeArmed = false;
        if (millis() - makeDownAt < 600) {
          makeLatched = true;
          updateMake();
        } else {
          finishRecording();
        }
        break;
      }
      if (makeLatched) {
        if (longPress) { finishRecording(); break; }
        if (tap && ty >= 44 && ty <= 104) finishRecording();
      }
      break;

    case ST_TODO:
      if (longPress || (tap && ty > 184)) { state = ST_MENU; drawMenu(); break; }
      if (swipe == -1 && todoTop > 0) { todoTop--; drawTodo(); }
      if (swipe == 1 && todoTop + 6 < (int)todos.size()) { todoTop++; drawTodo(); }
      if (tap) {
        int trow = (ty - 36) / 24;
        if (trow >= 0 && trow < 6 && todoTop + trow < (int)todos.size()) {
          todos[todoTop + trow].done = !todos[todoTop + trow].done;
          todoSave();
          if (soundOn()) beep();
          drawTodo();
        }
      }
      break;

    case ST_TAG:
      if (tap) {
        const char* pick = nullptr;
        if (ty >= 36 && ty < 70) pick = tx < 100 ? TAGS[0] : TAGS[1];
        else if (ty >= 78 && ty < 112) pick = tx < 100 ? TAGS[2] : TAGS[3];
        else if (ty >= 120 && ty < 154) pick = TAGS[4];
        if (pick) {
          saveTag(pick);
          if (soundOn()) beep();
        }
        state = ST_HOME;
        drawHome();
      }
      break;

    case ST_SETTINGS:
      if (longPress || (tap && ty > 184)) { state = ST_HOME; drawHome(); break; }
      if (tap && ty >= 32 && ty < 180) {
        int row = (ty - 32) / 25;
        if (soundOn()) beep();
        switch (row) {
          case 0:
            state = ST_SET_WIFI;
            drawWifiScreen(portalStart());
            break;
          case 1:
            syncAll(false);
            break;
          case 2:
            confirmFree = false;
            state = ST_STORAGE;
            drawStorage();
            break;
          case 3:
            drawSyncScreen(0, 0, "connecting wifi...");
            if (staConnect(20000)) {
              serverStartSta();
              state = ST_SET_IP;
              drawIpScreen();
            } else {
              showError("wifi failed");
              drawSettings();
            }
            break;
          case 4:
            state = ST_HOWTO;
            drawHowTo();
            break;
          case 5:
            state = ST_EXTRA;
            drawExtra();
            break;
        }
      }
      break;

    case ST_HOWTO:
      if (tap || longPress) { state = ST_SETTINGS; drawSettings(); }
      break;

    case ST_EXTRA:
      if (longPress || (tap && ty > 184)) { state = ST_SETTINGS; drawSettings(); break; }
      if (tap) {
        if (ty >= 36 && ty < 72) {
          if (soundOn()) beep();
          tourFromExtra = true;   /* re-run the tour, touch nothing else */
          walkStep = 0;
          state = ST_WALKTHROUGH;
          drawTourStep(walkStep);
        } else if (ty >= 80 && ty < 116) {
          if (soundOn()) beep();
          state = ST_SYNCRATE;
          drawSyncRate();
        } else if (ty >= 124 && ty < 160) {
          if (soundOn()) beep();
          factoryStage = 0;
          state = ST_FACTORY;
          drawFactory();
        }
      }
      break;

    case ST_SYNCRATE:
      if (longPress || (tap && ty > 184)) { state = ST_EXTRA; drawExtra(); break; }
      if (tap) {
        int rowIdx = -1;
        if (ty >= 46 && ty < 76) rowIdx = 0;
        else if (ty >= 80 && ty < 110) rowIdx = 1;
        else if (ty >= 114 && ty < 144) rowIdx = 2;
        if (rowIdx >= 0) {
          int idx = rowIdx * 2 + (tx < 100 ? 0 : 1);
          netSetU32("syncHrs", SYNC_OPTS[idx]);
          if (soundOn()) beep();
          drawSyncRate();
        }
      }
      break;

    case ST_FACTORY:
      if (longPress || (tap && ty > 184)) { state = ST_EXTRA; drawExtra(); break; }
      if (tap && ty >= 132 && ty < 162) {
        bool right = tx >= 100;
        if (!right) {                    /* CANCEL / KEEP IT */
          state = ST_EXTRA;
          drawExtra();
        } else if (factoryStage == 0) {  /* ERASE -> ask once more */
          factoryStage = 1;
          drawFactory();
        } else {                         /* ERASE ALL - no way back */
          doFactoryReset();
        }
      }
      break;

    case ST_WALKTHROUGH:
      /* Gated: only the NEXT button advances, so every screen gets seen. */
      if (tap) {
        if (walkStep == TOUR_SOUND) {
          if (ty >= 66 && ty < 100) {
            netSet("sound", tx < 100 ? "1" : "0");
            if (soundOn()) beep();
            walkStep++;
            drawTourStep(walkStep);
          }
        } else if (ty >= 156 && ty < 182) {
          if (tx >= 64) {
            walkStep++;
            if (walkStep >= TOUR_STEPS) {
              netSetBool("first_boot_done", true);
              if (tourFromExtra) {
                tourFromExtra = false;
                state = ST_EXTRA;
                drawExtra();
              } else {
                state = ST_HOME;
                drawHome();
              }
            } else {
              if (soundOn()) beep();
              drawTourStep(walkStep);
            }
          } else if (tx < 60 && walkStep > 0) {
            walkStep--;
            if (soundOn()) beep();
            drawTourStep(walkStep);
          }
        }
      }
      break;

    case ST_SET_WIFI:
      portalPoll();
      if (longPress || (tap && ty > 184)) {
        portalStop();
        state = ST_SETTINGS;
        drawSettings();
      }
      break;

    case ST_SYNC:
      if (tap) syncCancel = true;
      break;

    case ST_STORAGE:
      if (longPress || (tap && ty > 184)) {
        confirmFree = false;
        state = ST_SETTINGS;
        drawSettings();
        break;
      }
      if (tap && ty >= 158 && ty < 180) {
        if (confirmFree) {
          freeTranscribedAudio();
          confirmFree = false;
          if (soundOn()) beep();
          drawStorage();
        } else {
          confirmFree = true;
          drawStorage();
        }
      } else if (tap) {
        confirmFree = false;
        drawStorage();
      }
      break;

    case ST_SET_IP:
      portalPoll();
      if (longPress || (tap && ty > 184)) {
        portalStop();
        staDisconnect();
        state = ST_SETTINGS;
        drawSettings();
      }
      break;

    case ST_VIEW_TAGS:
      if (longPress || (tap && ty > 184)) { state = ST_HOME; drawHome(); break; }
      if (tap) {
        int row = (ty - 38) / 27;
        if (row >= 0 && row < 5) {
          if (soundOn()) beep();
          viewTag = TAGS[row];
          refreshNoteList();
          listTop = 0; noteRow = -1;
          state = ST_VIEW_LIST;
          drawViewList();
        }
      }
      break;

    case ST_VIEW_LIST:
      if (longPress || (tap && ty > 184)) { state = ST_VIEW_TAGS; drawViewTags(); break; }
      if (swipe == -1 && listTop > 0) { listTop--; drawViewList(); }
      if (swipe == 1 && listTop + 6 < (int)noteList.size()) { listTop++; drawViewList(); }
      if (tap) {
        int row = (ty - 34) / 24;
        if (row >= 0 && row < 6 && listTop + row < (int)noteList.size()) {
          if (soundOn()) beep();
          noteRow = listTop + row;
          confirmDelete = false;
          loadTranscript(noteList[noteRow]);
          state = ST_VIEW_NOTE;
          drawViewNote();
        }
      }
      break;

    case ST_VIEW_NOTE:
      playPoll();
      if (longPress) {
        playStop();
        state = ST_VIEW_LIST;
        drawViewList();
        break;
      }
      if (swipe == 1 && transcriptLines.size() > 6 && transcriptPage < (int)((transcriptLines.size() + 5) / 6) - 1) {
        transcriptPage++;
        drawViewNote();
      }
      if (swipe == -1 && transcriptPage > 0) { transcriptPage--; drawViewNote(); }
      if (tap) {
        if (ty >= 134 && ty <= 156) {
          if (playActive()) {
            playStop();
            if (soundOn()) beep();
          } else if (playFile("/recordings" + noteList[noteRow])) {
            if (soundOn()) beep();
          }
          drawViewNote();
        } else if (ty >= 160 && ty <= 182) {
          if (confirmDelete) {
            deleteCurrentNote();
          } else {
            confirmDelete = true;
            drawViewNote();
          }
        } else if (ty > 184) {
          playStop();
          state = ST_VIEW_LIST;
          drawViewList();
        } else {
          confirmDelete = false;
          drawViewNote();
        }
      }
      break;
  }
  delay(10);
}
