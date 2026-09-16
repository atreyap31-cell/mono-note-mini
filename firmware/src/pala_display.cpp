#include "pala_display.h"
#include "user_config.h"
#include <Arduino_GFX_Library.h>
#include <esp_heap_caps.h>
#include <math.h>

/* The canvas is 480 x 480 x 2 bytes = 460 KB, and the whole chip has 512 KB of
   internal RAM. Arduino_Canvas::begin allocates with aligned_alloc, which is
   the ordinary heap, so left to itself it either fails outright or succeeds and
   starves everything else - and a failure there looks exactly like the display
   never initialising.

   begin() only allocates when the buffer is still null, so handing it one from
   PSRAM first is enough to place it deliberately. */
class PsramCanvas : public Arduino_Canvas {
public:
  PsramCanvas(int16_t w, int16_t h, Arduino_G* out) : Arduino_Canvas(w, h, out) {}
  bool beginInPsram() {
    if (!_framebuffer) {
      _framebuffer = (uint16_t*)heap_caps_aligned_alloc(
          16, (size_t)LCD_WIDTH * LCD_HEIGHT * 2, MALLOC_CAP_SPIRAM);
      if (!_framebuffer) return false;
    }
    return begin();
  }
};

static Arduino_DataBus* bus    = nullptr;
static Arduino_GFX*     panel  = nullptr;
static PsramCanvas*     canvas = nullptr;   /* the frame we actually draw into */

bool dispBegin() {
  bus = new Arduino_ESP32QSPI(LCD_CS_PIN, LCD_SCLK_PIN,
                              LCD_SDIO0_PIN, LCD_SDIO1_PIN,
                              LCD_SDIO2_PIN, LCD_SDIO3_PIN);
  panel = new Arduino_CO5300(bus, LCD_RESET_PIN, 0 /* rotation */,
                             LCD_WIDTH, LCD_HEIGHT, 0, 0, 0, 0);
  canvas = new PsramCanvas(LCD_WIDTH, LCD_HEIGHT, panel);
  if (!canvas->beginInPsram()) return false;
  canvas->fillScreen(COL_BLACK);
  canvas->flush();
  dispBrightness(200);
  return true;
}

void dispBrightness(uint8_t level) {
  /* Not virtual on Arduino_GFX - brightness is a CO5300 command, not a
     property every panel has. */
  if (panel) ((Arduino_CO5300*)panel)->setBrightness(level);
}

void dispSleep(bool off) {
  if (!panel) return;
  /* Brightness to zero as well as the panel's own sleep: on this controller a
     display-off still leaves a faint glow on some units. */
  if (off) { dispBrightness(0); panel->displayOff(); }
  /* Waking does not pick a brightness: the caller has a stored setting and
     restoring 200 here would quietly override it every time. */
  else     { panel->displayOn(); }
}

/* Write CE - the panel's own contrast enhancement. The values are the ones in
   the driver's init table, which lists them commented out: 0x00 off, then 05,
   06 and 07 for low, medium and high. */
void dispSunlight(uint8_t level) {
  if (!bus) return;
  static const uint8_t CE[4] = { 0x00, 0x05, 0x06, 0x07 };
  if (level > 3) level = 3;
  bus->beginWrite();
  bus->writeC8D8(0x58 /* CO5300_W_WCE */, CE[level]);
  bus->endWrite();
}

void dispClear(uint16_t colour) { if (canvas) canvas->fillScreen(colour); }
void dispShow()                 { if (canvas) canvas->flush(); }

void dispFillRect(int x, int y, int w, int h, uint16_t c) { if (canvas) canvas->fillRect(x, y, w, h, c); }
void dispRect(int x, int y, int w, int h, uint16_t c)     { if (canvas) canvas->drawRect(x, y, w, h, c); }
void dispFillCircle(int cx, int cy, int r, uint16_t c)    { if (canvas) canvas->fillCircle(cx, cy, r, c); }
void dispCircle(int cx, int cy, int r, uint16_t c)        { if (canvas) canvas->drawCircle(cx, cy, r, c); }

void dispRoundRect(int x, int y, int w, int h, int r, uint16_t c, bool filled) {
  if (!canvas) return;
  if (filled) canvas->fillRoundRect(x, y, w, h, r, c);
  else        canvas->drawRoundRect(x, y, w, h, r, c);
}

/* A ring segment, drawn as a run of filled circles along the arc. Cruder than
   a proper polygon fill and perfectly adequate at this size: the dots overlap
   at any sensible thickness, and it avoids a trigonometry-heavy span fill for
   something used twice. */
void dispArc(int cx, int cy, int r, int thickness, float fromDeg, float toDeg, uint16_t colour) {
  if (!canvas) return;
  if (toDeg < fromDeg) { float t = fromDeg; fromDeg = toDeg; toDeg = t; }
  /* One step per degree is dense enough that the dots touch; fewer leaves
     visible scalloping on the outside edge. */
  const float step = 1.0f;
  const int   rad  = thickness / 2;
  for (float a = fromDeg; a <= toDeg; a += step) {
    float rr = a * (float)M_PI / 180.0f;
    int x = cx + (int)lroundf(cosf(rr) * r);
    int y = cy + (int)lroundf(sinf(rr) * r);
    canvas->fillCircle(x, y, rad, colour);
  }
}

void dispText(int x, int y, const String& s, int size, uint16_t colour) {
  if (!canvas) return;
  canvas->setTextColor(colour);
  canvas->setTextSize(size);
  canvas->setCursor(x, y);
  canvas->print(s);
}

int dispTextWidth(const String& s, int size) {
  /* The built-in font is a fixed 6x8 cell, so this is exact rather than an
     estimate, and centring never drifts. */
  return (int)s.length() * 6 * size;
}

void dispTextCentered(int y, const String& s, int size, uint16_t colour) {
  dispText((LCD_WIDTH - dispTextWidth(s, size)) / 2, y, s, size, colour);
}

void dispBitmap1(int x, int y, int w, int h, const uint8_t* bits, uint16_t colour) {
  if (canvas) canvas->drawBitmap(x, y, (uint8_t*)bits, w, h, colour);
}
