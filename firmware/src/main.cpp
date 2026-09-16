/* Mono Note Mini - the full device, on a touch screen.
 *
 * Hardware: Waveshare ESP32-S3-Touch-AMOLED-2.16. A 480x480 colour AMOLED on
 * quad-SPI, a CST9220 touch panel, an ES7210 microphone array with an ES8311
 * for playback, an AXP2101 for power, and a PCF85063 keeping time.
 *
 * This restores what the two-button e-paper device could do - notes, tags,
 * playback, tasks, Wi-Fi, Bluetooth, sync - and drops the thing that made it
 * hard to use: a tree of nested menus navigated by counting button presses.
 * Five tabs, everything two taps from anywhere.
 *
 * Three things the screen made possible that were not before:
 *
 *   A Wi-Fi password can be typed on the device. The old board had to send
 *   one over Bluetooth from a browser because there was no way to enter text
 *   on a 200x200 panel with two buttons.
 *
 *   A passcode is chosen on first use rather than shipped as 1234. A default
 *   everybody knows is not a passcode, and the old device could only warn
 *   about it.
 *
 *   The panel is fast, so lists scroll, the recording timer ticks, and
 *   nothing has to be designed around a 2.7 second refresh that flashed the
 *   whole display.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <SD_MMC.h>
#include <esp_sleep.h>
#include <time.h>
#include <vector>
#include "user_config.h"
#include "pala_display.h"
#include "pala_ui.h"
#include "pala_touch.h"
#include "pala_power.h"
#include "pala_lock.h"
#include "pala_record.h"
#include "pala_net.h"
#include "pala_sync.h"
#include "pala_rtc.h"
#include "pala_ble.h"
#include "pala_usb.h"
#include "logo_mn.h"
#include "soc/usb_serial_jtag_struct.h"

/* How long the screen stays on with nothing happening. Chosen from a setting
   rather than fixed: on a desk while charging, sleeping after a minute is just
   obstructive, and in a pocket a minute is already too long. */
static const uint32_t SLEEP_CHOICES[4] = { 30000UL, 60000UL, 180000UL, 600000UL };
static const char* SLEEP_LABELS[4]     = { "30 sec", "1 min", "3 min", "10 min" };

#define IDLE_SLEEP_MS   60000UL
#define SYNC_RETRY_MS  300000UL
#define MAX_NOTE_SECONDS 119
#define CX (LCD_WIDTH / 2)

enum Screen {
  SCR_LOCK, SCR_REC, SCR_NOTES, SCR_NOTE, SCR_TASKS,
  SCR_WIFI, SCR_WIFI_PASS, SCR_MORE, SCR_PASSCODE,
  SCR_STORAGE, SCR_FACTORY, SCR_INTRO, SCR_BATTERY, SCR_TIME, SCR_DISPLAY,
  SCR_USBDRIVE
};

static const char* TABS[] = { "NOTES", "RECORD", "TASKS", "WI-FI", "MORE" };
static const int   TAB_SCREEN[] = { SCR_NOTES, SCR_REC, SCR_TASKS, SCR_WIFI, SCR_MORE };

static const char* TAGS[5] = { "idea", "reminder", "task", "journal", "none" };

static Screen  screen = SCR_LOCK;
static int     activeTab = 1;
static uint32_t lastActivity = 0;
static uint32_t lastSyncTry = 0;
static bool     bootedOnUsb = false;

/* display and quality-of-life settings, all held in NVS */
static uint8_t  cfgBright = 70;    /* per cent, 5-100 */
static uint8_t  cfgSleep = 1;      /* index into SLEEP_CHOICES */
static uint8_t  cfgSun = 0;        /* sunlight readability, 0-3 */
static bool     cfgStayOnUsb = true;
static bool     cfgAutoSync = true;

/* Per cent to what the panel wants, on a curve rather than a straight line.
   Perceived brightness is roughly the square of the drive level, so a linear
   slider spends most of its travel in a range that all looks the same and
   crosses the useful low end in the first few pixels. */
static uint8_t brightnessValue(uint8_t pct) {
  if (pct < 5) pct = 5;
  if (pct > 100) pct = 100;
  const uint32_t v = (uint32_t)pct * pct * 255u / 10000u;
  return (uint8_t)(v < 4 ? 4 : v);
}

/* Dimmed to a quarter before sleeping. The screen is most of the power draw
   while it is on, and the last stretch before a timeout is almost always time
   nobody is looking - dimming it is free, and it warns that sleep is coming. */
static bool     dimmed = false;
static uint8_t  cfgSaver = 0;      /* battery saver, 0 off 1 on */

static void setDim(bool on) {
  if (on == dimmed) return;
  dimmed = on;
  dispBrightness(on ? brightnessValue(cfgBright / 4 + 5)
                    : brightnessValue(cfgBright));
}

static void applyDisplaySettings() {
  dimmed = false;
  dispBrightness(brightnessValue(cfgBright));
  dispSunlight(cfgSun);
}

/* Battery saver, as one switch rather than five. The parts that cost power on
   this device are the screen, the radios and the clock speed, and a person who
   wants longer life wants all of them turned down at once - not a settings
   page to work through. */
static void applySaver() {
  if (cfgSaver) {
    if (cfgBright > 40) { cfgBright = 40; }
    cfgSun = 0;
    cfgSleep = 0;                 /* 30 seconds */
    cfgAutoSync = false;
    if (bleAdvertising()) bleStop();
    setCpuFrequencyMhz(80);       /* from 240 - the UI is not CPU-bound */
  } else {
    setCpuFrequencyMhz(240);
  }
  applyDisplaySettings();
}

static void loadSettings() {
  cfgBright    = (uint8_t)netGetU32("uiBright", 70);
  cfgSaver     = (uint8_t)netGetU32("uiSaver", 0);
  cfgSleep     = (uint8_t)netGetU32("uiSleep", 1);
  cfgSun       = (uint8_t)netGetU32("uiSun", 0);
  cfgStayOnUsb = netGetU32("uiUsbAwake", 1) != 0;
  cfgAutoSync  = netGetU32("uiAutoSync", 1) != 0;
  if (cfgBright < 5 || cfgBright > 100) cfgBright = 70;
  if (cfgSleep > 3)  cfgSleep = 1;
  if (cfgSun > 3)    cfgSun = 0;
}

static void saveSettings() {
  netSetU32("uiBright", cfgBright);
  netSetU32("uiSaver", cfgSaver);
  netSetU32("uiSleep", cfgSleep);
  netSetU32("uiSun", cfgSun);
  netSetU32("uiUsbAwake", cfgStayOnUsb ? 1 : 0);
  netSetU32("uiAutoSync", cfgAutoSync ? 1 : 0);
}

/* recording */
static bool     recording = false;
static uint32_t recStarted = 0;

/* notes
   Read once into memory, not while drawing. Every visible row used to do an
   exists() and a file open for its tag on every frame, which at fifty frames a
   second is hundreds of card operations to render a list that had not changed. */
struct Note {
  String base;
  String tag;
  bool   synced;
};
static std::vector<Note> notes;
static String noteFilter;           /* "" means show everything */
static UiPager notePager;
static String  openNote;
static int     syncedCount = 0, totalCount = 0;
static String  statusLine;

/* tasks */
static std::vector<String> tasks;      /* each begins "[x] " or "[ ] " */
static UiPager taskPager;

/* wi-fi */
static std::vector<String> wifiNames;
static UiPager wifiPager;
static String  wifiPick, wifiPass;
static bool    entryIsServer = false;   /* the keyboard is shared */
static String  wifiNote;
static bool    kbShift = false, kbSyms = false;

/* passcode entry - reused for unlocking, first-run and changing */
static String  codeEntry, codeFirst;
static String  codeNote;
static int     codeStage = 0;        /* 0 enter, 1 choose new, 2 confirm new */
static bool    codeForChange = false;
static uint32_t codeBlockedUntil = 0;

/* confirmation counters - both of these throw things away, so both ask twice */
static int freeStage = 0;
static int factoryStage = 0;
static int introPage = 0;

