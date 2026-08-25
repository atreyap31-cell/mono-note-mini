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

enum State { ST_HOME, ST_MENU, ST_MAKE, ST_TAG, ST_SETTINGS, ST_SET_WIFI, ST_SYNC, ST_STORAGE, ST_SET_IP, ST_VIEW_TAGS, ST_VIEW_LIST, ST_VIEW_NOTE };
static State state = ST_HOME;
static State syncReturnTo = ST_HOME;

static uint32_t touchDownAt = 0;
static int downX = -1, downY = -1, lastY = -1;
static bool longFired = false;
static bool pressed = false;
static uint32_t lastActivity = 0;
static uint32_t lastAutoCheck = 0;

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

static bool soundOn() { return netGet("sound", "1") == "1"; }

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
  uiTextCentered(88, "mono note", 2);
  uiFlushFull();
}

static void drawMenu() {
  epd->EPD_Clear();
  uiTextCentered(10, "mono note mini", 1);
  uiRect(0, 26, 200, 1);
  uiRect(10, 40, 180, 44);
  uiTextCentered(56, "VIEW NOTES", 2);
  uiRect(10, 94, 180, 44);
  uiTextCentered(110, "MAKE NOTE", 2);
  uiRect(10, 148, 180, 44);
  uiTextCentered(164, "SETTINGS", 2);
  uiFlushFull();
}

static void drawMake() {
  epd->EPD_Clear();
  uiTextCentered(14, "MAKE NOTE", 1);
  uiRect(0, 30, 200, 1);
  uiFillRect(10, 44, 180, 60, 0x00);
  uiTextCentered(66, "0:00", 3, 0xff);
  for (int i = 0; i < 16; i++) {
    int h = wave[i] * 6 / 10 + 2;
    uiFillRect(24 + i * 10, 150 - h / 2, 6, h, 0x00);
  }
  uiTextCentered(176, "tap to stop", 1);
  uiFlushFull();
  uiFlushPartialPrepare();
}

static void updateMake() {
  uint32_t s = recSeconds();
  String t = String(s / 60) + ":" + String(s % 60 < 10 ? "0" : "") + String(s % 60);
  uiFillRect(10, 44, 180, 60, 0x00);
  uiTextCentered(66, t, 3, 0xff);
  for (int i = 0; i < 16; i++) {
    int h = wave[i] * 6 / 10 + 2;
    uiFillRect(24 + i * 10, 150 - h / 2, 6, h, 0x00);
  }
  uiFlushPartial();
}

static void drawTagScreen() {
  epd->EPD_Clear();
  uiTextCentered(8, "SORT IT", 2);
  const char* labels[5] = {"Work", "Projects", "Ideas", "Quotes", "Random"};
  uiRect(10, 36, 86, 34);  uiTextCentered(47, labels[0], 1);
  uiRect(104, 36, 86, 34); uiTextCentered(47, labels[1], 1);
  uiRect(10, 78, 86, 34);  uiTextCentered(89, labels[2], 1);
  uiRect(104, 78, 86, 34); uiTextCentered(89, labels[3], 1);
  uiRect(10, 120, 180, 34); uiTextCentered(131, labels[4], 1);
  uiTextCentered(170, "tap a tag to file it", 1);
  uiTextCentered(186, "tap here to skip", 1);
  uiFlushFull();
}

static void drawSettings() {
  epd->EPD_Clear();
  uiTextCentered(10, "SETTINGS", 2);
  uiRect(0, 28, 200, 1);
  uiRect(10, 38, 180, 30);  uiText(18, 47, "1  Wi-Fi", 1);
  uiRect(10, 74, 180, 30);  uiText(18, 83, "2  Sync", 1);
  uiRect(10, 110, 180, 30); uiText(18, 119, "3  Storage", 1);
  uiRect(10, 146, 180, 30); uiText(18, 155, "4  IP", 1);
  uiFillRect(0, 184, 200, 16, 0x00);
  uiTextCentered(188, "< back", 1, 0xff);
  uiFlushFull();
}

