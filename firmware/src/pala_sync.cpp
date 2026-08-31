#include "pala_sync.h"
#include "pala_net.h"
#include "pala_gh_ca.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>

static const char* GH_HOST = "api.github.com";

String syncDeviceId() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char buf[20];
  snprintf(buf, sizeof(buf), "mnm-%02x%02x%02x%02x", mac[2], mac[3], mac[4], mac[5]);
  return String(buf);
}

bool syncConfigured() {
  return netGet("ghOwner").length() && netGet("ghRepo").length() && netGet("ghToken").length();
}

String syncPath() {
  String p = netGet("ghPath");
  p.trim();
  if (!p.length()) p = "data/" + syncDeviceId() + ".json";
  while (p.startsWith("/")) p.remove(0, 1);
  return p;
}

/* rec_20260812_101200 -> epoch milliseconds, so the web app can sort and show
   a real date. A name that does not parse falls back to "now" rather than to
   1970, which would sort every unparsed note to the top of the list forever. */
static uint64_t createdFromBase(const String& base) {
  int u = base.indexOf('_');
  int u2 = u >= 0 ? base.indexOf('_', u + 1) : -1;
  if (u >= 0 && u2 > u && base.length() >= (unsigned)(u2 + 7)) {
    struct tm t = {};
    t.tm_year = base.substring(u + 1, u + 5).toInt() - 1900;
    t.tm_mon  = base.substring(u + 5, u + 7).toInt() - 1;
    t.tm_mday = base.substring(u + 7, u + 9).toInt();
    t.tm_hour = base.substring(u2 + 1, u2 + 3).toInt();
    t.tm_min  = base.substring(u2 + 3, u2 + 5).toInt();
    t.tm_sec  = base.substring(u2 + 5, u2 + 7).toInt();
    if (t.tm_year > 100) {
      time_t e = mktime(&t);
      if (e > 0) return (uint64_t)e * 1000ULL;
    }
  }
  time_t now = time(nullptr);
  return (now > 1600000000 ? (uint64_t)now : 0ULL) * 1000ULL;
}

static String jsonEscape(const String& in) {
  String out;
  out.reserve(in.length() + 16);
  for (unsigned int i = 0; i < in.length(); i++) {
    char c = in[i];
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:
        if ((unsigned char)c < 0x20) {          /* other controls are illegal raw */
          char u[8];
          snprintf(u, sizeof(u), "\\u%04x", c);
          out += u;
        } else out += c;
    }
  }
  return out;
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static String base64Encode(const String& in) {
  String out;
  out.reserve(((in.length() + 2) / 3) * 4 + 4);
  unsigned int i = 0;
  while (i + 2 < in.length()) {
    uint32_t n = ((uint8_t)in[i] << 16) | ((uint8_t)in[i + 1] << 8) | (uint8_t)in[i + 2];
    out += B64[(n >> 18) & 63]; out += B64[(n >> 12) & 63];
    out += B64[(n >> 6) & 63];  out += B64[n & 63];
    i += 3;
  }
  int rem = in.length() - i;
  if (rem == 1) {
    uint32_t n = (uint8_t)in[i] << 16;
    out += B64[(n >> 18) & 63]; out += B64[(n >> 12) & 63]; out += "==";
  } else if (rem == 2) {
    uint32_t n = ((uint8_t)in[i] << 16) | ((uint8_t)in[i + 1] << 8);
    out += B64[(n >> 18) & 63]; out += B64[(n >> 12) & 63];
    out += B64[(n >> 6) & 63];  out += '=';
  }
  return out;
}

static String isoNow() {
  time_t now = time(nullptr);
  if (now < 1600000000) return "";
  struct tm t;
  gmtime_r(&now, &t);
  char buf[32];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
           t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
  return String(buf);
}

static String contentsUrl() {
  return "https://" + String(GH_HOST) + "/repos/" + netGet("ghOwner") + "/" +
         netGet("ghRepo") + "/contents/" + syncPath();
}

static void applyHeaders(HTTPClient& http) {
  http.addHeader("Authorization", "Bearer " + netGet("ghToken"));
  http.addHeader("Accept", "application/vnd.github+json");
  http.addHeader("X-GitHub-Api-Version", "2022-11-28");
  http.addHeader("User-Agent", "mono-note-mini");
}

/* Only the sha is wanted: this device is the sole writer of its own file, so
   the existing contents are about to be replaced wholesale and there is
   nothing in them worth reading back. Pulling just the sha also keeps a large
   file from ever having to fit in RAM. */
