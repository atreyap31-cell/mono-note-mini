#ifndef PALA_UI_H
#define PALA_UI_H
#include <Arduino.h>

void uiBegin(class epaper_driver_display* d);
void uiFillRect(int x, int y, int w, int h, uint8_t color);
void uiRect(int x, int y, int w, int h, uint8_t color = 0x00);
void uiText(int x, int y, const String& text, int scale = 1, uint8_t color = 0x00);
int uiTextWidth(const String& text, int scale = 1);
void uiTextCentered(int y, const String& text, int scale = 1, uint8_t color = 0x00);
void uiFlushFull();
void uiFlushPartialPrepare();
void uiFlushPartial();

#endif

