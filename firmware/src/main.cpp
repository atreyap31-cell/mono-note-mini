/* Mono Note Mini - a recorder, and nothing else.
 *
 * Tap the screen to start, tap it again to stop. The note is saved to the card
 * and goes up to the machine that serves the website the next time there is
 * Wi-Fi. No account, no repository, no token.
 *
 * Hardware: Waveshare ESP32-S3-Touch-AMOLED-2.16. A 480x480 colour AMOLED on
 * quad-SPI, a CST9220 touch panel, an ES7210 microphone array with an ES8311
 * for playback, and an AXP2101 looking after the battery.
 *
 * It replaced a 1.54" e-paper board with two physical buttons, and almost
 * nothing about the screen carried over. Three differences drive the whole
 * interface:
 *
 *   The panel is fast, so the elapsed time can simply tick. On e-paper a
 *   redraw took 2.7 seconds and flashed the whole display, so a recording
 *   showed a circle and nothing moved until it stopped.
 *
 *   The panel does not hold an image without power, so sleeping is now
 *   genuinely off. The old device slept showing something useful, for free.
 *
 *   Black costs nothing on an AMOLED, so the interface is light on black. That
 *   is the efficient choice as well as the better-looking one.
 *
 * The previous firmware, for the e-paper board, is at
 * T:\mnm-backup-epaper-final and will not run on this hardware.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <SD_MMC.h>
#include <esp_sleep.h>
#include <time.h>
#include <vector>
#include "user_config.h"
#include "pala_display.h"
#include "pala_touch.h"
#include "pala_power.h"
#include "pala_record.h"
#include "pala_net.h"
#include "pala_sync.h"
#include "pala_rtc.h"
#include "pala_ble.h"
#include "logo_mn.h"
#include "soc/usb_serial_jtag_struct.h"

#define IDLE_SLEEP_MS   45000UL     /* awake this long with nothing happening */
#define SYNC_RETRY_MS  300000UL     /* how often to try again after a failure */

/* The recorder's buffer holds two minutes and its task simply stops when it is
   full. Left to that, the screen would go on saying RECORDING over a
   microphone that had stopped listening. */
#define MAX_NOTE_SECONDS 119

#define CX (LCD_WIDTH / 2)
#define CY (LCD_HEIGHT / 2)

static bool     recording = false;
static uint32_t recStarted = 0;
static uint32_t lastActivity = 0;
static uint32_t lastSyncTry = 0;
static int      syncedCount = 0, totalCount = 0;
static String   statusLine;

/* ---- counting what has gone up ------------------------------------------
   A marker file beside each note records that it reached the server, so the
   count survives a reboot. Per note rather than all-or-nothing: a sync that
   gets three of seven up shows three, and only the other four are retried. */

static bool isWav(const String& n) { return n.endsWith(".wav"); }

static String baseOf(const String& name) {
  String n = name;
  int slash = n.lastIndexOf('/');
  if (slash >= 0) n = n.substring(slash + 1);
  int dot = n.lastIndexOf('.');
  if (dot > 0) n = n.substring(0, dot);
  return n;
}

static void countNotes() {
  totalCount = 0;
  syncedCount = 0;
  File dir = SD_MMC.open("/recordings");
  if (!dir) return;
  File f;
  while ((f = dir.openNextFile())) {
    String n = f.name();
    f.close();
    if (!isWav(n)) continue;
    totalCount++;
    if (SD_MMC.exists("/recordings/" + baseOf(n) + ".synced")) syncedCount++;
  }
  dir.close();
}

/* ---- the screen --------------------------------------------------------- */

static void drawBattery() {
  const int pct = powerPercent();
  const bool chg = powerCharging();
  const int x = CX - 40, y = 34, w = 64, h = 26;

  uint16_t colour = COL_GREEN;
  if (pct >= 0 && pct < 15)      colour = COL_RED;
  else if (pct >= 0 && pct < 40) colour = COL_AMBER;
  if (chg) colour = COL_GREEN;

  dispRoundRect(x, y, w, h, 6, COL_FAINT, false);
  dispFillRect(x + w + 3, y + 8, 4, 10, COL_FAINT);   /* the terminal nub */
  if (pct > 0) {
    int fill = (w - 6) * pct / 100;
    if (fill < 3) fill = 3;
    dispRoundRect(x + 3, y + 3, fill, h - 6, 3, colour, true);
  }
  String label = (pct < 0) ? "--" : String(pct);
  dispText(x + w + 14, y + 6, label + "%", 2, COL_DIM);
  if (chg) dispText(x - 22, y + 6, "+", 2, COL_GREEN);
}

static void drawSyncLine(int y) {
  String counter;
  uint16_t colour;
  if (totalCount == 0)                { counter = "no notes yet";  colour = COL_DIM; }
  else if (syncedCount >= totalCount) { counter = "all synced";    colour = COL_GREEN; }
  else {
    counter = String(syncedCount) + " of " + String(totalCount) + " synced";
    colour = COL_AMBER;
  }
  dispTextCentered(y, counter, 2, colour);
}