static void drawStorage() {
  uint64_t total = SD_MMC.totalBytes();
  uint64_t used = SD_MMC.usedBytes();
  uint64_t freeB = total > used ? total - used : 0;
  epd->EPD_Clear();
  uiTextCentered(14, "STORAGE", 2);
  uiRect(0, 34, 200, 1);
  uiText(20, 60, "Total:  " + String(total / (1024ULL * 1024ULL)) + " MB", 1);
  uiText(20, 84, "Used:   " + String(used / (1024ULL * 1024ULL)) + " MB", 1);
  uiText(20, 108, "Free:   " + String(freeB / (1024ULL * 1024ULL)) + " MB", 1);
  int pct = total ? (int)(used * 100 / total) : 0;
  uiRect(20, 136, 160, 12);
  uiFillRect(22, 138, (int)(156 * pct / 100), 8, 0x00);
  uiTextCentered(170, String(pct) + "% used", 1);
  uiFillRect(0, 184, 200, 16, 0x00);
  uiTextCentered(188, "< back", 1, 0xff);
  uiFlushFull();
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
  state = ST_MAKE;
  drawMake();
  return true;
}

static void finishRecording() {
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

static void maybeAutoSync() {
  if (!netGet("ssid").length() || !netGet("api").length()) return;
  time_t now = time(nullptr);
  if (now < 1600000000) return;
  uint64_t last = netGetU64("lastSync", 0);
  if (now - (time_t)last < 86400) return;
  syncReturnTo = ST_HOME;
  state = ST_HOME;
  syncAll(true);
}

static void pollTouch(bool& tap, bool& longPress, bool& releaseEvent, int& tx, int& ty, int& swipe) {
  tap = false; longPress = false; releaseEvent = false; swipe = 0;
  uint16_t x, y;
  bool down = touch->GetTouchPoint(&x, &y);
  if (down && !pressed) {
    pressed = true;
    touchDownAt = millis();
    downX = x; downY = y; lastY = y;
    longFired = false;
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
  drawHome();
  lastActivity = millis();
  maybeAutoSync();
}

void loop() {
  bool tap, longPress, releaseEvent;
  int tx, ty, swipe;
  pollTouch(tap, longPress, releaseEvent, tx, ty, swipe);

  switch (state) {
    case ST_HOME:
      if (tap) {
        if (soundOn()) beep();
        state = ST_MENU;
        drawMenu();
      }
      if (millis() - lastActivity > 30000 && !pressed) sleepNow();
      if (millis() - lastAutoCheck > 3600000UL) { lastAutoCheck = millis(); maybeAutoSync(); }
      break;

    case ST_MENU:
      if (longPress || (tap && ty > 184)) { sleepNow(); break; }
      if (tap) {
        if (soundOn()) beep();
        if (ty >= 40 && ty < 88) {
          viewTag = "";
          state = ST_VIEW_TAGS;
          drawViewTags();
        } else if (ty >= 94 && ty < 142) {
          startRecording();
        } else if (ty >= 148 && ty < 196) {
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
      if (tap && ty >= 44 && ty <= 104) finishRecording();
      if (longPress) finishRecording();
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
      if (tap) {
        if (soundOn()) beep();
        if (ty >= 38 && ty < 68) {
          state = ST_SET_WIFI;
          drawWifiScreen(portalStart());
        } else if (ty >= 74 && ty < 104) {
          syncAll(false);
        } else if (ty >= 110 && ty < 140) {
          state = ST_STORAGE;
          drawStorage();
        } else if (ty >= 146 && ty < 176) {
          drawSyncScreen(0, 0, "connecting wifi...");
          if (staConnect(20000)) {
            String ip = serverStartSta();
            state = ST_SET_IP;
            drawIpScreen();
          } else {
            showError("wifi failed");
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
      if (tap || longPress || (ty > 184)) { state = ST_SETTINGS; drawSettings(); }
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
