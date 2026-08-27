#ifndef PALA_NET_H
#define PALA_NET_H
#include <Arduino.h>
#include <WebServer.h>

void netBegin();
String netGet(const char* key, const String& def = "");
void netSet(const char* key, const String& value);
uint64_t netGetU64(const char* key, uint64_t def);
void netSetU64(const char* key, uint64_t value);
bool netHasKey(const char* key);
void netSetBool(const char* key, bool v);
bool netGetBool(const char* key, bool def);
bool staConnect(uint32_t timeoutMs);
void staDisconnect();
bool portalActive();
String portalStart();
String serverStartSta();
void portalPoll();
void portalStop();
bool transcribeFile(const String& wavPath, String& outText);

#endif
