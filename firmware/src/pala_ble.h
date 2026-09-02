#ifndef PALA_BLE_H
#define PALA_BLE_H
#include <Arduino.h>

/* Bluetooth transfer, so a browser next to the device can read its notes with
   no Wi-Fi, no server, no GitHub account and nothing installed.

   This is not audio streaming to a speaker - the ESP32-S3 has no Bluetooth
   Classic radio at all, so A2DP is impossible on this hardware at any firmware
   version. What it does is carry the notes themselves over BLE to a web page
   using the Web Bluetooth API.

   That also sidesteps a wall the Wi-Fi route runs into: a page served over
   HTTPS is forbidden by every browser from calling an http:// address on the
   local network, which is why the hosted web app cannot talk to the device
   over Wi-Fi. Bluetooth is not HTTP, so the restriction does not apply and the
   hosted page can talk to the device directly.

   Web Bluetooth works in Chrome and Edge on desktop, and Chrome on Android.
   It does not exist on iOS in any browser - Apple requires every iOS browser
   to use WebKit, and WebKit has never shipped it. */

void bleBegin();          /* start advertising */
void bleStop();
bool bleAdvertising();
bool bleConnected();

/* Progress of the transfer in flight, for the screen to show. 0-100, or -1
   when nothing is being sent. */
int  bleProgress();

/* What the device is doing, short enough for a 200px screen. */
const char* bleStatus();

#endif
