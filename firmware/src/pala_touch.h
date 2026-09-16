#ifndef PALA_TOUCH_H
#define PALA_TOUCH_H
#include <Arduino.h>

/* The capacitive touch panel, reduced to the one question this device asks.
 *
 * The board before this had two physical buttons and a gesture layer that
 * turned presses into taps, double taps, holds and repeats. A touch screen
 * needs none of that here: there is one thing to do, and the whole screen does
 * it. So this reports taps, and where they landed, and nothing else.
 *
 * A tap is reported when the finger lifts, not when it lands. Acting on touch
 * down means a swipe or a fumbled grab fires the button, and on a recorder the
 * cost of that is a recording nobody asked for.
 */

bool touchBegin();

/* True once per tap, on release. x and y are the release position. */
bool touchTapped(int* x, int* y);

/* True while a finger is down - used to hold off the idle timeout. */
bool touchDown();

/* Where the finger is, while it is down. A tap is reported on release, which
   is right for buttons but useless for anything dragged: a slider has to
   follow the finger, not learn where it finished. */
bool touchPosition(int* x, int* y);

/* Last moment anything was touched, for the sleep timer. */
uint32_t touchLastActivity();

void touchSleep();

#endif
