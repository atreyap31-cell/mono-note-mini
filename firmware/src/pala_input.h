#ifndef PALA_INPUT_H
#define PALA_INPUT_H
#include <Arduino.h>

/* Two buttons are the whole input device on this board - there is no touch
   panel. The top button navigates; the bottom button records, scrolls down and
   powers off.

   Gestures, as the user asked for them:
     top    tap         move to the next option
     top    double tap  go back (and leave a scrolling view)
     top    hold 1s     choose the highlighted option
     top    hold        scroll up, repeating, on screens that scroll
     bottom tap         start or stop recording
     bottom hold        scroll down, repeating
     bottom hold 5s     power off

   The two meanings of a top hold - choose, and scroll up - never collide
   because a screen is either a menu or a scrolling view, never both. The
   screen decides which of the two it listens for. */

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
