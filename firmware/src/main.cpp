/* Mono Note Mini - a recorder, and nothing else.
 *
 * One button. Press it to start, press it again to stop. The note is saved to
 * the card, and goes up to the machine that serves the website the next
 * time there is Wi-Fi. No account, no repository, no token.
 *
 * There are no menus, no settings and no second button. Everything that used
 * to be on the device - browsing notes, tagging, to-dos, Wi-Fi setup, the PIN,
 * voice unlock, transcription - either lives on the website now or is gone.
 * The previous version is backed up in full at T:\mnm-backup-2026-09-10.
 *
 * The one thing the device cannot do without is Wi-Fi credentials, and there
 * is nowhere to type them. So it advertises over Bluetooth whenever it is
 * awake and not recording, and the website writes them in. That needs no
 * button, which is the point.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <SD_MMC.h>
#include <esp_sleep.h>
#include <time.h>
#include <vector>
#include "user_config.h"
#include "i2c_bsp.h"
#include "pala_input.h"
#include "board_power_bsp.h"
#include "epaper_driver_bsp.h"
#include "pala_ui.h"
#include "pala_record.h"
#include "pala_net.h"
#include "pala_sync.h"
#include "pala_rtc.h"
#include "pala_ble.h"
#include "logo_mn.h"
#include "soc/usb_serial_jtag_struct.h"

#define BAT_ADC_PIN 4
#define BAT_EMPTY_MV 3300
#define BAT_FULL_MV 4200

#define IDLE_SLEEP_MS   30000UL     /* awake this long with nothing happening */
#define SYNC_RETRY_MS  300000UL     /* how often to try again after a failure */

/* The recorder's buffer holds two minutes and its task simply stops when it is
   full. Left to that, the screen would still say RECORDING over a microphone
   that had stopped listening, so the cap is enforced here where it can be
   said out loud. */
#define MAX_NOTE_SECONDS 119

static board_power_bsp_t pwr(EPD_PWR_PIN, Audio_PWR_PIN, VBAT_PWR_PIN);
static I2cMasterBus* i2c = nullptr;
static epaper_driver_display* epd = nullptr;

static bool recording = false;
static uint32_t lastActivity = 0;
static uint32_t lastSyncTry = 0;
static int syncedCount = 0, totalCount = 0;
static String statusLine;          /* shown under the counter when it matters */

/* ---- battery ------------------------------------------------------------ */

static int batteryPct() {
  /* analogReadMilliVolts applies the chip's factory ADC calibration from
     eFuse. The raw-count conversion this replaced ignored it, and the S3's ADC
     is non-linear enough for that to be worth over 100mV - most of a quarter
     on a 3.3-4.2V cell. The divider is 2x. */
  analogReadMilliVolts(BAT_ADC_PIN);
  analogReadMilliVolts(BAT_ADC_PIN);
  uint32_t sum = 0;
  for (int i = 0; i < 8; i++) { sum += analogReadMilliVolts(BAT_ADC_PIN); delay(2); }
  uint32_t mv = (sum / 8) * 2;
  if (mv > 4650) return -1;                  /* on charge, or no cell fitted */
  int pct = (int)(mv - BAT_EMPTY_MV) * 100 / (BAT_FULL_MV - BAT_EMPTY_MV);
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  return pct;
}

/* Four steps. A percentage on a 1-bit panel invites re-reading a number that
   has not changed, and four steps is all anyone reads off a battery gauge. */
static int batteryQuarter() {
  int pct = batteryPct();
  if (pct < 0)  return 4;                    /* charging shows as full */
  if (pct >= 75) return 4;
  if (pct >= 50) return 3;
  if (pct >= 25) return 2;
  if (pct >= 10) return 1;
  return 0;
}

static void drawBatteryBar(int quarter) {
  const int x = 20, y = 10, w = 160, h = 14;
  uiRect(x, y, w, h);
  uiFillRect(x + w, y + 4, 3, 6, 0x00);      /* the little terminal nub */
  const int cell = (w - 4) / 4;
  for (int i = 0; i < 4; i++) {
    int cx = x + 2 + i * cell;
    if (i < quarter) uiFillRect(cx + 1, y + 3, cell - 2, h - 6, 0x00);
    if (i > 0)       uiFillRect(cx, y + 1, 1, h - 2, 0x00);   /* divider */
  }
}

