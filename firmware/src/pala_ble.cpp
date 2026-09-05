#include "pala_ble.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <SD_MMC.h>
#include "pala_net.h"
#include "pala_sync.h"
#include <time.h>

/* Custom UUIDs - nothing standard describes "a notebook you talk to". The web
   page filters on the service UUID, so the browser only ever offers this
   device rather than every Bluetooth thing in the room. */
/* Proper 8-4-4-4-12. The first attempt had eight digits in the fourth
   group, which is not a UUID at all - the browser refuses the filter and
   never shows a device picker, so pressing Bluetooth did nothing at all. */
#define SVC_UUID  "6d6f6e6f-0001-4e6f-7465-6d696e690001"
#define CMD_UUID  "6d6f6e6f-0002-4e6f-7465-6d696e690002"
#define DATA_UUID "6d6f6e6f-0003-4e6f-7465-6d696e690003"

static BLEServer*         server   = nullptr;
static BLECharacteristic* cmdChar  = nullptr;
static BLECharacteristic* dataChar = nullptr;
static volatile bool connected  = false;
static volatile bool advertising = false;

/* A request is parked here by the write callback and carried out by a task.
   Doing the work inside the callback would block the Bluetooth stack for the
   length of an SD read, which stalls the connection and can drop it. */
static volatile bool  pending = false;
static String         pendingCmd;
static volatile int   progress = -1;
static char           statusMsg[24] = "off";

static void setStatus(const char* s) { strncpy(statusMsg, s, sizeof(statusMsg) - 1); }

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer*) override    { connected = true;  setStatus("connected"); }
  void onDisconnect(BLEServer*) override {
    connected = false;
    setStatus("waiting");
    /* Advertising stops on connect and does not resume on its own, so a device
       that has been connected to once would otherwise be invisible afterwards. */
    if (advertising) BLEDevice::startAdvertising();
  }
};

class CmdCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    if (pending) return;                    /* one at a time */
    String v = String(c->getValue().c_str());
    if (!v.length()) return;
    pendingCmd = v;
    pending = true;
  }
};

/* Every reply is a 4-byte little-endian length followed by that many bytes.
   The page knows when it has everything without needing an end marker that
   could also appear inside the data. */
static void sendHeader(uint32_t total) {
  uint8_t h[4] = { (uint8_t)(total), (uint8_t)(total >> 8),
                   (uint8_t)(total >> 16), (uint8_t)(total >> 24) };
  dataChar->setValue(h, 4);
  dataChar->notify();
  delay(20);
}

static bool sendChunks(const uint8_t* data, size_t len) {
  const size_t CHUNK = 180;               /* comfortably inside a negotiated MTU */
  size_t sent = 0;
  while (sent < len && connected) {
    size_t n = len - sent < CHUNK ? len - sent : CHUNK;
    dataChar->setValue((uint8_t*)(data + sent), n);
    dataChar->notify();
    sent += n;
    progress = (int)(100ULL * sent / (len ? len : 1));
    delay(8);                             /* let the stack drain its queue */
  }
  return sent == len;
}

/* Notes as JSON: the same shape the device's own web page uses, so the two
   pages can share their rendering. */
static void serveList() {
  setStatus("sending list");
  String out = "[";
  File dir = SD_MMC.open("/recordings");
  bool first = true;
  if (dir) {
    File f;
    while ((f = dir.openNextFile())) {
      String n = f.name();
      int slash = n.lastIndexOf('/');
      if (slash >= 0) n = n.substring(slash + 1);
      if (!n.endsWith(".wav")) { f.close(); continue; }
      String base = n.substring(0, n.length() - 4);
      size_t bytes = f.size();
      f.close();

      String tag, txt;
      File tf = SD_MMC.open("/recordings/" + base + ".tag", "r");
      if (tf) { tag = tf.readStringUntil('\n'); tag.trim(); tf.close(); }
      File xf = SD_MMC.open("/recordings/" + base + ".txt", "r");
      if (xf) { txt = xf.readString(); xf.close(); }
      txt.replace("\\", "\\\\");
      txt.replace("\"", "\\\"");
      txt.replace("\n", "\\n");
      txt.replace("\r", "");

      if (!first) out += ",";
      first = false;
      out += "{\"base\":\"" + base + "\",\"tag\":\"" + tag + "\",";
      out += "\"bytes\":" + String((uint32_t)bytes) + ",";
      out += "\"secs\":" + String((uint32_t)(bytes > 44 ? (bytes - 44) / 32000 : 0)) + ",";
      out += "\"txt\":\"" + txt + "\"}";
    }
    dir.close();
  }
  out += "]";
  sendHeader(out.length());
  sendChunks((const uint8_t*)out.c_str(), out.length());
  progress = -1;
  setStatus(connected ? "connected" : "waiting");
}

/* Audio, streamed straight off the card rather than read into memory - a two
   minute note is nearly four megabytes and there is nowhere to put it. */
