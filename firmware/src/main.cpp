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
#include "logo_mn.h"
#include "soc/usb_serial_jtag_struct.h"

#define IDLE_SLEEP_MS   60000UL
#define SYNC_RETRY_MS  300000UL
#define MAX_NOTE_SECONDS 119
#define CX (LCD_WIDTH / 2)

enum Screen {
  SCR_LOCK, SCR_REC, SCR_NOTES, SCR_NOTE, SCR_TASKS,
  SCR_WIFI, SCR_WIFI_PASS, SCR_MORE, SCR_PASSCODE,
  SCR_STORAGE, SCR_FACTORY, SCR_INTRO, SCR_BATTERY
};

static const char* TABS[] = { "NOTES", "RECORD", "TASKS", "WI-FI", "MORE" };
static const int   TAB_SCREEN[] = { SCR_NOTES, SCR_REC, SCR_TASKS, SCR_WIFI, SCR_MORE };

static const char* TAGS[5] = { "idea", "reminder", "task", "journal", "none" };

static Screen  screen = SCR_LOCK;
static int     activeTab = 1;
static uint32_t lastActivity = 0;
static uint32_t lastSyncTry = 0;
static bool     bootedOnUsb = false;

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
  notePager.perPage = 5;
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

static String clockNow() {
  struct tm t;
  if (!getLocalTime(&t, 100)) return "";
  char buf[8];
  snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
  return String(buf);
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
    dispBitmap1(CX - LOGO_W / 2, 92, LOGO_W, LOGO_H, LOGO_MN, COL_WHITE);
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

  const int top = UI_HEADER_H + 54, rowH = 52;
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
  uiPagerBar(t, notePager, LCD_HEIGHT - UI_TABBAR_H - 54);
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
  const int top = UI_HEADER_H + 10, rowH = 54;
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
  uiPagerBar(t, taskPager, LCD_HEIGHT - UI_TABBAR_H - 56);
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
    wifiPager.perPage = 4;
    wifiPager.page = 0;
    wifiNote = wifiNames.empty() ? "nothing in range" : "";
  }
  if (uiButton(t, LCD_WIDTH - UI_PAD - 210, 96, 210, 58, "ROUTER BUTTON", COL_DIM, false)) {
    wifiNote = netWpsStart() ? "press WPS on the router" : "could not start WPS";
  }

  const int wps = netWpsState();
  if (wps == 2) wifiNote = "joined by router button";
  else if (wps == 3) wifiNote = "router button not pressed in time";

  const int top = 168, rowH = 50;
  for (int i = 0; i < wifiPager.perPage; i++) {
    const int idx = wifiPager.first() + i;
    if (idx >= (int)wifiNames.size()) break;
    if (uiRow(t, top + i * (rowH + 6), rowH, wifiNames[idx], "", COL_DIM)) {
      wifiPick = wifiNames[idx];
      wifiPass = "";
      kbShift = false; kbSyms = false;
      screen = SCR_WIFI_PASS;
    }
  }
  uiPagerBar(t, wifiPager, LCD_HEIGHT - UI_TABBAR_H - 56);
  if (wifiNote.length()) dispTextCentered(LCD_HEIGHT - UI_TABBAR_H - 96, wifiNote, TXT_SMALL, COL_AMBER);
}

