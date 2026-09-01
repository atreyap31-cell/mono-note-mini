#include "pala_ui.h"
#include <esp_heap_caps.h>
#include <string.h>
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

/* Full refresh drives the panel with waveform 0xc7, which inverts the whole
   screen twice on its way to the new image - the flash. Partial uses 0xcf and
   simply moves the pixels that changed. Every screen used the full path, so
   moving a menu highlight cost a two second flash to change one row. */
/* A copy of the last thing pushed to the panel. Diffing against it says which
   rows actually changed, so the window can be narrowed without any draw
   function having to declare what it touched - they all clear and repaint the
   whole buffer, so asking them would get "everything" every time. 5000 bytes
   in PSRAM to avoid pushing 5000 bytes over SPI and driving 200 rows of
   waveform to move a highlight down by one. */
static uint8_t* shadow = nullptr;
static int  partialsSince = 0;
static bool inPartialMode = false;
static int  lastScreenId  = 0;
/* How many partial updates before the panel wants clearing. This was 4, set
   while chasing ghosting that turned out to be a screen-change bug rather than
   accumulated residue - so it fired on the fifth press of every menu, which is
   exactly where it is most annoying. Entering any screen is already a full
   refresh, so in practice the count rarely gets near this. */
static const int PARTIAL_LIMIT = 16;

/* Rows [r0,r1] that differ from what the panel is currently showing.
   Returns false when nothing changed at all - which happens more often than
   expected, and skipping those saves a whole panel update. */
static bool dirtyRows(int& r0, int& r1) {
  const uint8_t* buf = epd->frameBuffer();
  const int stride = epd->frameStride();
  if (!shadow || !buf) return false;
  r0 = -1; r1 = -1;
  for (int y = 0; y < 200; y++) {
    if (memcmp(buf + y * stride, shadow + y * stride, stride) != 0) {
      if (r0 < 0) r0 = y;
      r1 = y;
    }
  }
  return r0 >= 0;
}

static void keepShadow() {
  const uint8_t* buf = epd->frameBuffer();
  if (!shadow) shadow = (uint8_t*)heap_caps_malloc(5000, MALLOC_CAP_SPIRAM);
  if (!shadow) shadow = (uint8_t*)malloc(5000);
  if (shadow && buf) memcpy(shadow, buf, 5000);
}

void uiFlushFull() {
  if (inPartialMode) { epd->EPD_Init(); inPartialMode = false; }
  epd->EPD_Display();
  partialsSince = 0;
  lastScreenId = 0;          /* whatever comes next is a new screen */
  keepShadow();
}

void uiFlushFast(int screenId) {
  /* Arriving from somewhere else: clear properly. A partial update here leaves
     the previous screen showing through, which is the whole complaint. */
  if (screenId != lastScreenId || !inPartialMode) {
    if (inPartialMode) { epd->EPD_Init(); inPartialMode = false; }
    epd->EPD_Display();                 /* full, clean, no ghost */
    epd->EPD_Init_Partial();
    epd->EPD_DisplayPartBaseImage();    /* base for the updates that follow */
    inPartialMode = true;
    partialsSince = 0;
    lastScreenId  = screenId;
    keepShadow();
    return;
  }
  int r0, r1;
  if (!dirtyRows(r0, r1)) return;      /* identical - nothing to push */
  partialsSince++;
  /* A margin either side: the waveform disturbs a row or two beyond what was
     written, and leaving those out shows as a faint seam. */
  epd->EPD_DisplayPartRows(r0 - 2, r1 + 2);
  keepShadow();
}

/* The cleanup no longer interrupts navigation. Rather than flashing on the nth
   press - always mid-scroll, always when someone is looking at it - the loop
   asks whether one is due and does it during a pause instead. The panel is
   just as clean; the flash simply happens when nobody is pressing anything. */
bool uiGhostingDue() { return inPartialMode && partialsSince >= PARTIAL_LIMIT; }

void uiClearGhosting() {
  if (!inPartialMode) return;
  epd->EPD_Init();
  inPartialMode = false;
  epd->EPD_Display();                 /* same image, cleanly rendered */
  epd->EPD_Init_Partial();
  epd->EPD_DisplayPartBaseImage();
  inPartialMode = true;
  partialsSince = 0;
  keepShadow();
}

void uiFlushPartialPrepare() {
  epd->EPD_DisplayPartBaseImage();
}

void uiFlushPartial() {
  epd->EPD_DisplayPart();
}