static void serveAudio(const String& base) {
  setStatus("sending audio");
  File f = SD_MMC.open("/recordings/" + base + ".wav", "r");
  if (!f) { sendHeader(0); progress = -1; setStatus("no such note"); return; }
  const size_t total = f.size();
  sendHeader(total);

  const size_t CHUNK = 180;
  uint8_t buf[CHUNK];
  size_t sent = 0;
  while (sent < total && connected) {
    int n = f.read(buf, CHUNK);
    if (n <= 0) break;
    dataChar->setValue(buf, n);
    dataChar->notify();
    sent += n;
    progress = (int)(100ULL * sent / total);
    delay(8);
  }
  f.close();
  progress = -1;
  setStatus(connected ? "connected" : "waiting");
}

/* Settings, so the browser can configure the device without anyone typing a
   Wi-Fi password on a two-button e-paper screen. Secrets are reported as
   present or absent and never sent back out: the page has no reason to read a
   password it already knows, and every reason not to be able to. */
static void serveSettings() {
  setStatus("sending settings");
  String j = "{";
  j += "\"device\":\"" + syncDeviceId() + "\",";
  j += "\"ssid\":\"" + netGet("ssid") + "\",";
  j += "\"api\":\"" + netGet("api") + "\",";
  j += "\"ghOwner\":\"" + netGet("ghOwner") + "\",";
  j += "\"ghRepo\":\"" + netGet("ghRepo") + "\",";
  j += "\"syncHrs\":" + String(netGetU32("syncHrs", 4)) + ",";
  j += "\"sound\":" + String(netGet("sound", "1") == "1" ? "true" : "false") + ",";
  j += "\"hasWifiPass\":" + String(netGet("pass").length() ? "true" : "false") + ",";
  j += "\"hasToken\":" + String(netGet("ghToken").length() ? "true" : "false") + ",";
  j += "\"clock\":" + String((uint32_t)time(nullptr));
  j += "}";
  sendHeader(j.length());
  sendChunks((const uint8_t*)j.c_str(), j.length());
  setStatus(connected ? "connected" : "waiting");
}

/* One setting per write, as "W<key>=<value>". Sending the whole settings
   object would need chunked writes for something that is only ever a handful
   of short strings; a key at a time fits in a single packet and each one is
   acknowledged on its own. */
static void applySetting(const String& kv) {
  int eq = kv.indexOf('=');
  if (eq < 1) { sendHeader(2); sendChunks((const uint8_t*)"no", 2); return; }
  String k = kv.substring(0, eq);
  String v = kv.substring(eq + 1);
  bool ok = true;

  if      (k == "ssid")    netSet("ssid", v);
  else if (k == "pass")    netSet("pass", v);
  else if (k == "api")     netSet("api", v);
  else if (k == "ghOwner") netSet("ghOwner", v);
  else if (k == "ghRepo")  netSet("ghRepo", v);
  else if (k == "ghToken") netSet("ghToken", v);
  else if (k == "sound")   netSet("sound", v == "1" ? "1" : "0");
  else if (k == "syncHrs") {
    uint32_t h = (uint32_t)v.toInt();
    if (h == 0 || h == 1 || h == 2 || h == 4 || h == 8 || h == 24) netSetU32("syncHrs", h);
    else ok = false;
  }
  else if (k == "tzmin") {
    int m = v.toInt();
    if (m > -900 && m < 900) { netSetU32("tzmin", (uint32_t)(m + 1000)); applyTimezone(); }
    else ok = false;
  }
  else if (k == "clock") {
    long long e = atoll(v.c_str());
    if (e > 1700000000LL) {
      struct timeval tv = { .tv_sec = (time_t)e, .tv_usec = 0 };
      settimeofday(&tv, nullptr);
    } else ok = false;
  }
  else ok = false;

  const char* r = ok ? "ok" : "no";
  sendHeader(2);
  sendChunks((const uint8_t*)r, 2);
  setStatus(ok ? "saved" : "rejected");
}

static void bleTask(void*) {
  for (;;) {
    if (pending && connected) {
      String c = pendingCmd;
      if (c.startsWith("L"))      serveList();
      else if (c.startsWith("A")) serveAudio(c.substring(1));
      else if (c.startsWith("S")) serveSettings();
      else if (c.startsWith("W")) applySetting(c.substring(1));
      pending = false;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

void bleBegin() {
  if (advertising) return;
  BLEDevice::init("Mono Note Mini");
  BLEDevice::setMTU(517);                 /* bigger packets, fewer round trips */

  server = BLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  BLEService* svc = server->createService(SVC_UUID);

  cmdChar = svc->createCharacteristic(CMD_UUID, BLECharacteristic::PROPERTY_WRITE |
                                                BLECharacteristic::PROPERTY_WRITE_NR);
  cmdChar->setCallbacks(new CmdCallbacks());

  dataChar = svc->createCharacteristic(DATA_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  dataChar->addDescriptor(new BLE2902());

  svc->start();

  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SVC_UUID);
  adv->setScanResponse(true);
  BLEDevice::startAdvertising();

  advertising = true;
  setStatus("waiting");
  xTaskCreatePinnedToCore(bleTask, "ble", 8192, nullptr, 3, nullptr, 1);
}

void bleStop() {
  if (!advertising) return;
  BLEDevice::stopAdvertising();
  advertising = false;
  connected = false;
  progress = -1;
  setStatus("off");
}

bool bleAdvertising() { return advertising; }
bool bleConnected()   { return connected; }
int  bleProgress()    { return progress; }
const char* bleStatus() { return statusMsg; }
