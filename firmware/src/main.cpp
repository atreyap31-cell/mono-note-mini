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
#include "pala_ui.h"
#include "pala_record.h"
#include "pala_net.h"

static board_power_bsp_t pwr(EPD_PWR_PIN, Audio_PWR_PIN, VBAT_PWR_PIN);
static I2cMasterBus* i2c = nullptr;
static I2cFt6336Dev* touch = nullptr;
static epaper_driver_display* epd = nullptr;

enum State { ST_MENU, ST_REC, ST_TAG, ST_PLAY, ST_SYNC, ST_PORTAL };
static State state = ST_MENU;

static uint32_t touchDownAt = 0;
static int downX = -1, downY = -1, lastY = -1;
static bool longFired = false;
static bool pressed = false;
static uint32_t lastActivity = 0;

static int recCount = 0;
static volatile bool syncCancel = false;
static String lastSavedName = "";

static std::vector<String> playList;
static int playTop = 0;
static int playRow = -1;

static const char* TAGS[4] = {"idea", "task", "reminder", "project"};

static void sleepNow() {
  uiFillRect(0, 0, 200, 200, 0xff);
  uiTextCentered(90, "sleep - press button", 1);
  uiFlushFull();
  delay(300);
  pwr.POWEER_Audio_OFF();
  pwr.POWEER_EPD_OFF();
  esp_sleep_enable_ext1_wakeup((1ULL << GPIO_NUM_0) | (1ULL << GPIO_NUM_18), ESP_EXT1_WAKEUP_ANY_LOW);
  esp_deep_sleep_start();
}

static bool soundOn() { return netGet("sound", "1") == "1"; }

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

static void refreshPlayList() {
  playList.clear();
  File dir = SD_MMC.open("/recordings");
  File f;
  std::vector<String> all;
  while ((f = dir.openNextFile())) {
    String n = String(f.name());
    if (!n.startsWith("/")) n = "/" + n;
    if (n.endsWith(".wav")) all.push_back(n);
    f.close();
  }
  for (int i = all.size() - 1; i >= 0; i--) playList.push_back(all[i]);
}

static String tagOf(const String& wavName) {
  String t = "/recordings/" + wavName.substring(1, wavName.length() - 4) + ".tag";
  if (!SD_MMC.exists(t)) return "";
  File f = SD_MMC.open(t, "r");
  String v = f.readStringUntil('\n');
  f.close();
  v.trim();
  return v;
}

static void drawMenu() {
  epd->EPD_Clear();
  uiTextCentered(6, "PALA NOTE", 2);
  uiText(150, 12, String(recCount) + " clips", 1);
  uiRect(0, 28, 200, 1);
  uiFillRect(10, 38, 180, 42, 0x00);
  uiTextCentered(52, "HOLD TO REC", 2, 0xff);
  uiRect(10, 90, 180, 28);
  uiTextCentered(98, "SYNC", 1);
  uiRect(10, 126, 180, 28);
  uiTextCentered(134, "PLAY", 1);
  uiRect(10, 162, 180, 28);
  uiTextCentered(170, "SEND / SETTINGS", 1);
  uiFlushFull();
}

