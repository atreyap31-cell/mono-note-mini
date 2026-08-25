#ifndef PALA_NET_H
#define PALA_NET_H
#include <Arduino.h>
#include <WebServer.h>

void netBegin();
String netGet(const char* key, const String& def = "");
void netSet(const char* key, const String& value);
bool staConnect(uint32_t timeoutMs);
void staDisconnect();
bool portalActive();
String portalStart();
void portalPoll();
void portalStop();
bool transcribeFile(const String& wavPath, String& outText);

#endif
