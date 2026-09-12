#include "pala_net.h"
#include "pala_sync.h"
#include "pala_rtc.h"
#include <sys/time.h>
#include <WiFi.h>
#include <Preferences.h>
#include <SD_MMC.h>
#include <vector>
#include "esp_wps.h"
#include "esp_wifi.h"

static Preferences prefs;

/* POSIX TZ strings run the opposite way round to everyone else: a zone two
   hours ahead of UTC is written "UTC-2". JS getTimezoneOffset already returns
   minutes behind UTC, so its sign is the one POSIX wants. */
void applyTimezone() {
  uint32_t stored = netGetU32("tzmin", 0);
  if (!stored) return;                       /* never set - stay on UTC */
  int mins = (int)stored - 1000;
  char tz[24];
  snprintf(tz, sizeof(tz), "UTC%+d:%02d", mins / 60, abs(mins % 60));
  setenv("TZ", tz, 1);
  tzset();
}

/* devpass used to be the password on the device's own web server. That server
   is gone, and nothing has read the key since - it was only still being
   written. */
void netBegin() { prefs.begin("pala", false); applyTimezone(); }

String netGet(const char* key, const String& def) { return prefs.getString(key, def); }
void netSet(const char* key, const String& value) { prefs.putString(key, value); }
uint64_t netGetU64(const char* key, uint64_t def) { return prefs.getULong64(key, def); }
void netSetU64(const char* key, uint64_t value) { prefs.putULong64(key, value); }
uint32_t netGetU32(const char* key, uint32_t def) { return prefs.getUInt(key, def); }
void netSetU32(const char* key, uint32_t value) { prefs.putUInt(key, value); }
bool netHasKey(const char* key){ return prefs.isKey(key); }
void netSetBool(const char* key, bool v){ prefs.putBool(key, v); }
bool netGetBool(const char* key, bool def){ return prefs.getBool(key, def); }

/* Factory reset: drop every stored preference. Caller reboots afterwards, and
   the missing first_boot_done key makes the next boot run the tour again. */
void netClearAll() { prefs.clear(); }

bool staConnect(uint32_t timeoutMs) {
  String ssid = netGet("ssid");
  String pass = netGet("pass");
  if (!ssid.length()) return false;
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) delay(100);
  if (WiFi.status() == WL_CONNECTED) {
    configTime(0, 0, "pool.ntp.org");
    return true;
  }
  WiFi.disconnect();
  return false;
}

void staDisconnect() { WiFi.mode(WIFI_OFF); }

static String jsonEscape(const String& s) {
  String o; o.reserve(s.length() + 8);
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"') o += "\\\"";
    else if (c == '\\') o += "\\\\";
    else if (c == '\n') o += "\\n";
    else if (c == '\r') continue;
    else if (c == '\t') o += "\\t";
    else if ((unsigned char)c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
    else o += c;
  }
  return o;
}

static String readSmall(const String& path) {
  if (!SD_MMC.exists(path)) return "";
  File f = SD_MMC.open(path, "r");
  if (!f) return "";
  String s = f.readString();
  f.close();
  if (s.length() > 6000) s = s.substring(0, 6000);
  s.trim();
  return s;
}

static String netBaseOf(const String& fileName) {
  String n = fileName;
  int slash = n.lastIndexOf('/');
  if (slash >= 0) n = n.substring(slash + 1);
  int dot = n.lastIndexOf('.');
  if (dot > 0) n = n.substring(0, dot);
  return n;
}



/* ---- finding a network -------------------------------------------------
   Typing a network name on a device with one button is impossible, and typing
   it on the page means getting the spelling and the capitals exactly right for
   something most people have never actually read. Scanning and offering the
   list removes the question. */
String netScanJson() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, false);
  delay(100);
  int n = WiFi.scanNetworks(false, false);
  String out = "[";
  /* Strongest first, and only the first of each name: mesh networks and
     repeaters put the same SSID on screen three times otherwise. */
  std::vector<String> seen;
  bool first = true;
  for (int i = 0; i < n && (int)seen.size() < 20; i++) {
    String ssid = WiFi.SSID(i);
    if (!ssid.length()) continue;
    bool dup = false;
    for (size_t k = 0; k < seen.size(); k++) if (seen[k] == ssid) { dup = true; break; }
    if (dup) continue;
    seen.push_back(ssid);

    String esc = ssid;
    esc.replace("\\", "\\\\");
    esc.replace("\"", "\\\"");
    if (!first) out += ",";
    first = false;
    out += "{\"ssid\":\"" + esc + "\",";
    out += "\"rssi\":" + String(WiFi.RSSI(i)) + ",";
    out += "\"lock\":" + String(WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "false" : "true") + "}";
  }
  out += "]";
  WiFi.scanDelete();
  return out;
}

/* ---- WPS ---------------------------------------------------------------
   Push the button on the router and the router hands over the credentials.
   The button is the proof you are standing next to it, so nothing has to be
   typed and no password passes through the page at all.

   The credentials land in the Wi-Fi driver's own store, which a factory reset
   would clear without warning, so they are copied into preferences where the
   rest of the settings live. */
static volatile int wpsState = 0;      /* 0 idle, 1 running, 2 ok, 3 failed */

static void wpsEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
    case ARDUINO_EVENT_WPS_ER_SUCCESS: {
      esp_wifi_wps_disable();
      wifi_config_t cfg;
      if (esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK) {
        String ssid = String((const char*)cfg.sta.ssid);
        String pass = String((const char*)cfg.sta.password);
        if (ssid.length()) {
          netSet("ssid", ssid);
          netSet("pass", pass);
        }
      }
      wpsState = 2;
      break;
    }
    case ARDUINO_EVENT_WPS_ER_FAILED:
    case ARDUINO_EVENT_WPS_ER_TIMEOUT:
      esp_wifi_wps_disable();
      wpsState = 3;
      break;
    default:
      break;
  }
}

bool netWpsStart() {
  if (wpsState == 1) return true;            /* already waiting for the button */
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, false);
  delay(100);
  WiFi.onEvent(wpsEvent);

  esp_wps_config_t cfg = WPS_CONFIG_INIT_DEFAULT(WPS_TYPE_PBC);
  strlcpy(cfg.factory_info.manufacturer, "Waveshare", sizeof(cfg.factory_info.manufacturer));
  strlcpy(cfg.factory_info.model_name,   "Mono Note", sizeof(cfg.factory_info.model_name));
  strlcpy(cfg.factory_info.model_number, "Mini",      sizeof(cfg.factory_info.model_number));
  strlcpy(cfg.factory_info.device_name,  "Mono Note Mini", sizeof(cfg.factory_info.device_name));

  if (esp_wifi_wps_enable(&cfg) != ESP_OK) { wpsState = 3; return false; }
  if (esp_wifi_wps_start(0) != ESP_OK)     { esp_wifi_wps_disable(); wpsState = 3; return false; }
  wpsState = 1;
  return true;
}

int netWpsState() { return wpsState; }
