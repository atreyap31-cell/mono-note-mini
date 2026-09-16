#include "pala_ui.h"
#include "user_config.h"

bool uiHit(const UiTap& t, int x, int y, int w, int h) {
  return t.happened && t.x >= x && t.x < x + w && t.y >= y && t.y < y + h;
}

bool uiButton(const UiTap& t, int x, int y, int w, int h,
              const String& label, uint16_t colour, bool filled) {
  dispRoundRect(x, y, w, h, 12, filled ? colour : COL_FAINT, true);
  if (!filled) dispRoundRect(x, y, w, h, 12, colour, false);
  const int size = (h >= 56) ? TXT_BODY : TXT_SMALL;
  const int tw = dispTextWidth(label, size);
  dispText(x + (w - tw) / 2, y + (h - 8 * size) / 2, label, size,
           filled ? COL_WHITE : colour);
  return uiHit(t, x, y, w, h);
}

/* Clip a label to the space it has. Wi-Fi names run to 32 characters and note
   names are whatever the clock produced, so without this a long one simply
   runs off the edge of the panel - and on a device with no console, a word
   missing its end is the only sign anything went wrong. */
static String clipTo(const String& s, int pixels, int size) {
  const int per = 6 * size;
  const int fits = pixels / per;
  if (fits <= 0) return String("");
  if ((int)s.length() <= fits) return s;
  if (fits <= 2) return s.substring(0, fits);
  return s.substring(0, fits - 2) + "..";
}

bool uiRow(const UiTap& t, int y, int h, const String& left, const String& right,
           uint16_t colour, bool selected) {
  const int x = UI_PAD, w = LCD_WIDTH - UI_PAD * 2;
  dispRoundRect(x, y, w, h, 10, selected ? COL_BLUE : 0x1082, true);
  /* The right-hand label is laid out first and keeps its room; the left is
     given whatever is left over. */
  const int rightW = right.length() ? dispTextWidth(right, TXT_SMALL) + 12 : 0;
  dispText(x + 14, y + (h - 8 * TXT_BODY) / 2,
           clipTo(left, w - 28 - rightW, TXT_BODY), TXT_BODY, COL_WHITE);
  if (right.length()) {
    const int tw = dispTextWidth(right, TXT_SMALL);
    dispText(x + w - 14 - tw, y + (h - 8 * TXT_SMALL) / 2, right, TXT_SMALL, colour);
  }
  return uiHit(t, x, y, w, h);
}

void uiHeader(const String& title, const String& right) {
  dispFillRect(0, 0, LCD_WIDTH, UI_HEADER_H, COL_BLACK);
  const int titleRoom = LCD_WIDTH - UI_PAD * 2 -
                        (right.length() ? dispTextWidth(right, TXT_SMALL) + 16 : 0);
  dispText(UI_PAD, (UI_HEADER_H - 8 * TXT_TITLE) / 2,
           clipTo(title, titleRoom, TXT_TITLE), TXT_TITLE, COL_WHITE);
  if (right.length()) {
    const int tw = dispTextWidth(right, TXT_SMALL);
    dispText(LCD_WIDTH - UI_PAD - tw, (UI_HEADER_H - 8 * TXT_SMALL) / 2, right,
             TXT_SMALL, COL_DIM);
  }
  dispFillRect(0, UI_HEADER_H - 2, LCD_WIDTH, TXT_SMALL, COL_FAINT);
}

int uiTabBar(const UiTap& t, int active, const char* const* labels, int count) {
  const int y = LCD_HEIGHT - UI_TABBAR_H;
  const int w = LCD_WIDTH / count;
  dispFillRect(0, y, LCD_WIDTH, UI_TABBAR_H, 0x0841);
  dispFillRect(0, y, LCD_WIDTH, TXT_SMALL, COL_FAINT);
  int hit = -1;
  for (int i = 0; i < count; i++) {
    const int x = i * w;
    const bool on = (i == active);
    if (on) dispFillRect(x + 6, y + 4, w - 12, TXT_TITLE, COL_BLUE);
    const String label = labels[i];
    const int tw = dispTextWidth(label, TXT_SMALL);
    dispText(x + (w - tw) / 2, y + 28, label, TXT_SMALL, on ? COL_WHITE : COL_DIM);
    if (uiHit(t, x, y, w, UI_TABBAR_H)) hit = i;
  }
  return hit;
}

void uiBatteryPill(int x, int y, int pct, bool charging) {
  const int w = 52, h = 24;
  uint16_t colour = COL_GREEN;
  if (pct >= 0 && pct < 15)      colour = COL_RED;
  else if (pct >= 0 && pct < 40) colour = COL_AMBER;
  dispRoundRect(x, y, w, h, 6, COL_FAINT, false);
  dispFillRect(x + w + 2, y + 8, 3, 8, COL_FAINT);
  if (pct > 0) {
    int fill = (w - 6) * pct / 100;
    if (fill < 3) fill = 3;
    dispRoundRect(x + 3, y + 3, fill, h - 6, 3, colour, true);
  }
  if (charging) dispText(x - 14, y + 8, "+", TXT_SMALL, COL_GREEN);
}

void uiPagerBar(const UiTap& t, UiPager& p, int y) {
  if (p.pages() <= 1) return;
  const int bw = 96, h = 44;
  if (uiButton(t, UI_PAD, y, bw, h, "<", COL_BLUE, false) && p.page > 0) p.page--;
  if (uiButton(t, LCD_WIDTH - UI_PAD - bw, y, bw, h, ">", COL_BLUE, false)
      && p.page < p.pages() - 1) p.page++;
  String label = String(p.page + 1) + " / " + String(p.pages());
  dispTextCentered(y + 16, label, TXT_SMALL, COL_DIM);
}