/* ---- notes on the card -------------------------------------------------- */

static bool isWav(const String& n) { return n.endsWith(".wav"); }

static String baseOf(const String& name) {
  String n = name;
  int slash = n.lastIndexOf('/');
  if (slash >= 0) n = n.substring(slash + 1);
  int dot = n.lastIndexOf('.');
  if (dot > 0) n = n.substring(0, dot);
  return n;
}

static String tagOf(const String& base);   /* defined below, used by loadNotes */

static void loadNotes() {
  notes.clear();
  totalCount = 0;
  syncedCount = 0;
  File dir = SD_MMC.open("/recordings");
  if (!dir) return;
  File f;
  while ((f = dir.openNextFile())) {
    String n = f.name();
    f.close();
    if (!isWav(n)) continue;
    Note note;
    note.base = baseOf(n);
    note.tag = tagOf(note.base);
    note.synced = SD_MMC.exists("/recordings/" + note.base + ".synced");
    notes.push_back(note);
    totalCount++;
    if (note.synced) syncedCount++;
  }
  dir.close();
  /* Newest first. The names are timestamps, so sorting them as text sorts them
     by time - which is only true because they are zero-padded and start with
     the year. */
  for (size_t i = 0; i + 1 < notes.size(); i++)
    for (size_t j = 0; j + 1 < notes.size() - i; j++)
      if (notes[j].base < notes[j + 1].base) {
        Note t = notes[j]; notes[j] = notes[j + 1]; notes[j + 1] = t;
      }
  notePager.perPage = 4;
  notePager.page = 0;
}

/* Which notes the Notes tab is currently showing. */
static void filtered(std::vector<int>& out) {
  out.clear();
  for (size_t i = 0; i < notes.size(); i++)
    if (!noteFilter.length() || notes[i].tag == noteFilter) out.push_back(i);
}

static String tagOf(const String& base) {
  File f = SD_MMC.open("/recordings/" + base + ".tag", "r");
  if (!f) return "";
  String t = f.readStringUntil('\n');
  f.close();
  t.trim();
  return t;
}

static void setTag(const String& base, const String& tag) {
  const String path = "/recordings/" + base + ".tag";
  if (tag == "none" || tag.length() == 0) { SD_MMC.remove(path); return; }
  File f = SD_MMC.open(path, "w");
  if (!f) return;
  f.print(tag);
  f.flush();                /* tag files came back empty without this once */
  f.close();
}

static void deleteNote(const String& base) {
  SD_MMC.remove("/recordings/" + base + ".wav");
  SD_MMC.remove("/recordings/" + base + ".tag");
  SD_MMC.remove("/recordings/" + base + ".txt");
  SD_MMC.remove("/recordings/" + base + ".synced");
}

/* A timestamped name reads better than the raw file name. */
static String prettyName(const String& base) {
  if (base.startsWith("rec_") && base.length() >= 19) {
    String d = base.substring(4, 12);      /* YYYYMMDD */
    String t = base.substring(13, 19);     /* HHMMSS   */
    return d.substring(6, 8) + "/" + d.substring(4, 6) + "  " +
           t.substring(0, 2) + ":" + t.substring(2, 4);
  }
  return base;
}

/* ---- tasks -------------------------------------------------------------- */

static void loadTasks() {
  tasks.clear();
  File f = SD_MMC.open("/todo.txt", "r");
  if (f) {
    while (f.available()) {
      String line = f.readStringUntil('\n');
      line.trim();
      if (line.length()) tasks.push_back(line);
    }
    f.close();
  }
  taskPager.total = tasks.size();
  taskPager.perPage = 5;
}

static void saveTasks() {
  File f = SD_MMC.open("/todo.txt", "w");
  if (!f) return;
  for (size_t i = 0; i < tasks.size(); i++) f.println(tasks[i]);
  f.flush();
  f.close();
}

/* ---- sync --------------------------------------------------------------- */

static void markSynced(const String& base) {
  File m = SD_MMC.open("/recordings/" + base + ".synced", "w");
  if (m) { m.print("1"); m.flush(); m.close(); }
}

static bool syncAll(bool sayWhy) {
  lastSyncTry = millis();
  if (usbDriveActive()) { if (sayWhy) statusLine = "card is on the computer"; return false; }
  if (totalCount == 0 || syncedCount >= totalCount) { if (sayWhy) statusLine = "nothing to send"; return false; }
  if (!syncConfigured())            { if (sayWhy) statusLine = "no server address"; return false; }
  if (netGet("ssid").length() == 0) { if (sayWhy) statusLine = "no wi-fi set"; return false; }

  statusLine = "connecting...";
  if (!staConnect(20000)) { statusLine = "no wi-fi"; return false; }

  int done = 0;
  String err;
  for (size_t i = 0; i < notes.size(); i++) {
    const String b = notes[i].base;
    if (notes[i].synced) continue;
    statusLine = "sending " + String(done + 1) + "...";
    if (syncUploadNote(b, err)) { markSynced(b); done++; }
    else break;                       /* the next will fail the same way */
  }
  staDisconnect();
  loadNotes();
  statusLine = (syncedCount >= totalCount) ? "all sent"
             : (err.length() ? err.substring(0, 34) : "");
  return done > 0;
}

/* ---- names -------------------------------------------------------------- */

