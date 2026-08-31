#include "pala_ui.h"
#include <math.h>
#include "epaper_driver_bsp.h"
#include "glcdfont.h"

static epaper_driver_display* epd = nullptr;

void uiBegin(epaper_driver_display* d) { epd = d; }

void uiFillRect(int x, int y, int w, int h, uint8_t color) {
  for (int j = 0; j < h; j++)
    for (int i = 0; i < w; i++) {
      int px = x + i, py = y + j;
      if (px >= 0 && px < 200 && py >= 0 && py < 200) epd->EPD_DrawColorPixel(px, py, color);
    }
}

void uiRect(int x, int y, int w, int h, uint8_t color) {
  uiFillRect(x, y, w, 1, color);
  uiFillRect(x, y + h - 1, w, 1, color);
  uiFillRect(x, y, 1, h, color);
  uiFillRect(x + w - 1, y, 1, h, color);
}

/* PALA_FONT is the standard 5x8 GLCD table: 256 glyphs from 0x00, five column
   bytes each, top pixel in bit 0. Index and bit order both have to match that
   or every glyph comes out as a fragment of some unrelated character. */
void uiText(int x, int y, const String& text, int scale, uint8_t color) {
  for (unsigned int n = 0; n < text.length(); n++) {
    char ch = text[n];
    if (ch < 0x20 || ch > 0x7E) ch = '?';
    const unsigned char* glyph = &PALA_FONT[(unsigned char)ch * 5];
    for (int col = 0; col < 5; col++) {
      unsigned char bits = pgm_read_byte(&glyph[col]);
      for (int row = 0; row < 8; row++)
        if (bits & (1 << row))
          for (int sy = 0; sy < scale; sy++)
            for (int sx = 0; sx < scale; sx++) {
              int px = x + n * 6 * scale + col * scale + sx;
              int py = y + row * scale + sy;
              if (px >= 0 && px < 200 && py >= 0 && py < 200) epd->EPD_DrawColorPixel(px, py, color);
            }
    }
  }
}

int uiTextWidth(const String& text, int scale) { return text.length() * 6 * scale - scale; }

void uiTextCentered(int y, const String& text, int scale, uint8_t color) {
  int x = (200 - uiTextWidth(text, scale)) / 2;
  if (x < 0) x = 0;
  uiText(x, y, text, scale, color);
}

void uiFillCircle(int cx, int cy, int r, uint8_t color) {
  for (int dy = -r; dy <= r; dy++) {
    int half = (int)(sqrtf((float)(r * r - dy * dy)) + 0.5f);
    uiFillRect(cx - half, cy + dy, half * 2 + 1, 1, color);
  }
}

void uiRow(int x, int y, int w, int h, const String& label, int scale, bool selected) {
  if (selected) uiFillRect(x, y, w, h, 0x00);
  else          uiRect(x, y, w, h, 0x00);
  int ty = y + (h - 8 * scale) / 2;
  uiTextCenteredIn(x, w, ty, label, scale, selected ? 0xff : 0x00);
}

void uiBitmap(int x, int y, int w, int h, const unsigned char* data, uint8_t color) {
  const int stride = (w + 7) / 8;
  for (int j = 0; j < h; j++) {
    for (int i = 0; i < w; i++) {
      unsigned char b = pgm_read_byte(&data[j * stride + (i >> 3)]);
      if (!(b & (0x80 >> (i & 7)))) continue;
      int px = x + i, py = y + j;
      if (px >= 0 && px < 200 && py >= 0 && py < 200) epd->EPD_DrawColorPixel(px, py, color);
    }
  }
}

/* Centre inside a box - used for button labels. */
void uiTextCenteredIn(int x, int w, int y, const String& text, int scale, uint8_t color) {
  int tx = x + (w - uiTextWidth(text, scale)) / 2;
  if (tx < x) tx = x;
  uiText(tx, y, text, scale, color);
}

void uiFlushFull() {
  epd->EPD_Display();
}

void uiFlushPartialPrepare() {
  epd->EPD_DisplayPartBaseImage();
}

void uiFlushPartial() {
  epd->EPD_DisplayPart();
}