/* ---- counting what has gone up ------------------------------------------
   A marker file beside each note records that it reached the server. That is
   what makes the count survive a reboot, and it is what the counter on screen
   reads. Per note rather than all-or-nothing: a sync that gets three of seven
   up shows three, and only the other four are tried again. */

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

/* ---- the screen ---------------------------------------------------------
   There is one, and it is what the device shows awake and what it leaves on
   the glass asleep. Waking changes nothing, so there is no second version to
   disagree with the first. */

static void drawScreen() {
  uiFillRect(0, 0, 200, 200, 0xff);
  drawBatteryBar(batteryQuarter());
  uiBitmap((200 - LOGO_W) / 2, 56, LOGO_W, LOGO_H, LOGO_MN);

  String counter;
  if (totalCount == 0)               counter = "no notes yet";
  else if (syncedCount >= totalCount) counter = "all synced";
  else counter = String(syncedCount) + " of " + String(totalCount) + " synced";
  uiTextCentered(140, counter, 2);

  if (statusLine.length()) uiTextCentered(166, statusLine, 1);
  else                     uiTextCentered(166, "press to record", 1);
  uiFlushFull();
}

/* Recording deliberately does not redraw. A full refresh takes about 2.7
   seconds and flashes the whole panel, which is unusable as a live meter and
   pointless besides - you know you are talking. A circle means recording, a
   square means it stopped, and nothing moves in between. */
static void drawRecordingMark(bool square) {
  uiFillRect(0, 0, 200, 200, 0xff);
  drawBatteryBar(batteryQuarter());
  if (square) uiFillRect(70, 60, 60, 60, 0x00);
  else        uiFillCircle(100, 90, 32, 0x00);
  uiTextCentered(150, square ? "SAVED" : "RECORDING", 2);
  uiTextCentered(176, square ? "" : "press to stop", 1);
  uiFlushFull();
}

