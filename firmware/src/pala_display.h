#ifndef PALA_DISPLAY_H
#define PALA_DISPLAY_H
#include <Arduino.h>

/* The screen.
 *
 * A 480x480 colour AMOLED on a quad-SPI bus, which is a different world from
 * the 200x200 one-bit e-paper this replaced. Three things follow from that and
 * shape everything drawn here:
 *
 *   It is fast. The e-paper took about 2.7 seconds for a full refresh and
 *   flashed the whole panel doing it, so the old firmware went to great
 *   lengths never to redraw - a recording showed a circle and nothing moved
 *   until it stopped. Here a frame is a few milliseconds, so the elapsed time
 *   can simply tick.
 *
 *   It does not hold an image without power. E-paper kept its last picture
 *   forever, which is why the old device could sleep showing something useful.
 *   This one goes black. Sleeping is now genuinely off rather than merely
 *   still.
 *
 *   Black costs nothing. On an AMOLED an unlit pixel draws no current, so a
 *   dark interface is the efficient one as well as the better-looking one.
 *   Everything here is light on black for that reason.
 *
 * Drawing goes into a full-frame buffer in PSRAM and is pushed in one go, so
 * nothing is ever seen half-drawn.
 */

/* RGB565. Named rather than scattered, so the palette is one place. */
#define COL_BLACK   0x0000
#define COL_WHITE   0xFFFF
#define COL_DIM     0x52AA   /* grey, for things that are present but quiet */
#define COL_FAINT   0x2104   /* barely there - rails, empty track */
#define COL_RED     0xF986   /* recording */
#define COL_GREEN   0x2E6B   /* synced, charged, good */
#define COL_AMBER   0xFD20   /* waiting, part-done */
#define COL_BLUE    0x2D7F   /* touchable things */

bool dispBegin();
void dispBrightness(uint8_t level);     /* 0-255 */
void dispSleep(bool off);

/* One frame: clear, draw, show. Nothing reaches the glass until dispShow. */
void dispClear(uint16_t colour = COL_BLACK);
void dispShow();

void dispFillRect(int x, int y, int w, int h, uint16_t colour);
void dispRect(int x, int y, int w, int h, uint16_t colour);
void dispRoundRect(int x, int y, int w, int h, int r, uint16_t colour, bool filled);
void dispFillCircle(int cx, int cy, int r, uint16_t colour);
void dispCircle(int cx, int cy, int r, uint16_t colour);
void dispArc(int cx, int cy, int r, int thickness, float fromDeg, float toDeg, uint16_t colour);

/* Text is drawn from the built-in font at integer scales. size 1 is 6x8. */
void dispText(int x, int y, const String& s, int size, uint16_t colour);
void dispTextCentered(int y, const String& s, int size, uint16_t colour);
int  dispTextWidth(const String& s, int size);

/* The wordmark is a one-bit bitmap inherited from the e-paper board, where
   one bit was all there was. Drawn in whatever colour is asked for. */
void dispBitmap1(int x, int y, int w, int h, const uint8_t* bits, uint16_t colour);

#endif
