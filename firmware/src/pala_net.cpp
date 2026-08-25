#include "pala_net.h"
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <SD_MMC.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>

static Preferences prefs;
static WebServer server(80);
static bool portalUp = false;

void netBegin() { prefs.begin("pala", false); }

String netGet(const char* key, const String& def) { return prefs.getString(key, def); }
void netSet(const char* key, const String& value) { prefs.putString(key, value); }
uint64_t netGetU64(const char* key, uint64_t def) { return prefs.getULong64(key, def); }
void netSetU64(const char* key, uint64_t value) { prefs.putULong64(key, value); }

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

static String htmlEscape(const String& s) {
  String o = s;
  o.replace("&", "&amp;"); o.replace("<", "&lt;"); o.replace(">", "&gt;");
  o.replace("'", "&#39;");
  return o;
}

static void handleRoot() {
  if (SD_MMC.exists("/www/index.html")) {
    File f = SD_MMC.open("/www/index.html", "r");
    server.streamFile(f, "text/html");
    f.close();
    return;
  }
  server.sendHeader("Location", "/app");
  server.send(302, "text/plain", "");
}

static void handleApp() {
  String page = "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'><title>PALA</title>";
  page += "<body style='font-family:sans-serif;max-width:480px;margin:auto'>";
  page += "<h2>PALA Note</h2><p>Drop your own site at <code>/www/index.html</code> on the SD card and it replaces this page.</p>";
  page += "<input id=q placeholder='filter...' oninput='f()' style='width:100%;padding:8px'>";
  page += "<ul id=list>";
  File dir = SD_MMC.open("/recordings");
  File f;
  while ((f = dir.openNextFile())) {
    String name = String(f.name());
    if (!name.startsWith("/")) name = "/" + name;
    if (name.endsWith(".wav") || name.endsWith(".txt"))
      page += "<li data-n='" + htmlEscape(name) + "'><a href='/file?n=" + name + "'>" + htmlEscape(name) + "</a> (" + String(f.size()) + " B)</li>";
    f.close();
  }
  page += "</ul><script>function f(){var q=document.getElementById('q').value.toLowerCase();"
          "document.querySelectorAll('#list li').forEach(function(li){li.style.display=li.dataset.n.toLowerCase().includes(q)?'':'none'})}</script>";
  page += "<h3>Upload transcript (.txt)</h3><form method=POST action=/up enctype=multipart/form-data><input type=file name=f accept='.txt'><button>Upload</button></form>";
  page += "<h3>Settings</h3><form method=POST action=/save>";
  page += "SSID <input name=ssid value='" + netGet("ssid") + "'><br>";
  page += "Password <input name=pass type=password value='" + netGet("pass") + "'><br>";
  page += "API base <input name=api value='" + netGet("api") + "'><br>";
  page += "Button sounds <input type=checkbox name=sound " + String(netGet("sound", "1") == "1" ? "checked" : "") + "><br>";
  page += "<button>Save &amp; reboot</button></form></body>";
  server.send(200, "text/html", page);
}

static void handleApiList() {
  String json = "[";
  File dir = SD_MMC.open("/recordings");
  File f;
  bool first = true;
  while ((f = dir.openNextFile())) {
    String name = String(f.name());
    if (!name.startsWith("/")) name = "/" + name;
    if (name.endsWith(".wav") || name.endsWith(".txt") || name.endsWith(".tag")) {
      if (!first) json += ",";
      first = false;
      json += "{\"name\":\"" + htmlEscape(name) + "\",\"size\":" + String(f.size()) + "}";
    }
    f.close();
  }
  json += "]";
  server.send(200, "application/json", json);
}

static void handleFile() {
  String name = server.arg("n");
  if (name.length() && !name.startsWith("/")) name = "/" + name;
  if (!name.length() || name.indexOf("..") >= 0 || !SD_MMC.exists("/recordings" + name)) {
    server.send(404, "text/plain", "not found");
    return;
  }
  File f = SD_MMC.open("/recordings" + name, "r");
  String type = name.endsWith(".txt") ? "text/plain" : name.endsWith(".tag") ? "text/plain" : "audio/wav";
  server.streamFile(f, type);
  f.close();
}

static void handleSave() {
  netSet("ssid", server.arg("ssid"));
  netSet("pass", server.arg("pass"));
  netSet("api", server.arg("api"));
  netSet("sound", server.hasArg("sound") ? "1" : "0");
  server.send(200, "text/html", "<body>Saved. Rebooting...</body><script>setTimeout(()=>location='/',1500)</script>");
  delay(800);
  ESP.restart();
}

static void handleUpload() { server.send(200, "text/plain", "ok"); }

static void onUploadFile() {
  HTTPUpload& up = server.upload();
  static File fh;
  if (up.status == UPLOAD_FILE_START) {
    String name = up.filename;
    if (!name.endsWith(".txt")) name += ".txt";
    if (name.startsWith("/")) name = name.substring(1);
    fh = SD_MMC.open("/recordings/" + name, "w");
  } else if (up.status == UPLOAD_FILE_WRITE && fh) {
    fh.write(up.buf, up.currentSize);
  } else if (up.status == UPLOAD_FILE_END && fh) {
    fh.close();
  }
}

String portalStart() {
  WiFi.mode(WIFI_AP);
  String mac = WiFi.macAddress();
  mac.replace(":", "");
  String ssid = "PALA-" + mac.substring(mac.length() - 4);
  WiFi.softAP(ssid.c_str(), "record123");
  server.on("/", handleRoot);
  server.on("/app", handleApp);
  server.on("/api/list", handleApiList);
  server.on("/file", handleFile);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/up", HTTP_POST, handleUpload, onUploadFile);
  server.serveStatic("/www/", SD_MMC, "/www/");
  server.begin();
  portalUp = true;
  return ssid;
}

void portalPoll() { if (portalUp) server.handleClient(); }
void portalStop() {
  if (portalUp) { server.stop(); portalUp = false; }
  WiFi.mode(WIFI_OFF);
}
bool portalActive() { return portalUp; }

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
