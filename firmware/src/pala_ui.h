#ifndef PALA_UI_H
#define PALA_UI_H
#include <Arduino.h>
#include "pala_display.h"

/* Widgets for a 480x480 touch screen.
 *
 * Immediate mode: every frame is drawn from scratch into the canvas and taps
 * are hit-tested against the same rectangles that were just drawn. There is no
 * retained widget tree to get out of step with what is on the glass, which is
 * the failure that produced the worst bug on the old board - menu rows and the
 * code that acted on them drifted apart, and selecting one entry ran its
 * neighbour's job for days without anyone noticing.
 *
 * Here the rectangle that is drawn is the rectangle that is tested, in the
 * same call, so they cannot disagree.
 */

/* Everything the screen needs to know about a frame's input. */
struct UiTap {
  bool happened;
  int  x, y;
};

/* ---- layout constants, so screens agree without coordinating ---- */
#define UI_TABBAR_H   72
#define UI_HEADER_H   64
#define UI_PAD        16

/* ---- basic pieces ---- */
bool uiHit(const UiTap& t, int x, int y, int w, int h);

/* A button. Draws it and returns true if this frame's tap was inside it. */
bool uiButton(const UiTap& t, int x, int y, int w, int h,
              const String& label, uint16_t colour, bool filled = true);

/* A row in a list. Returns true when tapped. */
bool uiRow(const UiTap& t, int y, int h, const String& left, const String& right,
           uint16_t colour, bool selected = false);

void uiHeader(const String& title, const String& right = "");

/* The tab bar along the bottom. Returns the index tapped, or -1. */
int  uiTabBar(const UiTap& t, int active, const char* const* labels, int count);

/* A slider that follows the finger. Returns true when the value changed this
   frame, with the new value in `value`. Unlike every other control here it
   acts while held rather than on release - that is the whole point of a
   slider, and it means brightness can be judged by looking at the screen as
   it changes rather than by guessing and checking. */
bool uiSlider(int x, int y, int w, int h, int lo, int hi, int* value);

/* A pill showing battery state, drawn top-right of a header. */
void uiBatteryPill(int x, int y, int pct, bool charging);

/* ---- scrolling ----
   Lists are paged rather than kinetically scrolled. A flick needs velocity
   tracking and inertia to feel right, and done badly it feels broken; paging
   with two large arrows always works and never overshoots. */
struct UiPager {
  int page = 0;
  int perPage = 5;
  int total = 0;
  int pages() const { return total <= 0 ? 1 : (total + perPage - 1) / perPage; }
  int first() const { return page * perPage; }
};
void uiPagerBar(const UiTap& t, UiPager& p, int y);

/* ---- text entry ----
   The screen is big enough for a real keyboard, which is the whole reason a
   Wi-Fi password can now be typed on the device instead of being sent from a
   browser over Bluetooth. */
enum KeyResult { KEY_NONE = 0, KEY_CHAR, KEY_BACKSPACE, KEY_ENTER, KEY_CANCEL };

/* Draws the keyboard and reports what was pressed. `out` receives the
   character for KEY_CHAR. `shift` and `symbols` are held by the caller so the
   layout survives between frames. */
KeyResult uiKeyboard(const UiTap& t, int y, bool& shift, bool& symbols, char* out);

/* A digits-only pad, for passcodes. Returns the same results. */
KeyResult uiKeypad(const UiTap& t, int y, char* out);

/* A field showing typed text, masked for secrets. */
void uiField(int x, int y, int w, const String& text, const String& placeholder,
             bool masked);

#endif