/* The idle screen. One obvious thing to press, filling the middle of a
   480-pixel panel, because there is only ever one thing to do. */
static void drawIdle() {
  dispClear(COL_BLACK);
  drawBattery();
  dispBitmap1(CX - LOGO_W / 2, 92, LOGO_W, LOGO_H, LOGO_MN, COL_WHITE);

  dispCircle(CX, CY + 58, 96, COL_FAINT);
  dispCircle(CX, CY + 58, 95, COL_FAINT);
  dispFillCircle(CX, CY + 58, 74, COL_BLUE);
  dispTextCentered(CY + 46, "TAP TO", 2, COL_WHITE);
  dispTextCentered(CY + 68, "RECORD", 2, COL_WHITE);

  drawSyncLine(LCD_HEIGHT - 74);
  if (statusLine.length()) dispTextCentered(LCD_HEIGHT - 44, statusLine, 1, COL_DIM);
  dispShow();
}

static String mmss(uint32_t secs) {
  char buf[8];
  snprintf(buf, sizeof(buf), "%u:%02u", (unsigned)(secs / 60), (unsigned)(secs % 60));
  return String(buf);
}

/* Recording. The ring fills as the two minutes run down, so the limit is
   visible before it arrives rather than announced when it does. */
static void drawRecording(uint32_t secs) {
  dispClear(COL_BLACK);
  drawBattery();

  const int r = 104;
  dispCircle(CX, CY + 40, r, COL_FAINT);
  float frac = (float)secs / (float)MAX_NOTE_SECONDS;
  if (frac > 1.0f) frac = 1.0f;
  /* Starts at the top and goes clockwise, which is the direction everyone
     reads a dial in. */
  dispArc(CX, CY + 40, r, 7, -90.0f, -90.0f + 360.0f * frac,
          frac > 0.9f ? COL_AMBER : COL_RED);

  dispFillCircle(CX, CY + 40, 66, COL_RED);
  dispTextCentered(CY + 26, mmss(secs), 3, COL_WHITE);
  dispTextCentered(CY + 56, "TAP TO STOP", 1, COL_WHITE);

  dispTextCentered(LCD_HEIGHT - 74, "recording", 2, COL_RED);
  dispShow();
}

static void drawSaved() {
  dispClear(COL_BLACK);
  drawBattery();
  dispFillCircle(CX, CY + 40, 66, COL_GREEN);
  /* A tick, drawn as two strokes rather than carried as a glyph. */
  for (int t = 0; t < 8; t++) {
    dispFillCircle(CX - 26 + t * 2, CY + 40 + t * 2, 4, COL_WHITE);
    if (t < 16) dispFillCircle(CX - 10 + t * 3, CY + 56 - t * 3, 4, COL_WHITE);
  }
  dispTextCentered(LCD_HEIGHT - 74, "saved", 2, COL_GREEN);
  dispShow();
}

static void showMessage(const String& a, const String& b, uint16_t colour) {
  dispClear(COL_BLACK);
  dispTextCentered(CY - 30, a, 3, colour);
  if (b.length()) dispTextCentered(CY + 20, b, 2, COL_DIM);
  dispShow();
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
  /* No clock and no sync yet. Say so, rather than emitting a number that looks
     like a timestamp, sorts wrongly and means nothing. */
  return "rec_noclock_" + String(millis() / 1000) + ".wav";
}

/* ---- sync --------------------------------------------------------------- */

static void unsyncedBases(std::vector<String>& out) {
  File dir = SD_MMC.open("/recordings");
  if (!dir) return;
  File f;
  while ((f = dir.openNextFile())) {
    String n = f.name();
    f.close();
    if (!isWav(n)) continue;
    String b = baseOf(n);
    if (!SD_MMC.exists("/recordings/" + b + ".synced")) out.push_back(b);
  }
  dir.close();
}

static void markSynced(const String& base) {
  File m = SD_MMC.open("/recordings/" + base + ".synced", "w");
  if (m) { m.print("1"); m.flush(); m.close(); }
}

static bool trySync(bool sayWhy) {
  lastSyncTry = millis();
  if (totalCount == 0 || syncedCount >= totalCount) return false;
  if (!syncConfigured())            { if (sayWhy) statusLine = "set up on the website"; return false; }
  if (netGet("ssid").length() == 0) { if (sayWhy) statusLine = "no wi-fi set"; return false; }

  statusLine = "syncing...";
  drawIdle();

  if (!staConnect(20000)) {
    statusLine = sayWhy ? "no wi-fi" : "";
    return false;
  }

  std::vector<String> todo;
  unsyncedBases(todo);
  int done = 0;
  String err;
  for (size_t i = 0; i < todo.size(); i++) {
    if (syncUploadNote(todo[i], err)) { markSynced(todo[i]); done++; }
    else break;                 /* the next will fail the same way */
  }
  staDisconnect();
  countNotes();
  statusLine = (done == (int)todo.size()) ? "" : (err.length() ? err.substring(0, 34) : "");
  return done > 0;
}

