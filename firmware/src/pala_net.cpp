#include "pala_net.h"
#include "pala_sync.h"
#include "pala_rtc.h"
#include <sys/time.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <SD_MMC.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <vector>

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

void netBegin() { prefs.begin("pala", false); applyTimezone(); if(!prefs.isKey("devpass")) prefs.putString("devpass","record123"); }
String base64Encode(const String& s){ static const char tbl[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"; String out; out.reserve(((s.length()+2)/3)*4); for(size_t i=0;i<s.length();i+=3){ uint32_t v=(uint8_t)s[i]<<16 | (i+1<s.length()?(uint8_t)s[i+1]<<8:0) | (i+2<s.length()?(uint8_t)s[i+2]:0); out+=tbl[(v>>18)&63]; out+=tbl[(v>>12)&63]; out+=(i+1<s.length()?tbl[(v>>6)&63]:'='); out+=(i+2<s.length()?tbl[v&63]:'='); } return out; }

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

bool transcribeFile(const String& wavPath, String& outText) {
  String api = netGet("api");
  api.trim();
  while (api.length() && api.endsWith("/")) api.remove(api.length() - 1);
  if (!api.length() || WiFi.status() != WL_CONNECTED) return false;
  File f = SD_MMC.open(wavPath, "r");
  if (!f) return false;
  size_t fileLen = f.size();
  const char* boundary = "----pala7d91bnd";
  String head = "--"; head += boundary;
  head += "\r\nContent-Disposition: form-data; name=\"audio\"; filename=\"clip.wav\"\r\nContent-Type: audio/wav\r\n\r\n";
  String tail = "\r\n--"; tail += boundary; tail += "--\r\n";
  size_t bodyLen = head.length() + fileLen + tail.length();
  uint8_t* body = (uint8_t*)heap_caps_malloc(bodyLen, MALLOC_CAP_SPIRAM);
  if (!body) { f.close(); return false; }
  memcpy(body, head.c_str(), head.length());
  f.read(body + head.length(), fileLen);
  f.close();
  memcpy(body + head.length() + fileLen, tail.c_str(), tail.length());

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(30);
  HTTPClient http;
  bool ok = false;
  if (http.begin(client, api + "/transcribe")) {
    http.setTimeout(60000);
    http.addHeader("Content-Type", String("multipart/form-data; boundary=") + boundary);
    int code = http.POST(body, bodyLen);
    if (code == 200) {
      String resp = http.getString();
      JsonDocument doc;
      if (!deserializeJson(doc, resp)) {
        String t = doc["text"] | doc["content"] | doc["transcript"] | doc["result"] | "";
        outText = t;
        ok = outText.length() > 0;
      }
    }
    http.end();
  }
  heap_caps_free(body);
  return ok;
}
