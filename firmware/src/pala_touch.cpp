#include "pala_touch.h"
#include "user_config.h"
#include <Wire.h>
#include <TouchDrv.hpp>

static TouchDrvCST92xx touch;
static bool     ready      = false;
static bool     wasDown    = false;
static int      lastX = 0, lastY = 0;
static bool     tapPending = false;
static uint32_t lastActive = 0;
static uint32_t downAt     = 0;

/* Long enough to be a press, short enough not to feel sticky. Below about
   30ms the panel reports contact that a finger never made. */
static const uint32_t TAP_MIN_MS = 30;
/* A finger resting on the screen is not a decision. Anything longer than this
   is treated as a lean or a pocket, and ignored. */
static const uint32_t TAP_MAX_MS = 1500;

bool touchBegin() {
  Wire.begin(ESP32_I2C_SDA_PIN, ESP32_I2C_SCL_PIN);
  Wire.setClock(400000);
  touch.setPins(TOUCH_RST_PIN, TOUCH_INT_PIN);
  ready = touch.begin(Wire, TOUCH_I2C_ADDR, ESP32_I2C_SDA_PIN, ESP32_I2C_SCL_PIN);
  lastActive = millis();
  return ready;
}

/* Polled rather than driven from the INT pin. The panel is read every loop
   anyway, the loop is short, and an interrupt would only add a way for a
   missed edge to leave the device believing a finger is still down. */
static void poll() {
  if (!ready) return;
  TouchPoints points = touch.getTouchPoints();
  const bool down = points.getPointCount() > 0;

  if (down) {
    const TouchPoint& p = points.getPoint(0);
    lastX = p.x;
    lastY = p.y;
    lastActive = millis();
    if (!wasDown) downAt = millis();
  } else if (wasDown) {
    /* Released. Decide here, on the way up, so a swipe that started on the
       button does not count as pressing it. */
    uint32_t held = millis() - downAt;
    if (held >= TAP_MIN_MS && held <= TAP_MAX_MS) tapPending = true;
    lastActive = millis();
  }
  wasDown = down;
}

bool touchTapped(int* x, int* y) {
  poll();
  if (!tapPending) return false;
  tapPending = false;
  if (x) *x = lastX;
  if (y) *y = lastY;
  return true;
}

bool     touchDown()          { return wasDown; }

bool touchPosition(int* x, int* y) {
  if (!wasDown) return false;
  if (x) *x = lastX;
  if (y) *y = lastY;
  return true;
}
uint32_t touchLastActivity()  { return lastActive; }
void     touchSleep()         { if (ready) touch.sleep(); }
