#ifndef PALA_UI_H
#define PALA_UI_H
#include <Arduino.h>

void uiBegin(class epaper_driver_display* d);
void uiFillRect(int x, int y, int w, int h, uint8_t color = 0x00);
void uiRect(int x, int y, int w, int h, uint8_t color = 0x00);
void uiText(int x, int y, const String& text, int scale = 1, uint8_t color = 0x00);
int uiTextWidth(const String& text, int scale = 1);
void uiTextCentered(int y, const String& text, int scale = 1, uint8_t color = 0x00);
void uiTextCenteredIn(int x, int w, int y, const String& text, int scale = 1, uint8_t color = 0x00);
/* Filled circle and square, for the recording indicator. Two shapes drawn once
   each beat a counter that repaints: e-paper ghosts and wears under constant
   partial refreshes, and a note's length is on the file anyway. */
void uiFillCircle(int cx, int cy, int r, uint8_t color = 0x00);

/* A menu row. When selected it fills solid and the label reverses out, which
   on e-paper reads far better than a thin outline - there is no colour or
   backlight to lean on. */
void uiRow(int x, int y, int w, int h, const String& label, int scale, bool selected);

/* 1bpp row-major bitmap, MSB leftmost; only set bits are drawn. */
void uiBitmap(int x, int y, int w, int h, const unsigned char* data, uint8_t color = 0x00);
void uiFlushFull();
/* Fast redraw for moving a highlight around a screen that is otherwise
   unchanged: no black/white flash, and a fraction of the time. The panel
   accumulates ghosting under repeated partial updates, so this falls back to a
   full refresh periodically on its own. */
void uiFlushFast();
void uiFlushPartialPrepare();
void uiFlushPartial();

#endif