static String timestampName() {
  struct tm t;
  if (getLocalTime(&t, 300)) {
    char buf[40];
    snprintf(buf, sizeof(buf), "rec_%04d%02d%02d_%02d%02d%02d.wav",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
    return String(buf);
  }
  return "rec_noclock_" + String(millis() / 1000) + ".wav";
}

/* An empty string when the time was unknown was worse than saying so: a blank
   corner reads as a layout fault, and it is the only warning that timestamps
   on new notes are about to be meaningless. */
static bool clockKnown() { return time(nullptr) >= 1700000000; }

static String clockNow() {
  if (!clockKnown()) return "--:--";
  struct tm t;
  if (!getLocalTime(&t, 100)) return "--:--";
  char buf[16];
  snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
  return String(buf);
}

static String dateNow() {
  if (!clockKnown()) return "clock not set";
  struct tm t;
  if (!getLocalTime(&t, 100)) return "clock not set";
  static const char* MON[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
  char buf[24];
  snprintf(buf, sizeof(buf), "%d %s %d", t.tm_mday, MON[t.tm_mon], t.tm_year + 1900);
  return String(buf);
}

static String tzLabel() {
  if (!netTimezoneSet()) return "not set";
  const int mins = netTimezoneMinutes();
  String l = "UTC";
  const int hrs = mins / 60;
  if (hrs > 0) l += "+" + String(hrs);
  else if (hrs < 0) l += String(hrs);
  else l += "+0";
  if (mins % 60) l += ":" + String(abs(mins % 60));
  return l;
}

/* ---- screens ------------------------------------------------------------ */

static void drawChrome(const String& title) {
  dispClear(COL_BLACK);
  uiHeader(title, clockNow());
  uiBatteryPill(LCD_WIDTH - 130, 22, powerPercent(), powerCharging());
}

static String mmss(uint32_t s) {
  char b[8];
  snprintf(b, sizeof(b), "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
  return String(b);
}

static void screenRecord(const UiTap& t) {
  drawChrome("RECORD");

  if (recording) {
    const uint32_t secs = (millis() - recStarted) / 1000;
    const int r = 96, cy = 210;
    dispCircle(CX, cy, r, COL_FAINT);
    float frac = (float)secs / (float)MAX_NOTE_SECONDS;
    if (frac > 1.0f) frac = 1.0f;
    dispArc(CX, cy, r, 7, -90.0f, -90.0f + 360.0f * frac,
            frac > 0.9f ? COL_AMBER : COL_RED);
    dispFillCircle(CX, cy, 62, COL_RED);
    dispTextCentered(cy - 12, mmss(secs), TXT_TITLE, COL_WHITE);
    dispTextCentered(cy + 22, "TAP TO STOP", TXT_SMALL, COL_WHITE);
    dispTextCentered(330, "recording", TXT_BODY, COL_RED);
  } else {
    dispBitmap1(CX - LOGO_W / 2, 84, LOGO_W, LOGO_H, LOGO_MN, COL_WHITE);
    dispTextCentered(150, dateNow(), TXT_SMALL, clockKnown() ? COL_DIM : COL_AMBER);
    dispFillCircle(CX, 232, 72, COL_BLUE);
    dispTextCentered(220, "TAP TO", TXT_BODY, COL_WHITE);
    dispTextCentered(242, "RECORD", TXT_BODY, COL_WHITE);

    String counter;
    uint16_t colour;
    if (totalCount == 0)                { counter = "no notes yet"; colour = COL_DIM; }
    else if (syncedCount >= totalCount) { counter = "all synced";   colour = COL_GREEN; }
    else { counter = String(syncedCount) + " of " + String(totalCount) + " synced";
           colour = COL_AMBER; }
    dispTextCentered(324, counter, 2, colour);
    const int pct = powerPercent();
    if (pct >= 0 && pct < POWER_WARN_PCT && !powerCharging()) {
      dispTextCentered(350, pct < POWER_TOO_LOW_PCT ? "battery too low to record"
                                                    : "battery low - charge soon",
                       1, pct < POWER_TOO_LOW_PCT ? COL_RED : COL_AMBER);
    } else if (statusLine.length()) {
      dispTextCentered(350, statusLine, TXT_SMALL, COL_DIM);
    }
  }
}

static void screenNotes(const UiTap& t) {
  drawChrome("NOTES");

  /* Filter chips. The old device had a whole screen for choosing a tag before
     it would show you a list; here they sit above the list and switching is one
     tap with the notes still in front of you. */
  const int chipW = (LCD_WIDTH - UI_PAD * 2 - 5 * 4) / 6;
  const char* chips[6] = { "all", TAGS[0], TAGS[1], TAGS[2], TAGS[3], "none" };
  for (int i = 0; i < 6; i++) {
    const String want = (i == 0) ? String("") : (i == 5 ? String("~none") : String(chips[i]));
    const bool on = (i == 0) ? (noteFilter.length() == 0)
                  : (i == 5) ? (noteFilter == "~none")
                             : (noteFilter == chips[i]);
    if (uiButton(t, UI_PAD + i * (chipW + 4), UI_HEADER_H + 6, chipW, 40,
                 chips[i], on ? COL_BLUE : COL_DIM, on)) {
      noteFilter = want;
      notePager.page = 0;
    }
  }

  std::vector<int> show;
  show.clear();
  for (size_t i = 0; i < notes.size(); i++) {
    const bool untagged = notes[i].tag.length() == 0;
    if (noteFilter.length() == 0) show.push_back(i);
    else if (noteFilter == "~none") { if (untagged) show.push_back(i); }
    else if (notes[i].tag == noteFilter) show.push_back(i);
  }
  notePager.total = show.size();
  if (notePager.page >= notePager.pages()) notePager.page = notePager.pages() - 1;

  if (show.empty()) {
    dispTextCentered(210, notes.empty() ? "nothing recorded yet" : "nothing with that tag",
                     2, COL_DIM);
    return;
  }

  /* Four rows of 48 on a 54 pitch. Five of 52 put the last one at y=358,
     which reached 410 - under a tab bar that starts at 408, and under the
     pager as well. */
  const int top = UI_HEADER_H + 54, rowH = 48;
  for (int i = 0; i < notePager.perPage; i++) {
    const int k = notePager.first() + i;
    if (k >= (int)show.size()) break;
    const Note& n = notes[show[k]];
    String right = n.tag.length() ? ("#" + n.tag) : String("");
    right += n.synced ? "  up" : "  --";
    if (uiRow(t, top + i * (rowH + 6), rowH, prettyName(n.base), right,
              n.synced ? COL_GREEN : COL_AMBER)) {
      openNote = n.base;
      screen = SCR_NOTE;
    }
  }
  uiPagerBar(t, notePager, LCD_HEIGHT - UI_TABBAR_H - 64);
}

static void screenNote(const UiTap& t) {
  drawChrome(prettyName(openNote));
  const bool playing = playActive();

  if (uiButton(t, UI_PAD, 90, 200, 64, playing ? "STOP" : "PLAY",
               playing ? COL_AMBER : COL_GREEN)) {
    if (playing) playStop();
    else playFile("/recordings/" + openNote + ".wav");
  }
  if (uiButton(t, LCD_WIDTH - UI_PAD - 200, 90, 200, 64, "DELETE", COL_RED, false)) {
    playStop();
    deleteNote(openNote);
    loadNotes();
    screen = SCR_NOTES;
    return;
  }

  dispText(UI_PAD, 176, "tag", TXT_SMALL, COL_DIM);
  const String cur = tagOf(openNote);
  const int bw = (LCD_WIDTH - UI_PAD * 2 - 4 * 6) / 5;
  for (int i = 0; i < 5; i++) {
    const bool on = (cur == TAGS[i]) || (cur.length() == 0 && i == 4);
    if (uiButton(t, UI_PAD + i * (bw + 6), 196, bw, 52, TAGS[i],
                 on ? COL_BLUE : COL_DIM, on)) {
      setTag(openNote, TAGS[i]);
      /* The list reads from the cache, so it has to be told as well - otherwise
         the tag is on the card and the list goes on showing the old one. */
      for (size_t k = 0; k < notes.size(); k++)
        if (notes[k].base == openNote)
          notes[k].tag = (String(TAGS[i]) == "none") ? String("") : String(TAGS[i]);
    }
  }

  if (playError()) dispTextCentered(266, String("playback: ") + playError(), TXT_SMALL, COL_RED);

  if (uiButton(t, UI_PAD, LCD_HEIGHT - UI_TABBAR_H - 74, 180, 58, "< BACK", COL_DIM, false)) {
    playStop();
    screen = SCR_NOTES;
  }
}

static void screenTasks(const UiTap& t) {
  drawChrome("TASKS");
  if (tasks.empty()) {
    dispTextCentered(180, "no tasks", TXT_BODY, COL_DIM);
    dispTextCentered(210, "add them from the website", TXT_SMALL, COL_DIM);
  }
  const int top = UI_HEADER_H + 10, rowH = 46;
  for (int i = 0; i < taskPager.perPage; i++) {
    const int idx = taskPager.first() + i;
    if (idx >= (int)tasks.size()) break;
    String line = tasks[idx];
    const bool done = line.startsWith("[x]");
    String text = line.length() > 4 ? line.substring(4) : line;
    if (text.length() > 24) text = text.substring(0, 24);
    if (uiRow(t, top + i * (rowH + 6), rowH, text, done ? "done" : "",
              done ? COL_GREEN : COL_DIM)) {
      tasks[idx] = (done ? "[ ] " : "[x] ") + (line.length() > 4 ? line.substring(4) : line);
      saveTasks();
    }
  }
  uiPagerBar(t, taskPager, LCD_HEIGHT - UI_TABBAR_H - 64);
}

static void screenWifi(const UiTap& t) {
  drawChrome("WI-FI");
  const String ssid = netGet("ssid");
  const bool joined = WiFi.status() == WL_CONNECTED;
  dispText(UI_PAD, 74, ssid.length() ? ("set: " + ssid) : "no network set", 1,
           joined ? COL_GREEN : COL_DIM);

  if (uiButton(t, UI_PAD, 96, 210, 58, "SCAN", COL_BLUE)) {
    wifiNote = "scanning...";
    dispTextCentered(300, wifiNote, TXT_BODY, COL_DIM);
    dispShow();
    String js = netScanJson();
    wifiNames.clear();
    /* The scan comes back as JSON. Only the names are wanted here, and pulling
       them out by hand avoids a parser for one field. */
    int i = 0;
    while (true) {
      int k = js.indexOf("\"ssid\":\"", i);
      if (k < 0) break;
      k += 8;
      int e = js.indexOf('"', k);
      if (e < 0) break;
      wifiNames.push_back(js.substring(k, e));
      i = e;
    }
    wifiPager.total = wifiNames.size();
    wifiPager.perPage = 3;
    wifiPager.page = 0;
    wifiNote = wifiNames.empty() ? "nothing in range" : "";
  }
  /* "ROUTER BUTTON" needed 234 pixels in a button with 194 of room, so it
     was being drawn past its own edge. */
  if (uiButton(t, LCD_WIDTH - UI_PAD - 210, 96, 210, 58, "ROUTER WPS", COL_DIM, false)) {
    wifiNote = netWpsStart() ? "press WPS on the router" : "could not start WPS";
  }

  const int wps = netWpsState();
  if (wps == 2) wifiNote = "joined by router button";
  else if (wps == 3) wifiNote = "router button not pressed in time";

  /* The space between the scan buttons (which end at 154) and the pager (at
     344) is 190 pixels. Four rows on a 46 pitch is exactly what fits; the
     first attempt at this started at 150 and ran into the buttons above it. */
  {
    const String api = netGet("api");
    /* Below the scan buttons, which end at 154 - the first attempt put this at
       128 and drew it across them. */
    if (uiRow(t, 158, 30, "Send notes to",
              api.length() ? api : String("not set"),
              api.length() ? COL_GREEN : COL_AMBER)) {
      entryIsServer = true;
      wifiPass = netGet("api");
      kbShift = false; kbSyms = false;
      screen = SCR_WIFI_PASS;
    }
  }

  /* Three networks between the server row (ending at 188) and the pager (at
     344), rather than four squeezed into the same band. */
  const int top = 196, rowH = 40;
  for (int i = 0; i < wifiPager.perPage; i++) {
    const int idx = wifiPager.first() + i;
    if (idx >= (int)wifiNames.size()) break;
    if (uiRow(t, top + i * (rowH + 6), rowH, wifiNames[idx], "", COL_DIM)) {
      wifiPick = wifiNames[idx];
      wifiPass = "";
      entryIsServer = false;
      kbShift = false; kbSyms = false;
      screen = SCR_WIFI_PASS;
    }
  }
  uiPagerBar(t, wifiPager, LCD_HEIGHT - UI_TABBAR_H - 64);
  if (wifiNote.length()) dispTextCentered(LCD_HEIGHT - UI_TABBAR_H - 96, wifiNote, TXT_SMALL, COL_AMBER);
}

static void screenWifiPass(const UiTap& t) {
  dispClear(COL_BLACK);
  uiHeader(entryIsServer ? String("SEND NOTES TO")
                         : (wifiPick.length() > 18 ? wifiPick.substring(0, 18) : wifiPick));
  uiField(UI_PAD, 72, LCD_WIDTH - UI_PAD * 2, wifiPass,
          entryIsServer ? "http://10.0.0.5:8000" : "wi-fi password", false);
  if (wifiNote.length()) dispTextCentered(126, wifiNote, TXT_SMALL, COL_AMBER);

  char ch = 0;
  const KeyResult k = uiKeyboard(t, 152, kbShift, kbSyms, &ch);
  if (k == KEY_CHAR && wifiPass.length() < 63) {
    wifiPass += ch;
    if (kbShift && !kbSyms) kbShift = false;    /* shift is for one letter */
  } else if (k == KEY_BACKSPACE && wifiPass.length()) {
    wifiPass.remove(wifiPass.length() - 1);
  } else if (k == KEY_ENTER) {
    if (entryIsServer) {
      String a = wifiPass;
      a.trim();
      /* Typing "http://" on a touch keyboard is eight presses of the fiddliest
         keys on it, so it is added when it is missing rather than demanded. */
      if (a.length() && !a.startsWith("http://") && !a.startsWith("https://"))
        a = "http://" + a;
      netSet("api", a);
      wifiNote = a.length() ? "saved" : "cleared";
      screen = SCR_WIFI;
    } else {
      netSet("ssid", wifiPick);
      netSet("pass", wifiPass);
      wifiNote = "joining...";
      dispTextCentered(126, wifiNote, TXT_SMALL, COL_AMBER);
      dispShow();
      const bool ok = staConnect(20000);
      wifiNote = ok ? "joined" : "wrong password, or out of range";
      if (ok) { screen = SCR_WIFI; }
    }
  }

  if (uiButton(t, UI_PAD, LCD_HEIGHT - 44, 120, 40, "cancel", COL_DIM, false)) {
    screen = SCR_WIFI;
  }
}

static void screenMore(const UiTap& t) {
  drawChrome("MORE");
  const bool ble = bleAdvertising();

  /* Seven rows of 44, pitched 48 apart, filling exactly the space between the
     header and the tab bar. The previous layout put a full-width row and two
     buttons at the same y - they were drawn on top of each other, and since
     the row is hit-tested first, tapping BATTERY locked the device instead. */
  const int H = 34, P = 37;
  int y = UI_HEADER_H + 2;

  if (uiRow(t, y, H, ble ? "Bluetooth: on" : "Bluetooth: off",
            ble ? bleStatus() : "tap to turn on", ble ? COL_GREEN : COL_DIM)) {
    if (ble) bleStop(); else bleBegin();
  }
  y += P;
  if (uiRow(t, y, H, "Sync all notes",
            statusLine.length() ? statusLine
              : (totalCount && syncedCount >= totalCount) ? String("all sent")
              : String(totalCount - syncedCount) + " waiting", COL_AMBER)) {
    /* Joining a network and uploading blocks for up to half a minute with the
       whole interface frozen. Say so first, or the tap looks like it missed. */
    statusLine = "working...";
    dispTextCentered(LCD_HEIGHT / 2, "syncing...", TXT_TITLE, COL_AMBER);
    dispShow();
    syncAll(true);
  }
  y += P;
  if (uiRow(t, y, H, "Battery",
            powerPercent() < 0 ? String("--") : (String(powerPercent()) + "%"),
            powerCharging() ? COL_GREEN : COL_DIM)) {
    screen = SCR_BATTERY;
  }
  y += P;
  if (uiRow(t, y, H, "Time and date", tzLabel(),
            clockKnown() ? COL_DIM : COL_AMBER)) {
    statusLine = "";
    screen = SCR_TIME;
  }
  y += P;
  if (uiRow(t, y, H, "Change passcode", "", COL_DIM)) {
    codeForChange = true;
    codeStage = 0;
    codeEntry = ""; codeFirst = "";
    codeNote = "enter your current passcode";
    screen = SCR_PASSCODE;
  }
  y += P;
  if (uiRow(t, y, H, "Storage and reset",
            String((uint32_t)(SD_MMC.usedBytes() / (1024ULL * 1024ULL))) + " MB", COL_DIM)) {
    freeStage = 0;
    screen = SCR_STORAGE;
  }
  y += P;
  if (uiRow(t, y, H, "Display and behaviour",
            SLEEP_LABELS[cfgSleep], COL_DIM)) {
    screen = SCR_DISPLAY;
  }
  y += P;
  if (uiRow(t, y, H, "USB drive",
            usbDriveActive() ? "open on the computer"
              : (usbHostPresent() ? "plugged in" : "not plugged in"),
            usbDriveActive() ? COL_GREEN : COL_DIM)) {
    statusLine = "";
    screen = SCR_USBDRIVE;
  }
  y += P;
  if (uiRow(t, y, H, "Lock now", "", COL_DIM)) {
    lockRelock();
    codeEntry = ""; codeNote = "";
    screen = SCR_LOCK;
  }

  /* No separate status line here any more: eight rows fill the space exactly,
     and a line drawn under them landed on top of the last one. Sync reports
     itself in its own row instead, which is where somebody is already looking. */
}

/* The passcode screen does three jobs: unlocking, choosing one on first use,
   and changing an existing one. They share a keypad and differ only in what
   the caption says and what happens when the digits are accepted. */
static void screenPasscode(const UiTap& t) {
  dispClear(COL_BLACK);

  const uint32_t penalty = lockPenaltyMs();
  if (penalty && millis() < codeBlockedUntil) {
    const uint32_t left = (codeBlockedUntil - millis()) / 1000;
    dispTextCentered(180, "TOO MANY TRIES", TXT_BODY, COL_RED);
    dispTextCentered(230, "wait " + String(left + 1) + "s", TXT_BODY, COL_DIM);
    dispTextCentered(280, String(lockFailures()) + " wrong attempts", TXT_SMALL, COL_DIM);
    return;
  }

  String title;
  if (screen == SCR_LOCK && !lockIsSet()) title = "CHOOSE A PASSCODE";
  else if (codeForChange && codeStage == 0) title = "CURRENT PASSCODE";
  else if (codeStage == 1) title = "NEW PASSCODE";
  else if (codeStage == 2) title = "CONFIRM IT";
  else title = "PASSCODE";

  dispTextCentered(26, title, TXT_BODY, COL_WHITE);
  if (codeNote.length()) dispTextCentered(54, codeNote, TXT_SMALL, COL_AMBER);

  /* Dots rather than digits: shoulder-surfing a four digit code off a screen
     this size is otherwise trivial. */
  const int n = codeEntry.length();
  for (int i = 0; i < LOCK_MAX_LEN && i < 8; i++) {
    const int x = CX - (8 * 26) / 2 + i * 26 + 13;
    if (i < n) dispFillCircle(x, 86, 8, COL_WHITE);
    else       dispCircle(x, 86, 8, COL_FAINT);
  }

  char ch = 0;
  const KeyResult k = uiKeypad(t, 116, &ch);
  if (k == KEY_CHAR && (int)codeEntry.length() < LOCK_MAX_LEN) {
    codeEntry += ch;
  } else if (k == KEY_BACKSPACE && codeEntry.length()) {
    codeEntry.remove(codeEntry.length() - 1);
  } else if (k == KEY_ENTER) {
    if ((int)codeEntry.length() < LOCK_MIN_LEN) {
      codeNote = "at least 4 digits";
      return;
    }
    dispTextCentered(96, "checking", TXT_SMALL, COL_DIM);
    dispShow();

    if (screen == SCR_LOCK && !lockIsSet()) {
      /* First run: choose, then confirm. */
      if (codeStage == 0) { codeFirst = codeEntry; codeEntry = ""; codeStage = 2;
                            codeNote = "type it again"; return; }
      if (codeEntry != codeFirst) { codeEntry = ""; codeStage = 0; codeFirst = "";
                                    codeNote = "they did not match"; return; }
      lockSet(codeEntry);
      codeEntry = ""; codeNote = ""; codeStage = 0;
      introPage = 0;
      screen = SCR_INTRO;
      return;
    }

    if (codeForChange) {
      if (codeStage == 0) {
        if (!lockVerify(codeEntry)) {
          codeEntry = "";
          codeNote = "wrong passcode";
          codeBlockedUntil = millis() + lockPenaltyMs();
          return;
        }
        codeEntry = ""; codeStage = 1; codeNote = "pick a new one";
        return;
      }
      if (codeStage == 1) { codeFirst = codeEntry; codeEntry = ""; codeStage = 2;
                            codeNote = "type it again"; return; }
      if (codeEntry != codeFirst) { codeEntry = ""; codeStage = 1; codeFirst = "";
                                    codeNote = "they did not match"; return; }
      lockSet(codeEntry);
      codeEntry = ""; codeNote = ""; codeStage = 0; codeForChange = false;
      screen = SCR_MORE;
      return;
    }

    /* Ordinary unlock. */
    if (lockVerify(codeEntry)) {
      codeEntry = ""; codeNote = "";
      screen = SCR_REC;
    } else {
      codeEntry = "";
      codeNote = "wrong passcode";
      codeBlockedUntil = millis() + lockPenaltyMs();
    }
  }
}

static void screenStorage(const UiTap& t) {
  drawChrome("STORAGE");

  const uint64_t used = SD_MMC.usedBytes() / (1024ULL * 1024ULL);
  const uint64_t all  = SD_MMC.totalBytes() / (1024ULL * 1024ULL);
  dispText(UI_PAD, 84, "card " + String((uint32_t)used) + " of " +
           String((uint32_t)all) + " MB used", TXT_BODY, COL_WHITE);

  int upCount = 0;
  for (size_t i = 0; i < notes.size(); i++) if (notes[i].synced) upCount++;
  dispText(UI_PAD, 120, String(upCount) + " notes are on the server", TXT_SMALL, COL_DIM);
  dispText(UI_PAD, 138, String((int)notes.size() - upCount) + " are not, and stay", TXT_SMALL, COL_DIM);

  /* Only notes that reached the server can be removed here. Deleting one that
     has not been uploaded would be destroying the only copy, which is not
     something a "free up space" button should ever do. */
  const uint16_t colour = freeStage ? COL_RED : COL_AMBER;
  const String label = freeStage ? "REALLY DELETE THEM" : "DELETE SYNCED NOTES";
  if (uiButton(t, UI_PAD, 176, LCD_WIDTH - UI_PAD * 2, 66, label, colour, freeStage > 0)) {
    if (!freeStage && upCount > 0) {
      freeStage = 1;
    } else if (freeStage) {
      for (size_t i = 0; i < notes.size(); i++)
        if (notes[i].synced) deleteNote(notes[i].base);
      loadNotes();
      freeStage = 0;
      statusLine = "deleted";
    }
  }
  if (freeStage) dispTextCentered(252, "the audio goes from the card", TXT_SMALL, COL_RED);
  else           dispTextCentered(252, "only notes already uploaded", TXT_SMALL, COL_DIM);

  if (uiButton(t, UI_PAD, LCD_HEIGHT - UI_TABBAR_H - 74, 180, 58, "< BACK", COL_DIM, false)) {
    freeStage = 0;
    screen = SCR_MORE;
  }
  if (uiButton(t, LCD_WIDTH - UI_PAD - 180, LCD_HEIGHT - UI_TABBAR_H - 74, 180, 58,
               "RESET", COL_RED, false)) {
    factoryStage = 0;
    screen = SCR_FACTORY;
  }
}

static void screenFactory(const UiTap& t) {
  drawChrome("RESET");
  dispText(UI_PAD, 84, "This forgets:", TXT_BODY, COL_WHITE);
  dispText(UI_PAD, 118, "the passcode", TXT_SMALL, COL_DIM);
  dispText(UI_PAD, 136, "the wi-fi network and password", TXT_SMALL, COL_DIM);
  dispText(UI_PAD, 154, "the server address", TXT_SMALL, COL_DIM);
  dispText(UI_PAD, 180, "Your notes on the card are kept.", TXT_SMALL, COL_GREEN);

  const String label = factoryStage == 0 ? "ERASE SETTINGS"
                     : factoryStage == 1 ? "ARE YOU SURE"
                                         : "ERASE, AND RESTART";
  if (uiButton(t, UI_PAD, 212, LCD_WIDTH - UI_PAD * 2, 66, label,
               factoryStage ? COL_RED : COL_AMBER, factoryStage > 1)) {
    if (factoryStage < 2) factoryStage++;
    else {
      netClearAll();
      lockClear();
      dispClear(COL_BLACK);
      dispTextCentered(220, "erased", TXT_TITLE, COL_WHITE);
      dispShow();
      delay(1200);
      ESP.restart();
    }
  }

  if (uiButton(t, UI_PAD, LCD_HEIGHT - UI_TABBAR_H - 74, 180, 58, "< BACK", COL_DIM, false)) {
    factoryStage = 0;
    screen = SCR_MORE;
  }
}

/* Shown once, after a passcode is chosen. Three cards, and every one of them
   can be walked out of. The tour on the old device ran automatically, demanded
   one specific button and had no exit, so a device whose button was not the one
   it wanted could not be used at all. */
static void screenIntro(const UiTap& t) {
  dispClear(COL_BLACK);
  const char* title[3] = { "TAP TO RECORD", "FIVE TABS", "WI-FI" };
  const char* line1[3] = { "The big circle on the",
                           "Notes, Record, Tasks,",
                           "Open the Wi-Fi tab and" };
  const char* line2[3] = { "Record tab starts it,",
                           "Wi-Fi and More, along",
                           "type your password in." };
  const char* line3[3] = { "and stops it.",
                           "the bottom. That is all.",
                           "Notes upload themselves." };

  dispTextCentered(120, title[introPage], TXT_TITLE, COL_BLUE);
  dispTextCentered(190, line1[introPage], TXT_BODY, COL_WHITE);
  dispTextCentered(218, line2[introPage], TXT_BODY, COL_WHITE);
  dispTextCentered(246, line3[introPage], TXT_BODY, COL_WHITE);

  for (int i = 0; i < 3; i++)
    dispFillCircle(CX - 24 + i * 24, 300, 6, i == introPage ? COL_WHITE : COL_FAINT);

  if (uiButton(t, CX - 110, 340, 220, 64,
               introPage < 2 ? "NEXT" : "START", COL_BLUE)) {
    if (introPage < 2) introPage++;
    else screen = SCR_REC;
  }
  if (uiButton(t, LCD_WIDTH - UI_PAD - 90, LCD_HEIGHT - 56, 90, 44, "skip", COL_DIM, false))
    screen = SCR_REC;
}

/* The card, handed to whatever it is plugged into.
 *
 * This is the transfer method that needs nothing: no app, no pairing, no
 * network, no account. It also cannot coexist with the device using its own
 * card, so recording stops for as long as the host has it - stated on the
 * screen rather than left to be discovered.
 */
static void screenUsbDrive(const UiTap& t) {
  drawChrome("USB DRIVE");

  if (!usbDriveActive()) {
    dispTextCentered(104, "Show the card as a drive", TXT_BODY, COL_WHITE);
    dispTextCentered(140, "on whatever this is plugged", TXT_SMALL, COL_DIM);
    dispTextCentered(160, "into. No app, no pairing.", TXT_SMALL, COL_DIM);

    if (!usbHostPresent()) {
      dispTextCentered(210, "nothing plugged in", TXT_BODY, COL_AMBER);
    } else if (!usbDriveAvailable()) {
      dispTextCentered(210, "no card", TXT_BODY, COL_RED);
    } else if (uiButton(t, UI_PAD, 196, LCD_WIDTH - UI_PAD * 2, 70,
                        "HAND OVER THE CARD", COL_BLUE)) {
      if (usbDriveBegin()) statusLine = "";
      else statusLine = "could not start";
    }
    dispTextCentered(292, "recording stops while the", TXT_SMALL, COL_DIM);
    dispTextCentered(312, "other machine has the card", TXT_SMALL, COL_DIM);
  } else {
    dispFillCircle(CX, 176, 58, COL_GREEN);
    dispTextCentered(166, "OPEN", TXT_BODY, COL_WHITE);
    dispTextCentered(256, "Your notes are a folder", TXT_BODY, COL_WHITE);
    dispTextCentered(286, "in /recordings", TXT_SMALL, COL_DIM);
    dispTextCentered(316, "eject it there first", TXT_SMALL, COL_AMBER);

    if (uiButton(t, UI_PAD, 346, LCD_WIDTH - UI_PAD * 2, 62,
                 "TAKE THE CARD BACK", COL_AMBER)) {
      usbDriveEnd();
      loadNotes();
      loadTasks();
      statusLine = "card back";
    }
  }

  if (statusLine.length())
    dispTextCentered(LCD_HEIGHT - UI_TABBAR_H - 38, statusLine, TXT_SMALL, COL_AMBER);

  /* Below the hand-over button rather than across it. The two are in
     opposite branches so they never drew together, but overlapping rectangles
     are how the BATTERY button became unreachable, and leaving one in place
     means the checker has to be argued with every time it runs. */
  if (!usbDriveActive() &&
      uiButton(t, LCD_WIDTH - UI_PAD - 150, 414, 150, 56,
               "< BACK", COL_DIM, false)) {
    statusLine = "";
    screen = SCR_MORE;
  }
}

/* Display and the handful of behaviours worth being able to change.
 *
 * Everything here is a preference rather than a feature: the device works with
 * all of it left alone. They exist because the defaults cannot be right for
 * both a desk and a pocket - a screen timeout that suits one is wrong for the
 * other, and brightness that reads indoors is invisible outside.
 */
static void screenDisplay(const UiTap& t) {
  drawChrome("DISPLAY");
  const int H = 44, P = 50;
  int y = UI_HEADER_H + 6;

  dispText(UI_PAD, y, "brightness", TXT_SMALL, COL_DIM);
  {
    const String pct = String(cfgBright) + "%";
    dispText(LCD_WIDTH - UI_PAD - dispTextWidth(pct, TXT_SMALL), y, pct,
             TXT_SMALL, COL_WHITE);
  }
  {
    int v = cfgBright;
    if (uiSlider(UI_PAD, y + 20, LCD_WIDTH - UI_PAD * 2, 40, 5, 100, &v)) {
      cfgBright = (uint8_t)v;
      dimmed = false;
      /* Applied while the finger is still down, so the panel is judged by
         looking at it rather than by letting go and hoping. Saved on release
         instead of every frame - NVS has a finite number of writes and a drag
         across the slider is a hundred of them. */
      dispBrightness(brightnessValue(cfgBright));
    } else if (!touchDown()) {
      static uint8_t lastSaved = 0;
      if (lastSaved != cfgBright) { lastSaved = cfgBright; saveSettings(); }
    }
  }
  y += 72;

  if (uiRow(t, y, H, "Battery saver", cfgSaver ? "on" : "off",
            cfgSaver ? COL_GREEN : COL_DIM)) {
    cfgSaver = cfgSaver ? 0 : 1;
    applySaver();
    saveSettings();
  }
  y += P;
  dispText(UI_PAD, y - 6, cfgSaver ? "dim, 30s screen, no auto-sync, 80MHz"
                                   : "dims the screen and slows the chip",
           TXT_SMALL, COL_DIM);
  y += 18;

  if (uiRow(t, y, H, "Screen off after", SLEEP_LABELS[cfgSleep], COL_DIM)) {
    cfgSleep = (uint8_t)((cfgSleep + 1) % 4);
    saveSettings();
  }
  y += P;

  static const char* SUN[4] = { "off", "low", "medium", "high" };
  if (uiRow(t, y, H, "Sunlight mode", SUN[cfgSun], cfgSun ? COL_AMBER : COL_DIM)) {
    cfgSun = (uint8_t)((cfgSun + 1) % 4);
    applyDisplaySettings();
    saveSettings();
  }
  y += P;

  if (uiRow(t, y, H, "Stay awake on USB", cfgStayOnUsb ? "yes" : "no",
            cfgStayOnUsb ? COL_GREEN : COL_DIM)) {
    cfgStayOnUsb = !cfgStayOnUsb;
    saveSettings();
  }
  y += P;

  if (uiRow(t, y, H, "Sync by itself", cfgAutoSync ? "yes" : "no",
            cfgAutoSync ? COL_GREEN : COL_DIM)) {
    cfgAutoSync = !cfgAutoSync;
    saveSettings();
  }

  if (uiButton(t, LCD_WIDTH - UI_PAD - 150, 414, 150, 56,
               "< BACK", COL_DIM, false)) {
    screen = SCR_MORE;
  }
}

/* Time and date.
 *
 * Four separate things had to be right for the clock to be, and none of them
 * were: the RTC was never read because the bus it wanted was never created,
 * NTP was asked and then hung up on, nothing wrote the answer back to the
 * chip, and the zone could only be set from a browser over Bluetooth. This
 * screen is where the last of those is fixed and where the others can be
 * checked by eye.
 */
static void screenTime(const UiTap& t) {
  drawChrome("TIME");

  dispTextCentered(96, clockNow(), TXT_HUGE, clockKnown() ? COL_WHITE : COL_DIM);
  dispTextCentered(168, dateNow(), TXT_BODY, clockKnown() ? COL_DIM : COL_AMBER);

  dispText(UI_PAD, 196, rtcChipFound() ? "clock chip: found"
                                       : "clock chip: NOT FOUND", TXT_SMALL,
           rtcChipFound() ? COL_DIM : COL_RED);
  dispText(UI_PAD, 216, "time zone", TXT_SMALL, COL_DIM);
  dispTextCentered(244, tzLabel(), TXT_TITLE,
                   netTimezoneSet() ? COL_WHITE : COL_AMBER);

  const int mins = netTimezoneMinutes();
  if (uiButton(t, UI_PAD, 236, 90, 56, "-", COL_BLUE, false))
    netSetTimezoneMinutes(mins - 60 < -720 ? -720 : mins - 60);
  if (uiButton(t, LCD_WIDTH - UI_PAD - 90, 236, 90, 56, "+", COL_BLUE, false))
    netSetTimezoneMinutes(mins + 60 > 840 ? 840 : mins + 60);

  /* Whole hours only. Offsets of 30 and 45 minutes exist, and can still be set
     exactly from the website over Bluetooth; handling them here would mean
     three controls instead of two arrows for a case most people never meet. */
  dispTextCentered(300, "whole hours only", TXT_SMALL, COL_DIM);

  if (uiButton(t, UI_PAD, 330, LCD_WIDTH - UI_PAD * 2, 60,
               "SET FROM THE INTERNET", COL_GREEN)) {
    if (netGet("ssid").length() == 0) {
      statusLine = "set up wi-fi first";
    } else {
      dispTextCentered(400, "connecting...", TXT_SMALL, COL_AMBER);
      dispShow();
      if (staConnect(20000)) {
        staDisconnect();
        statusLine = clockKnown() ? "clock set" : "no answer from the time server";
      } else {
        statusLine = "could not join the network";
      }
    }
  }
  if (statusLine.length())
    dispTextCentered(LCD_HEIGHT - UI_TABBAR_H - 40, statusLine, TXT_SMALL, COL_AMBER);

  if (uiButton(t, LCD_WIDTH - UI_PAD - 150, 414, 150, 56,
               "< BACK", COL_DIM, false)) {
    statusLine = "";
    screen = SCR_MORE;
  }
}

/* Everything the PMU knows about power, on one screen.
 *
 * The old board could only estimate this: a divider on an ADC pin, needing the
 * chip's factory calibration applied by hand, and even then it could not tell
 * a flat cell from no cell at all. The AXP2101 measures the battery itself,
 * so the percentage here is a real reading rather than a voltage curve guess -
 * which is why it is shown as a number instead of rounded to quarters to hide
 * how rough it was.
 */
static void screenBattery(const UiTap& t) {
  drawChrome("BATTERY");

  const int pct = powerPercent();
  const bool chg = powerCharging();
  const bool usb = powerUsbPresent();
  const bool fitted = powerBatteryPresent();

  uint16_t colour = COL_GREEN;
  if (pct >= 0 && pct < POWER_TOO_LOW_PCT)  colour = COL_RED;
  else if (pct >= 0 && pct < POWER_WARN_PCT) colour = COL_AMBER;
  if (chg) colour = COL_GREEN;

  const int cy = 178, r = 78;
  dispArc(CX, cy, r, 10, -90.0f, 270.0f, COL_FAINT);
  if (pct > 0) dispArc(CX, cy, r, 10, -90.0f, -90.0f + 3.6f * pct, colour);

  if (pct < 0) {
    dispTextCentered(cy - 12, "--", TXT_TITLE, COL_DIM);
  } else {
    const String n = String(pct);
    dispText(CX - dispTextWidth(n, 4) / 2 - 8, cy - 16, n, TXT_BIG, COL_WHITE);
    dispText(CX + dispTextWidth(n, 4) / 2 + 2, cy - 4, "%", TXT_BODY, COL_DIM);
  }

  String state;
  if (!fitted)     state = "no battery fitted";
  else if (chg)    state = "charging";
  else if (usb)    state = "on USB, charged";
  else             state = "on battery";
  dispTextCentered(cy + r + 14, state, 2, chg ? COL_GREEN : COL_DIM);

  const int ly = 300;
  dispText(UI_PAD, ly, "battery", TXT_SMALL, COL_DIM);
  dispText(UI_PAD + 150, ly, String(powerMillivolts()) + " mV", TXT_SMALL, COL_WHITE);
  dispText(UI_PAD, ly + 20, "system rail", TXT_SMALL, COL_DIM);
  dispText(UI_PAD + 150, ly + 20, String(powerSystemMillivolts()) + " mV", TXT_SMALL, COL_WHITE);
  dispText(UI_PAD, ly + 40, "usb", TXT_SMALL, COL_DIM);
  dispText(UI_PAD + 150, ly + 40, usb ? (String(powerUsbMillivolts()) + " mV") : String("not connected"),
           1, usb ? COL_WHITE : COL_DIM);
  dispText(UI_PAD, ly + 60, "regulator", TXT_SMALL, COL_DIM);
  dispText(UI_PAD + 150, ly + 60, String(powerTemperatureC(), 1) + " C", TXT_SMALL, COL_WHITE);

  if (uiButton(t, LCD_WIDTH - UI_PAD - 150, LCD_HEIGHT - UI_TABBAR_H - 74, 150, 58,
               "< BACK", COL_DIM, false)) {
    screen = SCR_MORE;
  }
}

/* ---- USB, sleep --------------------------------------------------------- */

/* Was a poke at the USB-Serial/JTAG peripheral's start-of-frame flag. That
   peripheral is not the one in use now, so its registers read as nothing
   whatever is plugged in - TinyUSB is asked instead. */
static bool usbSofSeen() { return usbHostPresent(); }

static void sleepNow() {
  playStop();
  bleStop();
  lockRelock();
  dispSleep(true);
  touchSleep();
  delay(50);
  esp_sleep_enable_ext1_wakeup(
      (1ULL << TOUCH_INT_PIN) | (1ULL << USER_BUTTON_PIN) | (1ULL << BOOT_BUTTON_PIN),
      ESP_EXT1_WAKEUP_ANY_LOW);
  esp_deep_sleep_start();
}

/* ---- recording ---------------------------------------------------------- */

static void startRecording() {
  /* The host has the card. Writing to it from here at the same time is how a
     filesystem gets corrupted, and the damage surfaces later as missing notes
     rather than as an error anyone sees now. */
  if (usbDriveActive()) { statusLine = "card is on the computer"; return; }
  /* A recording that loses power part way through is lost anyway, and writing
     to the card while the rail collapses is how a filesystem gets damaged
     rather than merely a file. Refusing is kinder than a corrupt card.
     Charging overrides it: on USB there is no rail about to collapse. */
  const int pct = powerPercent();
  if (pct >= 0 && pct < POWER_TOO_LOW_PCT && !powerCharging()) {
    statusLine = "battery too low to record";
    return;
  }
  playStop();
  if (!recBegin()) { statusLine = "microphone busy"; return; }
  recording = true;
  recStarted = millis();
}

static void stopRecording() {
  recording = false;
  const String name = timestampName();
  const bool ok = recSave("/recordings/" + name);
  statusLine = ok ? "saved" : "not saved - card problem";
  loadNotes();
  if (ok) syncAll(false);
}

/* ---- setup and loop ----------------------------------------------------- */

/* A line per subsystem, over USB, so the state of the device can be read
   instead of guessed at from the outside.
 *
 * The transmit timeout is set to zero first, and that is not a detail. A write
 * to a console with no reader blocks until somebody drains it, and on the old
 * board that froze every button on the device for an evening. At zero the
 * bytes are dropped instead, so this is safe to call whether or not anything
 * is listening. */
static void bootReport() {
  Serial.setTxTimeoutMs(0);
  Serial.println();
  Serial.println("--- mono note mini ---");

  Serial.printf("psram      %u KB free of %u KB\n",
                (unsigned)(ESP.getFreePsram() / 1024),
                (unsigned)(ESP.getPsramSize() / 1024));

  const uint8_t ct = SD_MMC.cardType();
  const char* kind = ct == CARD_NONE ? "none" : ct == CARD_MMC ? "MMC"
                   : ct == CARD_SD ? "SDSC" : ct == CARD_SDHC ? "SDHC" : "unknown";
  if (ct == CARD_NONE) {
    Serial.println("sd card    NOT MOUNTED");
    Serial.printf("           tried 1-bit SDIO on clk=%d cmd=%d d0=%d, dat3 held high\n",
                  (int)SDMMC_CLK_PIN, (int)SDMMC_CMD_PIN, (int)SDMMC_D0_PIN);
  } else {
    Serial.printf("sd card    %s, %u MB total, %u MB used\n", kind,
                  (unsigned)(SD_MMC.totalBytes() / (1024ULL * 1024ULL)),
                  (unsigned)(SD_MMC.usedBytes() / (1024ULL * 1024ULL)));
    Serial.printf("           /recordings %s\n",
                  SD_MMC.exists("/recordings") ? "present" : "MISSING");
  }

  Serial.printf("clock chip %s\n", rtcChipFound() ? "found at 0x51" : "NOT FOUND");
  Serial.printf("time       %s\n", clockKnown() ? "set" : "not set");
  Serial.printf("battery    %d%%%s, %d mV\n", powerPercent(),
                powerCharging() ? " (charging)" : "", powerMillivolts());
  Serial.printf("wi-fi      %s\n",
                netGet("ssid").length() ? netGet("ssid").c_str() : "not configured");
  Serial.printf("server     %s\n",
                syncConfigured() ? "set" : "not set");
  Serial.printf("notes      %d, %d already sent\n", totalCount, syncedCount);
  Serial.println("----------------------");
}

void setup() {
  Serial.begin(115200);
  delay(150);

  powerBegin();
  if (!dispBegin()) { delay(2000); ESP.restart(); }
  dispClear(COL_BLACK);
  dispTextCentered(230, "starting", TXT_BODY, COL_DIM);
  dispShow();

  touchBegin();
  if (rtcBegin()) rtcRestoreSystemTime();

  pinMode(SDMMC_DAT3_PIN, OUTPUT);
  digitalWrite(SDMMC_DAT3_PIN, HIGH);
  SD_MMC.setPins(SDMMC_CLK_PIN, SDMMC_CMD_PIN, SDMMC_D0_PIN);
  if (!SD_MMC.begin("/sdcard", true)) {
    dispClear(COL_BLACK);
    dispTextCentered(200, "NO SD CARD", TXT_TITLE, COL_RED);
    dispTextCentered(250, "nothing can be saved", TXT_SMALL, COL_DIM);
    dispShow();
    delay(2500);
  }
  SD_MMC.mkdir("/recordings");

  netBegin();
  applyTimezone();
  lockBegin();
  audioReady();

  for (int i = 0; i < 8 && !bootedOnUsb; i++) {
    if (usbSofSeen()) bootedOnUsb = true;
    delay(120);
  }

  loadSettings();
  applySaver();          /* which applies the display settings too */
  loadNotes();
  loadTasks();
  bootReport();

  /* If the RTC had nothing and a network is already known, the clock is worth
     a few seconds at boot: every note made before it is set carries a name
     that means nothing and sorts wrongly. */
  if (!clockKnown() && netGet("ssid").length()) {
    dispClear(COL_BLACK);
    dispTextCentered(220, "setting the clock", TXT_BODY, COL_DIM);
    dispShow();
    if (staConnect(15000)) staDisconnect();
  }

  /* First use goes straight to choosing a passcode; afterwards, to unlocking.
     There is no way past either, which is the point of having one. */
  screen = SCR_LOCK;
  codeStage = 0;
  codeEntry = "";
  codeNote = lockIsSet() ? "" : "pick 4 to 12 digits";

  /* Re-arm any outstanding penalty. The failure count is in NVS, but the wait
     was only ever in RAM, so without this a power cycle skipped it entirely -
     and a device somebody is holding is trivially power cycled. Storing the
     count and not enforcing it on the way back up protects nothing. */
  if (lockFailures() > 0) codeBlockedUntil = millis() + lockPenaltyMs();

  lastActivity = millis();
}

void loop() {
  UiTap tap{ false, 0, 0 };
  tap.happened = touchTapped(&tap.x, &tap.y);
  if (tap.happened || touchDown()) { lastActivity = millis(); setDim(false); }

  playPoll();

  /* The report is printed at boot, but nothing is listening then - a CDC port
     only exists once the firmware is running, and by the time anything opens
     it the report has already been dropped. Sending any character asks for
     another, which makes it a diagnostic that can be used rather than one that
     has to be caught. */
  if (Serial.available()) {
    while (Serial.available()) Serial.read();
    bootReport();
  }

  /* Recording is modal: the tab bar is not drawn and a tap anywhere stops it,
     because fumbling for a small target is not part of catching a thought. */
  if (recording) {
    const uint32_t secs = (millis() - recStarted) / 1000;
    if (tap.happened || recSeconds() >= MAX_NOTE_SECONDS || secs >= MAX_NOTE_SECONDS) {
      stopRecording();
    } else {
      static uint32_t lastDrawn = 0xFFFFFFFF;
      if (secs != lastDrawn) { lastDrawn = secs; screenRecord(tap); dispShow(); }
      delay(20);
      return;
    }
  }

  if (screen == SCR_LOCK || screen == SCR_PASSCODE) {
    screenPasscode(tap);
    dispShow();
    /* Locked means locked: no tabs, no sleep shortcut past it. */
    if (millis() - lastActivity > SLEEP_CHOICES[cfgSleep]
      && !(bootedOnUsb && cfgStayOnUsb)) sleepNow();
    delay(20);
    return;
  }

  switch (screen) {
    case SCR_REC:       screenRecord(tap);   break;
    case SCR_NOTES:     screenNotes(tap);    break;
    case SCR_NOTE:      screenNote(tap);     break;
    case SCR_TASKS:     screenTasks(tap);    break;
    case SCR_WIFI:      screenWifi(tap);     break;
    case SCR_WIFI_PASS: screenWifiPass(tap); break;
    case SCR_MORE:      screenMore(tap);     break;
    case SCR_STORAGE:   screenStorage(tap); break;
    case SCR_FACTORY:   screenFactory(tap); break;
    case SCR_BATTERY:   screenBattery(tap); break;
    case SCR_TIME:      screenTime(tap);    break;
    case SCR_DISPLAY:   screenDisplay(tap); break;
    case SCR_USBDRIVE:  screenUsbDrive(tap); break;
    default:            screenRecord(tap);   break;
  }

  /* The keyboard fills the screen, so the tab bar would be under a finger. */
  if (screen == SCR_INTRO) {
    screenIntro(tap);
    dispShow();
    delay(20);
    return;
  }

  if (screen != SCR_WIFI_PASS && screen != SCR_STORAGE
      && screen != SCR_FACTORY && screen != SCR_BATTERY && screen != SCR_TIME
      && screen != SCR_DISPLAY && screen != SCR_USBDRIVE) {
    const int hit = uiTabBar(tap, activeTab, TABS, 5);
    if (hit >= 0) {
      activeTab = hit;
      screen = (Screen)TAB_SCREEN[hit];
      statusLine = "";
    }
  }

  /* The record button is the one target big enough to press without looking,
     so it is handled after drawing, when its position is known. */
  if (screen == SCR_REC && tap.happened && !recording) {
    const int dx = tap.x - CX, dy = tap.y - 232;
    if (dx * dx + dy * dy <= 72 * 72) startRecording();
  }

  dispShow();

  if (cfgAutoSync && millis() - lastSyncTry > SYNC_RETRY_MS
      && syncedCount < totalCount && !recording && !playActive()) {
    syncAll(false);
  }

  /* Dim for the last quarter of the timeout. Cheap, and it says sleep is
     coming rather than the screen simply vanishing mid-thought. */
  const uint32_t idle = millis() - lastActivity;
  const uint32_t limit = SLEEP_CHOICES[cfgSleep];
  setDim(idle > limit - limit / 4 && !playActive());

  if (idle > limit && !touchDown()
      && !playActive() && !usbDriveActive()
      && !(bootedOnUsb && cfgStayOnUsb)) sleepNow();

  delay(20);
}
