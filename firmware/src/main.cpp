#include <Arduino.h>
#include <WiFi.h>
#include <SD_MMC.h>
#include <esp_sleep.h>
#include <time.h>
#include <vector>
#include "user_config.h"
#include "i2c_bsp.h"
#include "ft6336_bsp.h"
#include "board_power_bsp.h"
#include "epaper_driver_bsp.h"
#include "audio_bsp.h"
#include "pala_ui.h"
#include "pala_record.h"
#include "pala_net.h"

static board_power_bsp_t pwr(EPD_PWR_PIN, Audio_PWR_PIN, VBAT_PWR_PIN);
static I2cMasterBus* i2c = nullptr;
static I2cFt6336Dev* touch = nullptr;
static epaper_driver_display* epd = nullptr;

enum State { ST_MENU, ST_REC, ST_SYNC, ST_PORTAL };
static State state = ST_MENU;

static uint32_t touchDownAt = 0;
static int downX = -1, downY = -1;
static bool longFired = false;
static uint32_t lastActivity = 0;

static int recCount = 0;
static volatile bool syncCancel = false;

static void sleepNow() {
  uiFillRect(0, 0, 200, 200, 0xff);
  uiTextCentered(90, "sleeping - press BOOT", 1);
  uiFlushFull();
  delay(300);
  pwr.POWEER_Audio_OFF();
  pwr.POWEER_EPD_OFF();
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);
  esp_deep_sleep_start();
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

static void drawMenu() {
  epd->EPD_Clear();
  uiTextCentered(8, "PALA NOTE", 2);
  uiRect(0, 30, 200, 1);
  uiFillRect(10, 45, 180, 40, 0x00);
  uiTextCentered(60, "REC", 2, 0xff);
  uiRect(10, 95, 180, 30);
  uiTextCentered(104, "SYNC  (" + String(recCount) + " clips)", 1);
  uiRect(10, 135, 180, 30);
  uiTextCentered(144, "TRANSFER", 1);
  uiTextCentered(180, "hold = sleep", 1, 0x00);
  uiFlushFull();
}

static void drawRecScreen() {
  epd->EPD_Clear();
  uiTextCentered(20, "RECORDING", 2);
  uiRect(0, 48, 200, 1);
  uiFillRect(10, 80, 180, 60, 0x00);
  uiTextCentered(102, "0:00", 3, 0xff);
  uiTextCentered(170, "tap to stop", 1);
  uiFlushFull();
  uiFlushPartialPrepare();
}

static void updateRecTimer() {
  uint32_t s = recSeconds();
  String t = String(s / 60) + ":" + String(s % 60 < 10 ? "0" : "") + String(s % 60);
  uiFillRect(10, 80, 180, 60, 0x00);
  uiTextCentered(102, t, 3, 0xff);
  uiFlushPartial();
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

static void drawPortalScreen(const String& ssid) {
  epd->EPD_Clear();
  uiTextCentered(16, "TRANSFER MODE", 2);
  uiRect(0, 44, 200, 1);
  uiTextCentered(64, "Wi-Fi: " + ssid, 1);
  uiTextCentered(84, "key: record123", 1);
  uiTextCentered(112, WiFi.softAPIP().toString(), 2);
  uiTextCentered(150, "open in a browser", 1);
  uiTextCentered(170, "tap = exit", 1);
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
  state = ST_REC;
  drawRecScreen();
  return true;
}

static void stopRecording() {
  String name = timestampName();
  bool ok = recSave("/recordings/" + name);
  countRecordings();
  state = ST_MENU;
  drawMenu();
  if (!ok) showError("save failed - SD?");
}

static void syncAll() {
  String api = netGet("api");
  if (!api.length()) {
    showError("set API in TRANSFER");
    return;
  }
  state = ST_SYNC;
  syncCancel = false;
  drawSyncScreen(0, 0, "connecting wifi...");
  if (!staConnect(20000)) {
    state = ST_MENU;
    drawMenu();
    showError("wifi failed");
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
  staDisconnect();
  state = ST_MENU;
  drawMenu();
  uiTextCentered(188, done > 0 ? "synced " + String(done) : "nothing synced", 1);
  uiFlushPartial();
  delay(2500);
  drawMenu();
}

static void pollTouch(bool& tap, bool& longPress, int& tx, int& ty) {
  tap = false;
  longPress = false;
  uint16_t x, y;
  bool pressed = touch->GetTouchPoint(&x, &y);
  if (pressed && touchDownAt == 0) {
    touchDownAt = millis();
    downX = x; downY = y;
    longFired = false;
    lastActivity = millis();
  } else if (pressed && touchDownAt) {
    lastActivity = millis();
    if (!longFired && millis() - touchDownAt > 900) {
      longFired = true;
      longPress = true;
    }
  } else if (!pressed && touchDownAt) {
    uint32_t held = millis() - touchDownAt;
    int moved = abs((int)x - downX) + abs((int)y - downY);
    if (!longFired && held < 600 && moved < 40) {
      tap = true;
      tx = downX; ty = downY;
    }
    touchDownAt = 0;
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  pwr.VBAT_POWER_ON();
  pwr.POWEER_EPD_ON();
  i2c = I2cMasterBus::requestInstance(ESP32_I2C_SCL_PIN, ESP32_I2C_SDA_PIN, ESP32_I2C_DEV_NUM);
  touch = I2cFt6336Dev::requestInstance(i2c->Get_I2cBusHandle(), I2C_FT6336_DEV_Address, EPD_WIDTH, EPD_HEIGHT);
  touch->Ft6336_Reset(EPD_TP_RST_PIN);
  epd = new epaper_driver_display(EPD_WIDTH, EPD_HEIGHT,
      {EPD_CS_PIN, EPD_DC_PIN, EPD_RST_PIN, EPD_BUSY_PIN, EPD_MOSI_PIN, EPD_SCK_PIN, EPD_SPI_NUM, EPD_WIDTH * EPD_HEIGHT / 8});
  epd->EPD_Init();
  uiBegin(epd);

  if (!SD_MMC.setPins(SDMMC_CLK_PIN, SDMMC_CMD_PIN, SDMMC_D0_PIN)) {
    showError("sd pins rejected");
  }
  if (!SD_MMC.begin("/sdcard", true)) {
    showError("no SD card");
  }
  SD_MMC.mkdir("/recordings");
  netBegin();
  pwr.POWEER_Audio_ON();
  audio_bsp_init();
  countRecordings();
  drawMenu();
  lastActivity = millis();
}

void loop() {
  bool tap, longPress;
  int tx, ty;
  pollTouch(tap, longPress, tx, ty);

  switch (state) {
    case ST_MENU:
      if (longPress) { sleepNow(); break; }
      if (tap) {
        if (ty < 88) startRecording();
        else if (ty < 132) syncAll();
        else {
          state = ST_PORTAL;
          drawPortalScreen(portalStart());
        }
      }
      if (millis() - lastActivity > 60000 && touchDownAt == 0) sleepNow();
      break;

    case ST_REC: {
      static uint32_t lastTick = 0;
      recPoll();
      if (millis() - lastTick > 1000) { updateRecTimer(); lastTick = millis(); }
      if (tap || longPress) stopRecording();
      break;
    }

    case ST_SYNC:
      if (tap) syncCancel = true;
      break;

    case ST_PORTAL:
      portalPoll();
      if (tap || longPress) {
        portalStop();
        state = ST_MENU;
        drawMenu();
      }
      break;
  }
  delay(10);
}

