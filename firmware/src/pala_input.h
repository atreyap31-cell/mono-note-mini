#ifndef PALA_INPUT_H
#define PALA_INPUT_H
#include <Arduino.h>

/* Two buttons exist on this board, and the device uses one of them.
 *
 *   top    tap   start recording, or stop it
 *   either -     wakes it from sleep, and does nothing else
 *
 * The rest of the gestures below - double tap, hold, the repeating scroll -
 * are still detected because the layer was written for a device with menus.
 * Nothing reads them now. They are cheap to leave and were expensive to get
 * right, so they stay for whatever comes next rather than being deleted and
 * rewritten from memory later.
 *
 * One rule about this file has not changed and must not: never print from the
 * sampling task. A write to the USB console blocks until a host drains it,
 * and that froze every button on the device for an evening. */

enum BtnEvent : uint16_t {
  BTN_NONE          = 0,
  BTN_TOP_TAP       = 1 << 0,
  BTN_TOP_DOUBLE    = 1 << 1,
  BTN_TOP_HOLD      = 1 << 2,   /* fires once, at 1s, while still held */
  BTN_TOP_REPEAT    = 1 << 3,   /* keeps firing while held - scroll up */
  BTN_BOT_TAP       = 1 << 4,
  BTN_BOT_DOUBLE    = 1 << 5,
  BTN_BOT_HOLD      = 1 << 6,
  BTN_BOT_REPEAT    = 1 << 7,   /* keeps firing while held - scroll down */
  BTN_POWER_OFF     = 1 << 8,   /* bottom held 5s */
  BTN_ANY_DOWN      = 1 << 9,   /* either button went down - wakes the screen */
};

void     inputBegin();
uint16_t inputPoll();          /* call every loop; returns a mask of the above */
bool     inputAnyHeld();       /* true while either button is down */
uint32_t inputLastActivity();

#endif