/* ---- USB ----------------------------------------------------------------
   Deep sleep switches off the USB-Serial-JTAG peripheral, so a plugged-in
   device drops off the bus and cannot be reflashed until somebody wakes it.
   Asked once at boot, while a host is unambiguously awake: Windows suspends a
   device no program has open, and a suspended bus sends nothing at all. */
static bool bootedOnUsb = false;

static bool usbSofSeen() {
  USB_SERIAL_JTAG.int_clr.sof_int_clr = 1;
  delay(4);
  return USB_SERIAL_JTAG.int_raw.sof_int_raw != 0;
}

/* ---- sleep -------------------------------------------------------------- */

static void sleepNow() {
  bleStop();
  showMessage("", "", COL_BLACK);
  dispSleep(true);
  touchSleep();
  delay(50);
  /* The touch panel's interrupt wakes it, so the screen itself is the wake
     button and there is nothing to learn. The user button and BOOT work too. */
  esp_sleep_enable_ext1_wakeup(
      (1ULL << TOUCH_INT_PIN) | (1ULL << USER_BUTTON_PIN) | (1ULL << BOOT_BUTTON_PIN),
      ESP_EXT1_WAKEUP_ANY_LOW);
  esp_deep_sleep_start();
}

/* ---- recording ---------------------------------------------------------- */

static void startRecording() {
  if (!recBegin()) { showMessage("MIC BUSY", "try again", COL_AMBER); delay(1200); drawIdle(); return; }
  recording = true;
  recStarted = millis();
  drawRecording(0);
}

static void stopRecording() {
  recording = false;
  String name = timestampName();
  bool ok = recSave("/recordings/" + name);
  if (ok) {
    drawSaved();
    delay(800);
  } else {
    showMessage("NOT SAVED", "card problem", COL_RED);
    delay(1800);
  }
  countNotes();
  trySync(false);
  drawIdle();
}

/* ---- setup and loop ----------------------------------------------------- */

void setup() {
  Serial.begin(115200);
  delay(150);

  /* The PMU comes first: it owns the rails everything else runs on, and the
     I2C bus it shares with the touch panel and the clock. */
  powerBegin();

  if (!dispBegin()) {
    /* Nothing can be reported on a screen that did not start, so the only
       useful thing left is not to hang. */
    delay(2000);
    ESP.restart();
  }
  dispClear(COL_BLACK);
  dispTextCentered(CY - 10, "starting", 2, COL_DIM);
  dispShow();

  touchBegin();
  if (rtcBegin()) rtcRestoreSystemTime();

  /* One-bit SDIO, as on the old board, with different pins. DAT3 is driven
     high rather than left floating: some cards read a floating DAT3 as a
     request for SPI mode and then never answer. */
  pinMode(SDMMC_DAT3_PIN, OUTPUT);
  digitalWrite(SDMMC_DAT3_PIN, HIGH);
  SD_MMC.setPins(SDMMC_CLK_PIN, SDMMC_CMD_PIN, SDMMC_D0_PIN);
  if (!SD_MMC.begin("/sdcard", true)) {
    showMessage("NO SD CARD", "nothing can be saved", COL_RED);
    delay(3000);
  }
  SD_MMC.mkdir("/recordings");

  netBegin();
  applyTimezone();
  audioReady();

  for (int i = 0; i < 8 && !bootedOnUsb; i++) {
    if (usbSofSeen()) bootedOnUsb = true;
    delay(120);
  }

  countNotes();
  drawIdle();

  /* Bluetooth is how the website sets Wi-Fi, and there is no control for it.
     It runs whenever the device is awake. */
  bleBegin();

  trySync(true);
  drawIdle();
  lastActivity = millis();
}

void loop() {
  int tx = 0, ty = 0;
  const bool tapped = touchTapped(&tx, &ty);
  if (tapped || touchDown()) lastActivity = millis();

  /* The whole interface. Anywhere on the screen, because there is one thing
     to do and hunting for a target is not part of catching a thought. */
  if (tapped) {
    if (recording) stopRecording();
    else           startRecording();
  }

  if (recording) {
    uint32_t secs = (millis() - recStarted) / 1000;
    if (recSeconds() >= MAX_NOTE_SECONDS || secs >= MAX_NOTE_SECONDS) {
      stopRecording();
      showMessage("TWO MINUTES", "saved - that is the most", COL_AMBER);
      delay(1600);
      drawIdle();
      return;
    }
    /* Once a second is enough; the only thing moving is the clock. */
    static uint32_t lastDrawn = 0;
    if (secs != lastDrawn) { lastDrawn = secs; drawRecording(secs); }
    delay(20);
    return;
  }

  if (millis() - lastSyncTry > SYNC_RETRY_MS && syncedCount < totalCount) {
    if (trySync(false)) drawIdle();
  }

  if (millis() - lastActivity > IDLE_SLEEP_MS && !touchDown() && !bootedOnUsb) sleepNow();

  delay(20);
}
