#include "pala_net.h"
#include "pala_sync.h"
#include "pala_rtc.h"
#include <sys/time.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <SD_MMC.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <vector>

static Preferences prefs;
static WebServer server(80);
static bool portalUp = false;

void netBegin() { prefs.begin("pala", false); if(!prefs.isKey("devpass")) prefs.putString("devpass","record123"); }
String base64Encode(const String& s){ static const char tbl[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"; String out; out.reserve(((s.length()+2)/3)*4); for(size_t i=0;i<s.length();i+=3){ uint32_t v=(uint8_t)s[i]<<16 | (i+1<s.length()?(uint8_t)s[i+1]<<8:0) | (i+2<s.length()?(uint8_t)s[i+2]:0); out+=tbl[(v>>18)&63]; out+=tbl[(v>>12)&63]; out+=(i+1<s.length()?tbl[(v>>6)&63]:'='); out+=(i+2<s.length()?tbl[v&63]:'='); } return out; }
bool isAuthenticated(){ String pass=netGet("devpass","record123"); if(pass.length()==0) return true; String h=server.header("Authorization"); if(!h.startsWith("Basic ")) return false; String b64=h.substring(6); b64.trim(); String e1=base64Encode("admin:"+pass), e2=base64Encode(":"+pass), e3=base64Encode("user:"+pass); return b64==e1||b64==e2||b64==e3; }
bool needAuth(){ if(isAuthenticated()) return false; server.sendHeader("WWW-Authenticate","Basic realm=\"Mono Note Mini\""); server.send(401,"text/plain","Auth required"); return true; }

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

static String htmlEscape(const String& s) {
  String o = s;
  o.replace("&", "&amp;"); o.replace("<", "&lt;"); o.replace(">", "&gt;");
  o.replace("'", "&#39;");
  return o;
}

static void handleRoot() {
  if(needAuth()) return;
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
  if(needAuth()) return;
  String page = "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'><title>Mono Note Mini</title><style>@import url('https://fonts.googleapis.com/css2?family=IBM+Plex+Mono:wght@600;700&display=swap');*{box-sizing:border-box}html,body{margin:0;background:#fff;color:#000;font-family:Inter,sans-serif}body{max-width:520px;margin:0 auto;padding:16px}h2,h3{font-family:'IBM Plex Mono',monospace;text-transform:uppercase;letter-spacing:-.02em}input,textarea,button,select{font:inherit;border:2px solid #000;border-radius:0;background:#fff;color:#000;padding:8px 10px}button{font-family:'IBM Plex Mono',monospace;font-weight:700;text-transform:uppercase;letter-spacing:.06em;cursor:pointer;box-shadow:3px 3px 0 #000}button:active{transform:translate(1px,1px);box-shadow:1px 1px 0 #000}a{color:#000;text-decoration:underline;text-underline-offset:2px}ul{padding-left:18px}li{margin:4px 0}code{background:#fff;border:1px solid #000;padding:1px 4px}</style>";
  page += "<body><h2>Mono Note Mini</h2><p>Drop your own site at <code>/www/index.html</code> on the SD card and it replaces this page. Everything here is black on white.</p>";
  page += "<input id=q placeholder='filter...' oninput='f()' style='width:100%'>";
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
  page += "</ul><h3>To-do</h3><p>One job per line. Use [x] for done.</p>";
  String todoText = "";
  if (SD_MMC.exists("/todo.txt")) {
    File tf = SD_MMC.open("/todo.txt", "r");
    todoText = tf.readString();
    tf.close();
  }
  page += "<textarea id=todo rows=6 style='width:100%'>" + htmlEscape(todoText) + "</textarea>";
  page += "<button onclick=\"fetch('/api/todo',{method:'POST',body:document.getElementById('todo').value}).then(()=>location.reload())\">Save to-do</button>";
  page += "<h3>Upload</h3><p>Recordings (<code>.wav</code>), transcripts (<code>.txt</code>) or tags (<code>.tag</code>).</p><form method=POST action=/up enctype=multipart/form-data><input type=file name=f accept='.wav,.txt,.tag'><button>Upload</button></form>";
  page += "<h3>Settings</h3><form method=POST action=/save>";
  page += "SSID <input name=ssid value='" + netGet("ssid") + "'><br>";
  page += "Password <input name=pass type=password value='" + netGet("pass") + "'><br>";
  page += "API base <input name=api value='" + netGet("api") + "'><br>";
  page += "Device password <input name=devpass type=password value='" + netGet("devpass","record123") + "'><br><small>Each device keeps its own — others on your Wi-Fi can't see your notes without it</small><br>";
  page += "<h3>Publish to your repo</h3>";
  page += "<p><small>Sync pushes your notes to a GitHub repo you own, so the web app can read them from anywhere. This device writes one file of its own and nothing else, so several devices can share a repo without overwriting each other.</small></p>";
  page += "<p>This device: <code>" + syncDeviceId() + "</code><br><small>Open the web app with <code>?d=" + syncDeviceId() + "</code> to see just these notes.</small></p>";
  page += "Owner <input name=ghowner value='" + netGet("ghOwner") + "'><br>";
  page += "Repo <input name=ghrepo value='" + netGet("ghRepo") + "'><br>";
  page += "Branch <input name=ghbranch value='" + netGet("ghBranch","main") + "'><br>";
  page += "Path <input name=ghpath placeholder='" + syncPath() + "' value='" + netGet("ghPath") + "'><br>";
  page += "Token <input name=ghtoken type=password value='" + String(netGet("ghToken").length() ? "________" : "") + "'><br>";
  page += "<small><b>Use a fine-grained token limited to this one repo, Contents: read and write.</b> It is stored on the device in plain text, so a lost device means a token that can write to that repo — and nothing else. Leave the field as-is to keep the current token.</small><br>";
  {
    uint32_t hrs = netGetU32("syncHrs", SYNC_HOURS_DEFAULT);
    const uint32_t opts[] = {0, 1, 2, 4, 8, 24};
    page += "Auto-sync <select name=synchrs>";
    for (int i = 0; i < 6; i++) {
      page += "<option value=" + String(opts[i]) + (opts[i] == hrs ? " selected" : "") + ">";
      page += opts[i] == 0 ? "Off" : ("Every " + String(opts[i]) + " h");
      page += "</option>";
    }
    page += "</select><br><small>Syncing more often drains the battery faster — every 4 h is the default</small><br>";
  }
  page += "Button sounds <input type=checkbox name=sound " + String(netGet("sound", "0") == "1" ? "checked" : "") + "><br><small>Clean soft tick — off by default</small><br>";
  page += "<button>Save &amp; reboot</button></form></body>";
  server.send(200, "text/html", page);
}

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

/* One note per entry, audio optional - FREE SPACE keeps the transcript after
   dropping the .wav, and the page has to keep showing those. Streamed
   chunk by chunk so a card full of transcripts never has to fit in RAM. */
static void handleApiNotes() {
  if (needAuth()) return;
  std::vector<String> bases;
  File dir = SD_MMC.open("/recordings");
  if (dir) {
    File f;
    while ((f = dir.openNextFile())) {
      String n = String(f.name());
      f.close();
      String lower = n; lower.toLowerCase();
      if (!lower.endsWith(".wav") && !lower.endsWith(".txt")) continue;
      String b = netBaseOf(n);
      bool seen = false;
      for (unsigned int i = 0; i < bases.size(); i++) if (bases[i] == b) { seen = true; break; }
      if (!seen) bases.push_back(b);
    }
    dir.close();
  }

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  server.sendContent("[");
  for (unsigned int i = 0; i < bases.size(); i++) {
    const String& b = bases[i];
    uint32_t wavBytes = 0;
    File w = SD_MMC.open("/recordings/" + b + ".wav", "r");
    if (w) { wavBytes = w.size(); w.close(); }
    String chunk = (i ? "," : "");
    chunk += "{\"base\":\"" + jsonEscape(b) + "\"";
    chunk += ",\"bytes\":" + String(wavBytes);
    chunk += ",\"secs\":" + String(wavBytes > 44 ? (wavBytes - 44) / 32000 : 0);
    chunk += ",\"audio\":" + String(wavBytes ? "true" : "false");
    chunk += ",\"tag\":\"" + jsonEscape(readSmall("/recordings/" + b + ".tag")) + "\"";
    chunk += ",\"txt\":\"" + jsonEscape(readSmall("/recordings/" + b + ".txt")) + "\"}";
    server.sendContent(chunk);
  }
  server.sendContent("]");
  server.sendContent("");
}

/* Removes the whole note - audio, transcript and tag together. */
static void handleApiDelete() {
  if (needAuth()) return;
  String b = server.arg("n");
  b = netBaseOf(b);
  if (!b.length() || b.indexOf("..") >= 0) { server.send(400, "text/plain", "bad name"); return; }
  SD_MMC.remove("/recordings/" + b + ".wav");
  SD_MMC.remove("/recordings/" + b + ".txt");
  SD_MMC.remove("/recordings/" + b + ".tag");
  server.send(200, "text/plain", "ok");
}

/* Re-file a note under a different tag, or clear it with an empty value. */
static void handleApiTag() {
  if (needAuth()) return;
  String b = netBaseOf(server.arg("n"));
  String tag = server.arg("tag");
  if (!b.length() || b.indexOf("..") >= 0) { server.send(400, "text/plain", "bad name"); return; }
  if (!tag.length()) SD_MMC.remove("/recordings/" + b + ".tag");
  else {
    File f = SD_MMC.open("/recordings/" + b + ".tag", "w");
    if (f) { f.println(tag); f.close(); }
  }
  server.send(200, "text/plain", "ok");
}

static void handleApiInfo() {
  if (needAuth()) return;
  uint64_t total = SD_MMC.totalBytes(), used = SD_MMC.usedBytes();
  String j = "{\"totalMB\":" + String((uint32_t)(total / 1048576ULL));
  j += ",\"usedMB\":" + String((uint32_t)(used / 1048576ULL));
  j += ",\"syncHrs\":" + String(netGetU32("syncHrs", SYNC_HOURS_DEFAULT));
  j += ",\"api\":\"" + jsonEscape(netGet("api")) + "\"";
  j += ",\"ssid\":\"" + jsonEscape(netGet("ssid")) + "\"";
  j += ",\"device\":\"" + syncDeviceId() + "\"";
  j += ",\"ghOwner\":\"" + jsonEscape(netGet("ghOwner")) + "\"";
  j += ",\"ghRepo\":\"" + jsonEscape(netGet("ghRepo")) + "\"";
  j += ",\"clock\":" + String((uint32_t)time(nullptr)) + "}";
  server.send(200, "application/json", j);
}

static void handleApiList() {
  if(needAuth()) return;
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
  if(needAuth()) return;
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
  if(needAuth()) return;
  netSet("ssid", server.arg("ssid"));
  netSet("pass", server.arg("pass"));
  netSet("api", server.arg("api"));
  String dp=server.arg("devpass"); if(dp.length()) netSet("devpass", dp);
  netSet("ghOwner",  server.arg("ghowner"));
  netSet("ghRepo",   server.arg("ghrepo"));
  netSet("ghBranch", server.arg("ghbranch").length() ? server.arg("ghbranch") : String("main"));
  netSet("ghPath",   server.arg("ghpath"));
  /* The form shows a placeholder rather than the real token, so writing that
     placeholder back would destroy it. */
  String gt = server.arg("ghtoken");
  if (gt.length() && gt != "________") netSet("ghToken", gt);
  netSet("sound", server.hasArg("sound") ? "1" : "0");
  if (server.hasArg("synchrs")) {
    uint32_t h = (uint32_t)server.arg("synchrs").toInt();
    if (h == 0 || h == 1 || h == 2 || h == 4 || h == 8 || h == 24) netSetU32("syncHrs", h);
  }
  server.send(200, "text/html", "<body>Saved. Rebooting...</body><script>setTimeout(()=>location='/',1500)</script>");
  delay(800);
  ESP.restart();
}

static void handleUpload() { if(needAuth()) return; server.send(200, "text/plain", "ok"); }

/* Set the clock from whatever is talking to the device.

   NTP only arrives once Wi-Fi is configured and reachable, so a device that
   has never been on a network has no idea what year it is, and its notes are
   named accordingly. A browser opening this page knows the time perfectly
   well; letting it say so means the clock solves itself the first time anyone
   looks at the device, with no setup at all.

   The value is written straight through to the RTC, which keeps it across
   power loss. */
static void handleApiTime() {
  if (needAuth()) return;
  String v = server.arg("t");
  long long epoch = atoll(v.c_str());
  if (epoch < 1700000000LL) {           /* sometime after this was written */
    server.send(400, "text/plain", "implausible time");
    return;
  }
  struct timeval tv = { .tv_sec = (time_t)epoch, .tv_usec = 0 };
  settimeofday(&tv, nullptr);
  bool saved = rtcSaveSystemTime();
  server.send(200, "text/plain", saved ? "ok, clock kept" : "ok, but the RTC did not take it");
}

static void handleTodoGet() {
  if(needAuth()) return;
  String out = "";
  if (SD_MMC.exists("/todo.txt")) {
    File f = SD_MMC.open("/todo.txt", "r");
    out = f.readString();
    f.close();
  }
  server.send(200, "text/plain", out);
}

static void handleTodoPost() {
  if(needAuth()) return;
  File f = SD_MMC.open("/todo.txt", "w");
  if (f) { f.print(server.arg("plain")); f.close(); }
  server.send(200, "text/plain", "ok");
}

static void onUploadFile() {
  if(!isAuthenticated()) return;
  HTTPUpload& up = server.upload();
  static File fh;
  if (up.status == UPLOAD_FILE_START) {
    String name = up.filename;
    /* keep only the basename so a crafted path can't escape /recordings */
    int slash = name.lastIndexOf('/');
    if (slash >= 0) name = name.substring(slash + 1);
    slash = name.lastIndexOf('\\');
    if (slash >= 0) name = name.substring(slash + 1);
    if (name.indexOf("..") >= 0) name.replace("..", "_");
    String lower = name; lower.toLowerCase();
    if (!lower.endsWith(".wav") && !lower.endsWith(".txt") && !lower.endsWith(".tag")) {
      fh = File();
      return;
    }
    fh = SD_MMC.open("/recordings/" + name, "w");
  } else if (up.status == UPLOAD_FILE_WRITE && fh) {
    fh.write(up.buf, up.currentSize);
  } else if (up.status == UPLOAD_FILE_END && fh) {
    fh.close();
  }
}

static void beginServerRoutes() {
  const char* hdr[]={"Authorization"};
  server.collectHeaders(hdr,1);
  server.on("/", handleRoot);
  server.on("/app", handleApp);
  server.on("/api/list", handleApiList);
  server.on("/api/notes", handleApiNotes);
  server.on("/api/info", handleApiInfo);
  server.on("/api/delete", HTTP_POST, handleApiDelete);
  server.on("/api/tag", HTTP_POST, handleApiTag);
  server.on("/file", handleFile);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/up", HTTP_POST, handleUpload, onUploadFile);
  server.on("/api/time", HTTP_POST, handleApiTime);
  server.on("/api/todo", HTTP_GET, handleTodoGet);
  server.on("/api/todo", HTTP_POST, handleTodoPost);
  server.serveStatic("/www/", SD_MMC, "/www/");
  server.begin();
  portalUp = true;
}

String portalStart() {
  WiFi.mode(WIFI_AP);
  String mac = WiFi.macAddress();
  mac.replace(":", "");
  String ssid = "MonoNote-" + mac.substring(mac.length() - 4);
  WiFi.softAP(ssid.c_str(), "record123");
  beginServerRoutes();
  return ssid;
}

String serverStartSta() {
  beginServerRoutes();
  return WiFi.localIP().toString();
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