static void showMessage(const String& a, const String& b) {
  uiFillRect(0, 0, 200, 200, 0xff);
  uiTextCentered(80, a, 2);
  if (b.length()) uiTextCentered(112, b, 1);
  uiFlushFull();
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

/* Returns true when anything went up. Quiet about there being no Wi-Fi yet:
   that is the normal state of a device in a pocket, not a fault worth putting
   on screen. */
static bool trySync(bool sayWhy) {
  lastSyncTry = millis();
  if (totalCount == 0 || syncedCount >= totalCount) return false;
  if (!syncConfigured())            { if (sayWhy) statusLine = "set up on the website"; return false; }
  if (netGet("ssid").length() == 0) { if (sayWhy) statusLine = "no wi-fi set"; return false; }

  statusLine = "syncing...";
  drawScreen();

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

  /* Whatever went wrong, in the words the server or the stack used. Silence
     here once cost an evening of guessing. */
  statusLine = (done == (int)todo.size()) ? "" : (err.length() ? err.substring(0, 24) : "");
  return done > 0;
}

/* ---- USB ---------------------------------------------------------------
   Deep sleep switches off the USB-Serial-JTAG peripheral, so a plugged-in
   device drops off the bus and cannot be reflashed until somebody presses a
   button. Asked once at boot, when a host is unambiguously awake: Windows
   suspends a device no program has open, and a suspended bus sends nothing at
   all, so asking later reads as "no USB" exactly when the answer needs to be
   yes. */
static bool bootedOnUsb = false;

static bool usbSofSeen() {
  USB_SERIAL_JTAG.int_clr.sof_int_clr = 1;
  delay(4);
  return USB_SERIAL_JTAG.int_raw.sof_int_raw != 0;
}

/* ---- sleep -------------------------------------------------------------- */

static void sleepNow() {
  bleStop();
  /* Exactly the same screen it shows awake, so what it leaves on the glass is
     what it wakes up to - byte for byte, not merely similar. */
  statusLine = "";
  drawScreen();
  delay(300);
  pwr.POWEER_Audio_OFF();
  pwr.POWEER_EPD_OFF();
  /* Either button wakes it. Only the top one does anything afterwards, but
     waking on the button you happen to press is kinder than making people
     learn which one is allowed to. */
  esp_sleep_enable_ext1_wakeup(
      (1ULL << BOOT_BUTTON_PIN) | (1ULL << PWR_BUTTON_PIN), ESP_EXT1_WAKEUP_ANY_LOW);
  esp_deep_sleep_start();
}

/* ---- recording ---------------------------------------------------------- */

/* Bluetooth is deliberately left running through a recording. Stopping and
   restarting it around one would be worse than leaving it: bleStop only clears
   the advertising flag, so the next bleBegin runs BLEDevice::init again,
   builds a second server and service on top of the first, and starts another
   bleTask. Every note would leak one. */
static void startRecording() {
  if (!recBegin()) { showMessage("MIC BUSY", "try again"); delay(1200); drawScreen(); return; }
  recording = true;
  drawRecordingMark(false);
}

static void stopRecording() {
  recording = false;
  String name = timestampName();
  bool ok = recSave("/recordings/" + name);
  drawRecordingMark(true);
  delay(700);
  if (!ok) {
    showMessage("NOT SAVED", "card problem");
    delay(1500);
  }
  countNotes();
  trySync(false);
  drawScreen();
}

/* ---- setup and loop ----------------------------------------------------- */

void setup() {
  Serial.begin(115200);
  delay(200);
  analogSetAttenuation(ADC_11db);
  pwr.VBAT_POWER_ON();
  pwr.POWEER_EPD_ON();
  /* The panel's rail is switched by a GPIO and needs time to come up. A panel
     that is not ready yet holds BUSY high, which used to hang setup() with the
     previous image still on the glass. */
  delay(200);

  i2c = I2cMasterBus::requestInstance(ESP32_I2C_SCL_PIN, ESP32_I2C_SDA_PIN, ESP32_I2C_DEV_NUM);
  /* The clock kept running while the device was off. Ask it before anything
     needs a timestamp, so a note made before any sync still gets a real name. */
  if (rtcBegin()) rtcRestoreSystemTime();
  inputBegin();

  epd = new epaper_driver_display(EPD_WIDTH, EPD_HEIGHT,
      {EPD_CS_PIN, EPD_DC_PIN, EPD_RST_PIN, EPD_BUSY_PIN, EPD_MOSI_PIN, EPD_SCK_PIN,
       EPD_SPI_NUM, EPD_WIDTH * EPD_HEIGHT / 8});
  epd->EPD_Init();
  /* One retry: the rail coming up late is the likely reason a first attempt
     fails, and by the second the extra delay has usually settled it. */
  if (!epd->panelResponded()) {
    pwr.POWEER_EPD_OFF();
    delay(500);
    pwr.POWEER_EPD_ON();
    delay(600);
    epd->EPD_Init();
  }
  uiBegin(epd);

  SD_MMC.setPins(SDMMC_CLK_PIN, SDMMC_CMD_PIN, SDMMC_D0_PIN);
  if (!SD_MMC.begin("/sdcard", true)) {
    showMessage("NO SD CARD", "nothing can be saved");
    delay(3000);
  }
  SD_MMC.mkdir("/recordings");

  netBegin();
  applyTimezone();
  pwr.POWEER_Audio_ON();
  audioReady();

  /* One look for a USB host, while it is unambiguously awake. */
  for (int i = 0; i < 8 && !bootedOnUsb; i++) {
    if (usbSofSeen()) bootedOnUsb = true;
    delay(120);
  }

  countNotes();
  drawScreen();

  /* Bluetooth is how the website sets Wi-Fi and the token, and there is no
     button to turn it on with. It runs whenever the device is awake. */
  bleBegin();

  trySync(true);
  drawScreen();
  lastActivity = millis();
}

void loop() {
  uint16_t ev = inputPoll();

  if (ev & BTN_ANY_DOWN) lastActivity = millis();

  /* The whole interface. */
  if (ev & BTN_TOP_TAP) {
    lastActivity = millis();
    if (recording) stopRecording();
    else           startRecording();
  }

  if (recording) {
    if (recSeconds() >= MAX_NOTE_SECONDS) {
      stopRecording();
      showMessage("TWO MINUTES", "saved - that is the most");
      delay(1600);
      drawScreen();
    }
    delay(20);
    return;
  }

  /* Retry a failed or postponed sync now and then, so a device that comes back
     into Wi-Fi range catches up without being touched. */
  if (millis() - lastSyncTry > SYNC_RETRY_MS && syncedCount < totalCount) {
    if (trySync(false)) drawScreen();
  }

  if (millis() - lastActivity > IDLE_SLEEP_MS && !inputAnyHeld() && !bootedOnUsb) sleepNow();

  delay(20);
}
