#ifndef PALA_NET_H
#define PALA_NET_H
#include <Arduino.h>
#include <WebServer.h>

/* Auto-sync interval in hours. 0 = off. Stored in NVS under "syncHrs". */
#define SYNC_HOURS_DEFAULT 4

void netBegin();
void applyTimezone();   /* local time for note names, not UTC */
String netGet(const char* key, const String& def = "");
void netSet(const char* key, const String& value);
uint64_t netGetU64(const char* key, uint64_t def);
void netSetU64(const char* key, uint64_t value);
uint32_t netGetU32(const char* key, uint32_t def);
void netSetU32(const char* key, uint32_t value);
bool netHasKey(const char* key);
void netSetBool(const char* key, bool v);
bool netGetBool(const char* key, bool def);
void netClearAll();
bool staConnect(uint32_t timeoutMs);
void staDisconnect();
bool portalActive();
String portalStart();
String serverStartSta();
void portalPoll();
void portalStop();
bool transcribeFile(const String& wavPath, String& outText);

#endif