static void screenWifiPass(const UiTap& t) {
  dispClear(COL_BLACK);
  uiHeader(wifiPick.length() > 18 ? wifiPick.substring(0, 18) : wifiPick);
  uiField(UI_PAD, 72, LCD_WIDTH - UI_PAD * 2, wifiPass, "wi-fi password", false);
  if (wifiNote.length()) dispTextCentered(126, wifiNote, TXT_SMALL, COL_AMBER);

  char ch = 0;
  const KeyResult k = uiKeyboard(t, 152, kbShift, kbSyms, &ch);
  if (k == KEY_CHAR && wifiPass.length() < 63) {
    wifiPass += ch;
    if (kbShift && !kbSyms) kbShift = false;    /* shift is for one letter */
  } else if (k == KEY_BACKSPACE && wifiPass.length()) {
    wifiPass.remove(wifiPass.length() - 1);
  } else if (k == KEY_ENTER) {
    netSet("ssid", wifiPick);
    netSet("pass", wifiPass);
    wifiNote = "joining...";
    dispTextCentered(126, wifiNote, TXT_SMALL, COL_AMBER);
    dispShow();
    const bool ok = staConnect(20000);
    wifiNote = ok ? "joined" : "wrong password, or out of range";
    if (ok) { screen = SCR_WIFI; }
  }

  if (uiButton(t, UI_PAD, LCD_HEIGHT - 44, 120, 40, "cancel", COL_DIM, false)) {
    screen = SCR_WIFI;
  }
}

static void screenMore(const UiTap& t) {
  drawChrome("MORE");
  const bool ble = bleAdvertising();

  if (uiRow(t, 80, 58, ble ? "Bluetooth: on" : "Bluetooth: off",
            ble ? bleStatus() : "tap to turn on", ble ? COL_GREEN : COL_DIM)) {
    if (ble) bleStop(); else bleBegin();
  }
  if (uiRow(t, 146, 58, "Sync all notes",
            (syncedCount >= totalCount && totalCount) ? "all sent"
              : String(totalCount - syncedCount) + " waiting", COL_AMBER)) {
    /* Joining a network and uploading blocks for up to half a minute, and the
       whole interface is frozen while it does. Say so before starting, or the
       tap looks like it did nothing at all. */
    statusLine = "working...";
    dispTextCentered(352, statusLine, TXT_SMALL, COL_AMBER);
    dispShow();
    syncAll(true);
  }
  if (uiRow(t, 212, 58, "Change passcode", "", COL_DIM)) {
    codeForChange = true;
    codeStage = 0;
    codeEntry = ""; codeFirst = "";
    codeNote = "enter your current passcode";
    screen = SCR_PASSCODE;
  }
  if (uiRow(t, 278, 58, "Storage",
            String((uint32_t)(SD_MMC.usedBytes() / (1024ULL * 1024ULL))) + " MB", COL_DIM)) {
    freeStage = 0;
    screen = SCR_STORAGE;
  }
  if (uiRow(t, 344, 58, "Lock now", "", COL_DIM)) {
    lockRelock();
    codeEntry = ""; codeNote = "";
    screen = SCR_LOCK;
  }

  if (uiButton(t, LCD_WIDTH - UI_PAD - 140, 344, 140, 58, "RESET", COL_RED, false)) {
    factoryStage = 0;
    screen = SCR_FACTORY;
  }
  if (uiButton(t, UI_PAD, 344, 140, 58, "BATTERY", COL_BLUE, false)) {
    screen = SCR_BATTERY;
  }
  if (statusLine.length())
    dispTextCentered(LCD_HEIGHT - UI_TABBAR_H - 42, statusLine, TXT_SMALL, COL_AMBER);
  dispText(UI_PAD, LCD_HEIGHT - UI_TABBAR_H - 24, syncDeviceId(), TXT_SMALL, COL_DIM);
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

static bool usbSofSeen() {
  USB_SERIAL_JTAG.int_clr.sof_int_clr = 1;
  delay(4);
  return USB_SERIAL_JTAG.int_raw.sof_int_raw != 0;
}

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

  loadNotes();
  loadTasks();

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
  if (tap.happened || touchDown()) lastActivity = millis();

  playPoll();

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
    if (millis() - lastActivity > IDLE_SLEEP_MS && !bootedOnUsb) sleepNow();
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
      && screen != SCR_FACTORY && screen != SCR_BATTERY) {
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

  if (millis() - lastSyncTry > SYNC_RETRY_MS && syncedCount < totalCount
      && !recording && !playActive()) {
    syncAll(false);
  }

  if (millis() - lastActivity > IDLE_SLEEP_MS && !touchDown()
      && !playActive() && !bootedOnUsb) sleepNow();

  delay(20);
}