void uiField(int x, int y, int w, const String& text, const String& placeholder,
             bool masked) {
  const int h = 46;
  dispRoundRect(x, y, w, h, 8, 0x1082, true);
  dispRoundRect(x, y, w, h, 8, COL_FAINT, false);
  String shown;
  if (text.length()) {
    if (masked) for (unsigned i = 0; i < text.length(); i++) shown += "*";
    else shown = text;
    /* Show the tail rather than the head: what was typed most recently is what
       somebody is checking. */
    const int fits = (w - 24) / (6 * TXT_BODY);
    if ((int)shown.length() > fits) shown = shown.substring(shown.length() - fits);
    dispText(x + 12, y + (h - 8 * TXT_BODY) / 2, shown, TXT_BODY, COL_WHITE);
  } else {
    dispText(x + 12, y + (h - 8 * TXT_SMALL) / 2, placeholder, TXT_SMALL, COL_DIM);
  }
}

/* ---- keyboard ----------------------------------------------------------
   Four rows, ten keys wide, filling the bottom of the screen. Keys are 44
   pixels square, which is a little over 7mm on this panel - about the width of
   a fingertip, and the smallest target most people hit reliably. */

static const char* KB_LOWER[4] = { "qwertyuiop", "asdfghjkl", "zxcvbnm", "" };
static const char* KB_UPPER[4] = { "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM", "" };
static const char* KB_SYMS[4]  = { "1234567890", "-/:;()$&@", ".,?!'\"", "" };

KeyResult uiKeyboard(const UiTap& t, int y, bool& shift, bool& symbols, char* out) {
  const int kw = 46, kh = 44, gap = 2;
  const char* const* rows = symbols ? KB_SYMS : (shift ? KB_UPPER : KB_LOWER);
  KeyResult result = KEY_NONE;

  for (int r = 0; r < 3; r++) {
    const String row = rows[r];
    const int n = row.length();
    if (!n) continue;
    const int rowW = n * kw + (n - 1) * gap;
    const int x0 = (LCD_WIDTH - rowW) / 2;
    const int ry = y + r * (kh + gap);
    for (int i = 0; i < n; i++) {
      const int kx = x0 + i * (kw + gap);
      dispRoundRect(kx, ry, kw, kh, 6, 0x2104, true);
      String ch = String(row[i]);
      dispText(kx + (kw - 6 * TXT_BODY) / 2, ry + (kh - 8 * TXT_BODY) / 2, ch,
               TXT_BODY, COL_WHITE);
      if (uiHit(t, kx, ry, kw, kh)) { *out = row[i]; result = KEY_CHAR; }
    }
  }

  /* Bottom row: shift, symbols, space, backspace, done. */
  const int by = y + 3 * (kh + gap);
  const int sw = 68;
  /* Two separate toggles rather than one that means different things in
     different states: shift for capitals, and a switch between letters and
     symbols. A single key that did both was ambiguous to label and worse to
     use. */
  if (uiButton(t, 4, by, sw, kh, shift ? "SHIFT" : "shift",
               shift ? COL_BLUE : COL_DIM, shift)) {
    shift = !shift;
    symbols = false;
    result = KEY_NONE;
  }
  if (uiButton(t, 4 + sw + gap, by, sw, kh, symbols ? "abc" : "?123",
               symbols ? COL_BLUE : COL_DIM, symbols)) {
    symbols = !symbols;
    result = KEY_NONE;
  }
  const int spx = 4 + 2 * (sw + gap);
  const int spw = LCD_WIDTH - spx - 4 - 2 * (sw + gap);
  if (uiButton(t, spx, by, spw, kh, "space", COL_DIM, false)) { *out = ' '; result = KEY_CHAR; }
  if (uiButton(t, spx + spw + gap, by, sw, kh, "del", COL_AMBER, false)) result = KEY_BACKSPACE;
  if (uiButton(t, spx + spw + gap + sw + gap, by, sw, kh, "go", COL_GREEN, true)) result = KEY_ENTER;
  return result;
}

KeyResult uiKeypad(const UiTap& t, int y, char* out) {
  const int kw = 96, kh = 66, gap = 10;
  const int x0 = (LCD_WIDTH - (3 * kw + 2 * gap)) / 2;
  KeyResult result = KEY_NONE;
  const char* keys = "123456789";

  for (int i = 0; i < 9; i++) {
    const int kx = x0 + (i % 3) * (kw + gap);
    const int ky = y + (i / 3) * (kh + gap);
    dispRoundRect(kx, ky, kw, kh, 10, 0x2104, true);
    String ch = String(keys[i]);
    dispText(kx + (kw - 6 * TXT_BIG) / 2, ky + (kh - 8 * TXT_BIG) / 2, ch,
             TXT_BIG, COL_WHITE);
    if (uiHit(t, kx, ky, kw, kh)) { *out = keys[i]; result = KEY_CHAR; }
  }
  const int by = y + 3 * (kh + gap);
  if (uiButton(t, x0, by, kw, kh, "del", COL_AMBER, false)) result = KEY_BACKSPACE;
  dispRoundRect(x0 + kw + gap, by, kw, kh, 10, 0x2104, true);
  dispText(x0 + kw + gap + (kw - 6 * TXT_BIG) / 2, by + (kh - 8 * TXT_BIG) / 2, "0",
           TXT_BIG, COL_WHITE);
  if (uiHit(t, x0 + kw + gap, by, kw, kh)) { *out = '0'; result = KEY_CHAR; }
  if (uiButton(t, x0 + 2 * (kw + gap), by, kw, kh, "ok", COL_GREEN, true)) result = KEY_ENTER;
  return result;
}
