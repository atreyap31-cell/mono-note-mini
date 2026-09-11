#include "pala_sync.h"
#include "pala_net.h"
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <SD_MMC.h>
#include <esp_heap_caps.h>

String syncDeviceId() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char buf[24];
  snprintf(buf, sizeof(buf), "mnm-%02x%02x%02x%02x", mac[2], mac[3], mac[4], mac[5]);
  return String(buf);
}

static String apiBase() {
  String api = netGet("api");
  api.trim();
  while (api.length() && api.endsWith("/")) api.remove(api.length() - 1);
  return api;
}

bool syncConfigured() { return apiBase().length() > 0; }

/* One multipart POST of a whole file. Built in PSRAM in one piece rather than
   streamed: a two minute note is under four megabytes, there are eight to
   spare, and a single POST has no half-sent state to reason about. */
static bool postFile(const String& path, const String& name,
                     const String& contentType, String& err) {
  String api = apiBase();
  if (!api.length())              { err = "no address set"; return false; }
  if (WiFi.status() != WL_CONNECTED) { err = "no wi-fi";    return false; }

  File f = SD_MMC.open(path, "r");
  if (!f) { err = "cannot read " + name; return false; }
  const size_t fileLen = f.size();

  const char* boundary = "----pala7d91bnd";
  String head;
  head += "--"; head += boundary;
  head += "\r\nContent-Disposition: form-data; name=\"name\"\r\n\r\n";
  head += name;
  head += "\r\n--"; head += boundary;
  head += "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"";
  head += name;
  head += "\"\r\nContent-Type: " + contentType + "\r\n\r\n";
  String tail = "\r\n--"; tail += boundary; tail += "--\r\n";

  const size_t bodyLen = head.length() + fileLen + tail.length();
  uint8_t* body = (uint8_t*)heap_caps_malloc(bodyLen, MALLOC_CAP_SPIRAM);
  if (!body) { f.close(); err = "out of memory"; return false; }
  memcpy(body, head.c_str(), head.length());
  f.read(body + head.length(), fileLen);
  f.close();
  memcpy(body + head.length() + fileLen, tail.c_str(), tail.length());

  /* A plain client for http, TLS only for https. The transcription path used a
     WiFiClientSecure for an http:// address, which is a TLS handshake nothing
     at the other end was ever going to answer. */
  const bool tls = api.startsWith("https://");
  WiFiClient plain;
  WiFiClientSecure secure;
  if (tls) { secure.setInsecure(); secure.setTimeout(30); }

  HTTPClient http;
  bool ok = false;
  const String url = api + "/notes";
  if (http.begin(tls ? (WiFiClient&)secure : plain, url)) {
    http.setTimeout(60000);
    http.addHeader("Content-Type", String("multipart/form-data; boundary=") + boundary);
    int code = http.POST(body, bodyLen);
    if (code == 200) {
      ok = true;
    } else if (code > 0) {
      err = "server said " + String(code);
    } else {
      err = "cannot reach server";
    }
    http.end();
  } else {
    err = "bad address";
  }
  heap_caps_free(body);
  return ok;
}

bool syncUploadNote(const String& base, String& err) {
  if (!postFile("/recordings/" + base + ".wav", base + ".wav", "audio/wav", err))
    return false;
  /* A transcript is optional and its failure is not the note's failure: the
     audio is already up, and marking the note unsynced over a missing text
     file would upload the audio again on every retry. */
  String ignored;
  if (SD_MMC.exists("/recordings/" + base + ".txt"))
    postFile("/recordings/" + base + ".txt", base + ".txt", "text/plain", ignored);
  return true;
}
