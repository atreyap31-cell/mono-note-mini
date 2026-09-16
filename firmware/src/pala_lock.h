#ifndef PALA_LOCK_H
#define PALA_LOCK_H
#include <Arduino.h>

/* The device passcode.
 *
 * Chosen the first time the device is used rather than shipped as a default.
 * The board before this one shipped with 1234 and warned about it on screen,
 * which meant the overwhelmingly likely state of any given device was "still
 * 1234" - a password everybody has is not one.
 *
 * What is stored is a salted PBKDF2-HMAC-SHA256 hash, never the passcode. A
 * card or a chip read out elsewhere yields the hash and the salt, and getting
 * from those back to a four digit code still costs the attacker the full
 * iteration count per guess.
 *
 * The failure count and the lockout live in NVS, not in RAM. That is the part
 * that actually matters: a backoff held in memory is defeated by pulling the
 * power, which is not a difficult attack on a device somebody is holding.
 */

#define LOCK_MIN_LEN 4
#define LOCK_MAX_LEN 12

bool lockBegin();

/* Has a passcode ever been chosen? False means first run. */
bool lockIsSet();

/* Set or replace the passcode. Changing one is the caller's job to authorise -
   verify the old one first. Returns false if it is too short or too long. */
bool lockSet(const String& code);

/* Checks a passcode. A wrong answer is counted and slows the next attempt. */
bool lockVerify(const String& code);

/* Milliseconds the caller must wait before the next attempt is accepted.
   Zero when there is no penalty outstanding. */
uint32_t lockPenaltyMs();

/* How many consecutive wrong answers have been given. */
int lockFailures();

bool lockUnlocked();
void lockRelock();

/* Forget the passcode entirely. Only for a factory reset, which also throws
   away everything else. */
void lockClear();

#endif
