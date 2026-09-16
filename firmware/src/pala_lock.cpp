#include "pala_lock.h"
#include "pala_net.h"
#include <mbedtls/pkcs5.h>
#include <mbedtls/md.h>
#include <esp_random.h>

/* Enough to make guessing expensive and few enough that unlocking does not
   feel broken. On this chip, with hardware SHA, this lands around a fifth of a
   second - the old firmware used 120,000 for its encryption key and the result
   was indistinguishable from a crash, because nothing was on screen while it
   ran. The caller here shows "checking" first. */
static const int PBKDF2_ROUNDS = 25000;

static bool unlocked = false;

static String toHex(const uint8_t* b, size_t n) {
  String s;
  s.reserve(n * 2);
  for (size_t i = 0; i < n; i++) {
    char h[3];
    snprintf(h, sizeof(h), "%02x", b[i]);
    s += h;
  }
  return s;
}

static bool fromHex(const String& s, uint8_t* out, size_t n) {
  if (s.length() != n * 2) return false;
  for (size_t i = 0; i < n; i++) {
    char h[3] = { s[i * 2], s[i * 2 + 1], 0 };
    out[i] = (uint8_t)strtoul(h, nullptr, 16);
  }
  return true;
}

static bool derive(const String& code, const uint8_t salt[16], uint8_t out[32]) {
  const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!md) return false;
  return mbedtls_pkcs5_pbkdf2_hmac_ext(
             MBEDTLS_MD_SHA256,
             (const unsigned char*)code.c_str(), code.length(),
             salt, 16, PBKDF2_ROUNDS, 32, out) == 0;
}

bool lockBegin() { unlocked = false; return true; }

bool lockIsSet() {
  return netGet("lockHash").length() == 64 && netGet("lockSalt").length() == 32;
}

bool lockSet(const String& code) {
  if ((int)code.length() < LOCK_MIN_LEN || (int)code.length() > LOCK_MAX_LEN) return false;

  uint8_t salt[16];
  esp_fill_random(salt, sizeof(salt));
  uint8_t hash[32];
  if (!derive(code, salt, hash)) return false;

  netSet("lockSalt", toHex(salt, sizeof(salt)));
  netSet("lockHash", toHex(hash, sizeof(hash)));
  netSetU32("lockFails", 0);
  unlocked = true;                  /* choosing it proves you know it */
  return true;
}

/* The backoff schedule. Deliberately harsh past a handful of tries: a person
   who has forgotten their own passcode gets three free goes and then a few
   seconds, while somebody working through all ten thousand four-digit codes
   runs into hours before they are a fraction of the way through. */
static uint32_t penaltyFor(uint32_t fails) {
  if (fails < 3)  return 0;
  if (fails < 5)  return 5000;
  if (fails < 8)  return 30000;
  if (fails < 12) return 300000;
  return 3600000;
}

uint32_t lockPenaltyMs() { return penaltyFor(netGetU32("lockFails", 0)); }
int      lockFailures()  { return (int)netGetU32("lockFails", 0); }

bool lockVerify(const String& code) {
  uint8_t salt[16], want[32], got[32];
  if (!fromHex(netGet("lockSalt"), salt, sizeof(salt))) return false;
  if (!fromHex(netGet("lockHash"), want, sizeof(want))) return false;
  if (!derive(code, salt, got)) return false;

  /* Constant time: a comparison that returns early leaks how much of the hash
     matched, and with it a way to solve the passcode a byte at a time. */
  uint8_t diff = 0;
  for (int i = 0; i < 32; i++) diff |= (uint8_t)(want[i] ^ got[i]);

  if (diff == 0) {
    netSetU32("lockFails", 0);
    unlocked = true;
    return true;
  }
  /* Counted in NVS before returning, so pulling the power mid-guess does not
     wipe the penalty - which is the whole point of keeping it there. */
  netSetU32("lockFails", netGetU32("lockFails", 0) + 1);
  return false;
}

bool lockUnlocked() { return unlocked; }
void lockRelock()   { unlocked = false; }

void lockClear() {
  netSet("lockSalt", "");
  netSet("lockHash", "");
  netSetU32("lockFails", 0);
  unlocked = false;
}