static void drawRecScreen() {
  epd->EPD_Clear();
  uiTextCentered(20, "RECORDING", 2);
  uiRect(0, 48, 200, 1);
  uiFillRect(10, 80, 180, 60, 0x00);
  uiTextCentered(102, "0:00", 3, 0xff);
  uiTextCentered(170, "release to save", 1);
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

static void drawTagScreen() {
  epd->EPD_Clear();
  uiTextCentered(10, "TAG IT", 2);
  uiTextCentered(34, "saved: " + lastSavedName.substring(0, 18), 1);
  const int xs[2] = {10, 104}, ys[2] = {52, 100};
  for (int i = 0; i < 4; i++) {
    uiRect(xs[i % 2], ys[i / 2], 86, 40);
    uiTextCentered(ys[i / 2] + 16, TAGS[i], 1);
  }
  uiTextCentered(160, "tap a tag", 1);
  uiTextCentered(178, "tap here to skip", 1);
  uiFlushFull();
}

static void drawPlayScreen() {
  epd->EPD_Clear();
  uiTextCentered(8, "PLAYBACK", 2);
  uiRect(0, 30, 200, 1);
  if (playList.empty()) uiTextCentered(90, "no recordings", 1);
  int rows = 6;
  for (int i = 0; i < rows && playTop + i < (int)playList.size(); i++) {
    int idx = playTop + i;
    String n = playList[idx];
    n = n.substring(1, n.length() - 4);
    if ((int)n.length() > 20) n = n.substring(0, 20);
    String tag = tagOf(playList[idx]);
    if (tag.length()) n += " #" + tag;
    if (idx == playRow) {
      uiFillRect(4, 36 + i * 24, 192, 24, 0x00);
      uiText(8, 42 + i * 24, (playActive() ? "> " : "| ") + n, 1, 0xff);
    } else {
      uiText(8, 42 + i * 24, n, 1);
    }
  }
  uiTextCentered(188, "tap=play  swipe=scroll  hold=back", 1);
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

static void drawPortalScreen(const String& ssid) {
  epd->EPD_Clear();
  uiTextCentered(16, "TRANSFER MODE", 2);
  uiRect(0, 44, 200, 1);
  uiTextCentered(64, "Wi-Fi: " + ssid, 1);
  uiTextCentered(84, "key: record123", 1);
  uiTextCentered(112, WiFi.softAPIP().toString(), 2);
  bool custom = SD_MMC.exists("/www/index.html");
  uiTextCentered(150, custom ? "your site is live" : "built-in page (no /www)", 1);
  uiTextCentered(178, "tap = exit", 1);
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

static void saveTag(const char* tag) {
  String t = "/recordings/" + lastSavedName.substring(1, lastSavedName.length() - 4) + ".tag";
  File f = SD_MMC.open(t, "w");
  if (f) { f.println(tag); f.close(); }
}

static void stopRecording() {
  lastSavedName = timestampName();
  bool ok = recSave("/recordings/" + lastSavedName);
  countRecordings();
  if (ok) {
    state = ST_TAG;
    drawTagScreen();
  } else {
    state = ST_MENU;
    drawMenu();
    showError("save failed - SD?");
  }
}

static void syncAll(bool autoRun) {
  String api = netGet("api");
  if (!api.length()) {
    if (!autoRun) showError("set API in SEND page");
    return;
  }
  state = ST_SYNC;
  syncCancel = false;
  drawSyncScreen(0, 0, autoRun ? "daily auto-sync" : "connecting wifi...");
  if (!staConnect(20000)) {
    state = ST_MENU;
    drawMenu();
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
  state = ST_MENU;
  drawMenu();
  String msg = syncCancel ? "sync cancelled" : (done > 0 ? "synced " + String(done) : "up to date");
  uiTextCentered(188, msg, 1);
  uiFlushPartial();
  delay(2500);
  drawMenu();
}

static void maybeAutoSync() {
  if (!netGet("ssid").length() || !netGet("api").length()) return;
  time_t now = time(nullptr);
  if (now < 1600000000) return;
  uint64_t last = netGetU64("lastSync", 0);
  if (now - (time_t)last < 86400) return;
  syncAll(true);
}

static void pollTouch(bool& tap, bool& longPress, bool& pressEvent, bool& releaseEvent, int& tx, int& ty, int& swipe) {
  tap = false; longPress = false; pressEvent = false; releaseEvent = false; swipe = 0;
  uint16_t x, y;
  bool down = touch->GetTouchPoint(&x, &y);
  if (down && !pressed) {
    pressed = true;
    touchDownAt = millis();
    downX = x; downY = y; lastY = y;
    longFired = false;
    pressEvent = true;
    tx = x; ty = y;
    lastActivity = millis();
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

  if (!SD_MMC.setPins(SDMMC_CLK_PIN, SDMMC_CMD_PIN, SDMMC_D0_PIN)) showError("sd pins rejected");
  if (!SD_MMC.begin("/sdcard", true)) showError("no SD card");
  SD_MMC.mkdir("/recordings");
  SD_MMC.mkdir("/www");
  netBegin();
  pwr.POWEER_Audio_ON();
  audioReady();
  countRecordings();
  drawMenu();
  lastActivity = millis();
  maybeAutoSync();
}

void loop() {
  bool tap, longPress, pressEvent, releaseEvent;
  int tx, ty, swipe;
  pollTouch(tap, longPress, pressEvent, releaseEvent, tx, ty, swipe);

  switch (state) {
    case ST_MENU:
      if (longPress) { sleepNow(); break; }
      if (pressEvent && ty >= 40 && ty <= 82) {
        if (soundOn()) beep();
        startRecording();
        break;
      }
      if (tap) {
        if (soundOn()) beep();
        if (ty >= 92 && ty <= 118) syncAll(false);
        else if (ty >= 128 && ty <= 154) {
          refreshPlayList();
          playTop = 0; playRow = -1;
          state = ST_PLAY;
          drawPlayScreen();
        } else if (ty >= 164) {
          state = ST_PORTAL;
          drawPortalScreen(portalStart());
        }
      }
      if (millis() - lastActivity > 60000 && !pressed) sleepNow();
      break;

    case ST_REC:
      recPoll();
      {
        static uint32_t lastTick = 0;
        if (millis() - lastTick > 1000) { updateRecTimer(); lastTick = millis(); }
      }
      if (releaseEvent || longPress) stopRecording();
      break;

    case ST_TAG:
      if (tap) {
        if (ty > 145) {
          beep();
          state = ST_MENU;
          drawMenu();
        } else {
          const char* pick = nullptr;
          if (tx < 100 && ty >= 54 && ty < 96) pick = TAGS[0];
          else if (tx >= 100 && ty >= 54 && ty < 96) pick = TAGS[1];
          else if (tx < 100 && ty >= 100 && ty < 142) pick = TAGS[2];
          else if (tx >= 100 && ty >= 100 && ty < 142) pick = TAGS[3];
          if (pick) {
            saveTag(pick);
            if (soundOn()) beep();
            state = ST_MENU;
            drawMenu();
          }
        }
      }
      break;

    case ST_PLAY:
      playPoll();
      if (longPress) {
        playStop();
        state = ST_MENU;
        drawMenu();
        break;
      }
      if (swipe == -1 && playTop > 0) { playTop--; drawPlayScreen(); }
      if (swipe == 1 && playTop + 6 < (int)playList.size()) { playTop++; drawPlayScreen(); }
      if (tap) {
        int row = (ty - 36) / 24;
        if (row >= 0 && row < 6 && playTop + row < (int)playList.size()) {
          int idx = playTop + row;
          if (idx == playRow && playActive()) {
            playStop();
            if (soundOn()) beep();
          } else {
            playRow = idx;
            if (playFile("/recordings" + playList[idx])) {
              if (soundOn()) beep();
            } else playRow = -1;
          }
          drawPlayScreen();
        }
      }
      break;

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

