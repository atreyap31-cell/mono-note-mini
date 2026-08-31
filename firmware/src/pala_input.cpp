#include "pala_input.h"
#include "user_config.h"

/* Both buttons are active low with the internal pull-up. Measured on the real
   board: a deliberate tap lands around 100-150ms and shows no bounce, so a
   short debounce is enough and the tap ceiling can stay generous. */
#define BTN_TOP_PIN  BOOT_BUTTON_PIN   /* upper button - navigate, scroll up */
#define BTN_BOT_PIN  PWR_BUTTON_PIN    /* lower button - record, scroll down, power */

static const uint32_t DEBOUNCE_MS   = 25;
static const uint32_t TAP_MAX_MS    = 600;    /* longer than this is a hold */
static const uint32_t HOLD_MS       = 1000;   /* "hold 1s to choose" */
static const uint32_t REPEAT_FIRST  = 400;    /* first scroll step */
static const uint32_t REPEAT_EVERY  = 180;    /* and every step after */
static const uint32_t DOUBLE_GAP_MS = 400;    /* second tap must land within */
static const uint32_t POWER_MS      = 5000;   /* bottom held this long = off */

struct Btn {
  uint8_t  pin;
  bool     down       = false;
  uint32_t downAt     = 0;
  uint32_t lastEdge   = 0;
  bool     holdFired  = false;
  bool     repeating  = false;
  uint32_t nextRepeat = 0;
  bool     consumed   = false;  /* a hold already produced something */
  uint32_t lastTapAt  = 0;
  bool     tapPending = false;
};

static Btn top, bot;
static uint32_t lastActivity = 0;

void inputBegin() {
  top.pin = BTN_TOP_PIN;
  bot.pin = BTN_BOT_PIN;
  pinMode(top.pin, INPUT_PULLUP);
  pinMode(bot.pin, INPUT_PULLUP);
  lastActivity = millis();
}

/* One button's worth of edge and timing work. Emits into `ev`, using the four
   event bits belonging to this button. */
static void step(Btn& b, uint32_t now, uint16_t& ev,
                 uint16_t tapBit, uint16_t doubleBit, uint16_t holdBit,
                 uint16_t repeatBit, bool isPower) {
  bool raw = (digitalRead(b.pin) == LOW);

  if (raw != b.down && now - b.lastEdge > DEBOUNCE_MS) {
    b.lastEdge = now;
    b.down = raw;
    lastActivity = now;
    if (raw) {
      b.downAt     = now;
      b.holdFired  = false;
      b.repeating  = false;
      b.consumed   = false;
      b.nextRepeat = now + REPEAT_FIRST;
      ev |= BTN_ANY_DOWN;
    } else {
      uint32_t held = now - b.downAt;
      if (!b.consumed && held <= TAP_MAX_MS) {
        /* A tap only counts once we know no second tap is coming, so hold it
           back and let the timeout below release it as a single. */
        if (b.tapPending && now - b.lastTapAt <= DOUBLE_GAP_MS) {
          b.tapPending = false;
          ev |= doubleBit;
        } else {
          b.tapPending = true;
          b.lastTapAt  = now;
        }
      }
    }
  }

  if (b.down) {
    lastActivity = now;
    uint32_t held = now - b.downAt;
    if (isPower && held >= POWER_MS) { ev |= BTN_POWER_OFF; b.consumed = true; return; }
    if (!b.holdFired && held >= HOLD_MS) { b.holdFired = true; b.consumed = true; ev |= holdBit; }
    if (held >= REPEAT_FIRST && now >= b.nextRepeat) {
      b.nextRepeat = now + REPEAT_EVERY;
      b.consumed   = true;
      ev |= repeatBit;
    }
  }

  /* No second tap arrived in time - it was a single after all. */
  if (b.tapPending && !b.down && now - b.lastTapAt > DOUBLE_GAP_MS) {
    b.tapPending = false;
    ev |= tapBit;
  }
}

uint16_t inputPoll() {
  uint16_t ev = BTN_NONE;
  uint32_t now = millis();
  step(top, now, ev, BTN_TOP_TAP, BTN_TOP_DOUBLE, BTN_TOP_HOLD, BTN_TOP_REPEAT, false);
  step(bot, now, ev, BTN_BOT_TAP, BTN_BOT_DOUBLE, BTN_BOT_HOLD, BTN_BOT_REPEAT, true);
  return ev;
}

bool     inputAnyHeld()      { return top.down || bot.down; }
uint32_t inputLastActivity() { return lastActivity; }
