#include "pala_ui.h"
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

void uiText(int x, int y, const String& text, int scale, uint8_t color) {
  for (unsigned int n = 0; n < text.length(); n++) {
    char ch = text[n];
    if (ch < 0x20 || ch > 0x7F) ch = '?';
    const unsigned char* glyph = &PALA_FONT[(ch - 0x20) * 5];
    for (int col = 0; col < 5; col++) {
      unsigned char bits = pgm_read_byte(&glyph[col]);
      for (int row = 0; row < 7; row++)
        if (bits & (0x80 >> row))
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

void uiFlushFull() {
  epd->EPD_Display();
}

void uiFlushPartialPrepare() {
  epd->EPD_DisplayPartBaseImage();
}

void uiFlushPartial() {
  epd->EPD_DisplayPart();
}
