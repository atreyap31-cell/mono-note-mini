#ifndef PALA_NET_H
#define PALA_NET_H
#include <Arduino.h>

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

/* Networks in range, as JSON: [{"ssid":"...","rssi":-54,"lock":true}]. Picking
   from a list beats typing a name nobody remembers exactly. */
String netScanJson();

/* WPS push-button. The router's button is the second factor, so no password is
   typed anywhere. Returns false if it could not be started at all; success
   arrives later and is reported by netWpsState. */
bool netWpsStart();

/* 0 idle, 1 running, 2 joined and saved, 3 failed or timed out. */
int netWpsState();
void staDisconnect();

#endif