static bool fetchSha(WiFiClientSecure& tls, String& sha, String& err) {
  HTTPClient http;
  String url = contentsUrl() + "?ref=" + netGet("ghBranch", "main");
  if (!http.begin(tls, url)) { err = "begin failed"; return false; }
  applyHeaders(http);
  int code = http.GET();
  if (code == 404) { sha = ""; http.end(); return true; }   /* first publish */
  if (code != 200) {
    err = "GET " + String(code);
    if (code > 0) {
      String b = http.getString();
      int m = b.indexOf("\"message\"");
      if (m >= 0) err += " " + b.substring(m + 11, min((int)b.length(), m + 90));
    }
    http.end();
    return false;
  }
  String body = http.getString();
  http.end();
  int s = body.indexOf("\"sha\"");
  if (s < 0) { err = "no sha in reply"; return false; }
  int q1 = body.indexOf('"', s + 6);
  int q2 = body.indexOf('"', q1 + 1);
  if (q1 < 0 || q2 < 0) { err = "bad sha"; return false; }
  sha = body.substring(q1 + 1, q2);
  return true;
}

bool syncPublish(const std::vector<SyncNote>& notes,
                 const std::vector<SyncTodo>& todos,
                 String& err) {
  err = "";
  if (!syncConfigured()) { err = "not configured"; return false; }
  if (WiFi.status() != WL_CONNECTED) { err = "no wifi"; return false; }

  WiFiClientSecure tls;
  tls.setCACert(GITHUB_ROOT_CA);      /* never setInsecure - a token rides on this */
  tls.setTimeout(20000);

  String sha;
  if (!fetchSha(tls, sha, err)) return false;

  /* Text only. The contents API refuses to read back anything over 1 MB, and
     base64 audio passes that within a handful of clips - at which point the
     file becomes unreadable to the very app meant to display it. Audio stays
     on the card, reachable over the LAN. */
  String doc = "{\n  \"version\": 2,\n  \"device\": \"" + syncDeviceId() + "\",\n  \"notes\": [";
  for (size_t i = 0; i < notes.size(); i++) {
    const SyncNote& n = notes[i];
    if (i) doc += ",";
    doc += "\n    {";
    doc += "\"id\":\"dev_" + jsonEscape(n.base) + "\",";
    doc += "\"name\":\"" + jsonEscape(n.base) + "\",";
    doc += "\"created\":" + String((unsigned long long)createdFromBase(n.base)) + ",";
    doc += "\"edited\":" + String((unsigned long long)createdFromBase(n.base)) + ",";
    doc += "\"tags\":[";
    if (n.tag.length()) doc += "\"" + jsonEscape(n.tag) + "\"";
    doc += "],";
    doc += "\"transcript\":\"" + jsonEscape(n.transcript) + "\",";
    doc += "\"duration\":" + String(n.secs) + ",";
    doc += "\"kind\":\"recording\"}";
  }
  doc += "\n  ],\n  \"todos\": [";
  for (size_t i = 0; i < todos.size(); i++) {
    if (i) doc += ",";
    doc += "\n    {\"id\":\"dev_todo_" + String((unsigned)i) + "\",";
    doc += "\"text\":\"" + jsonEscape(todos[i].text) + "\",";
    doc += String("\"done\":") + (todos[i].done ? "true" : "false") + "}";
  }
  doc += "\n  ],\n  \"exportedAt\": \"" + isoNow() + "\"\n}\n";

  String body = "{\"message\":\"Mono Note Mini sync from " + syncDeviceId() +
                "\",\"branch\":\"" + netGet("ghBranch", "main") +
                "\",\"content\":\"" + base64Encode(doc) + "\"";
  if (sha.length()) body += ",\"sha\":\"" + sha + "\"";
  body += "}";

  HTTPClient http;
  if (!http.begin(tls, contentsUrl())) { err = "begin failed"; return false; }
  applyHeaders(http);
  http.addHeader("Content-Type", "application/json");
  int code = http.PUT((uint8_t*)body.c_str(), body.length());
  if (code != 200 && code != 201) {
    err = "PUT " + String(code);
    if (code > 0) {
      String b = http.getString();
      int m = b.indexOf("\"message\"");
      if (m >= 0) err += " " + b.substring(m + 11, min((int)b.length(), m + 90));
    } else {
      err += " (tls/network)";
    }
    http.end();
    return false;
  }
  http.end();
  netSetU64("lastPublish", (uint64_t)time(nullptr));
  return true;
}
