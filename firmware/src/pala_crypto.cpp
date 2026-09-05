#include "pala_crypto.h"
#include "pala_net.h"
#include <SD_MMC.h>
#include <mbedtls/ecp.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/gcm.h>
#include <mbedtls/hkdf.h>
#include <mbedtls/pkcs5.h>
#include <mbedtls/md.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <esp_random.h>

/* secp256r1: present in every mbedTLS build, hardware-accelerated on this
   chip, and entirely adequate here. */
#define CURVE MBEDTLS_ECP_DP_SECP256R1

/* Enough iterations to cost roughly a second on this processor. The PIN is
   short, so the only defence against guessing is making each guess expensive. */
#define PBKDF2_ITERS 120000

#define DEFAULT_PIN "1234"

static mbedtls_ecp_group grp;
static bool grpReady = false;

static mbedtls_mpi  privKey;              /* only valid while unlocked */
static mbedtls_ecp_point pubKey;
static bool unlocked = false;
static bool haveKey  = false;

static bool ensureGroup() {
  if (grpReady) return true;
  mbedtls_ecp_group_init(&grp);
  if (mbedtls_ecp_group_load(&grp, CURVE) != 0) return false;
  grpReady = true;
  return true;
}

static void fillRandom(uint8_t* out, size_t n) {
  esp_fill_random(out, n);                /* hardware RNG */
}

static int rngWrap(void*, unsigned char* out, size_t n) {
  esp_fill_random(out, n);
  return 0;
}

/* PIN plus a stored random salt into a private scalar. The salt is not secret;
   it exists so two devices with the same PIN do not share a key. */
static bool deriveScalar(const String& pin, mbedtls_mpi* out) {
  String saltHex = netGet("cryptoSalt");
  if (saltHex.length() != 32) return false;
  uint8_t salt[16];
  for (int i = 0; i < 16; i++)
    salt[i] = (uint8_t)strtoul(saltHex.substring(i * 2, i * 2 + 2).c_str(), nullptr, 16);

  uint8_t key[32];
  int rc = mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256,
                                         (const unsigned char*)pin.c_str(), pin.length(),
                                         salt, sizeof(salt), PBKDF2_ITERS,
                                         sizeof(key), key);
  if (rc != 0) return false;

  if (mbedtls_mpi_read_binary(out, key, sizeof(key)) != 0) { memset(key, 0, 32); return false; }
  memset(key, 0, sizeof(key));
  /* Fold into the curve order, and never allow zero. */
  if (mbedtls_mpi_mod_mpi(out, out, &grp.N) != 0) return false;
  if (mbedtls_mpi_cmp_int(out, 0) == 0) return false;
  return true;
}

static String pointToHex(const mbedtls_ecp_point* p) {
  uint8_t buf[CRYPTO_EPK_N];
  size_t olen = 0;
  if (mbedtls_ecp_point_write_binary(&grp, p, MBEDTLS_ECP_PF_UNCOMPRESSED,
                                     &olen, buf, sizeof(buf)) != 0) return "";
  String s;
  for (size_t i = 0; i < olen; i++) {
    char h[3]; snprintf(h, sizeof(h), "%02x", buf[i]); s += h;
  }
  return s;
}

static bool pointFromHex(const String& hex, mbedtls_ecp_point* p) {
  if (hex.length() != CRYPTO_EPK_N * 2) return false;
  uint8_t buf[CRYPTO_EPK_N];
  for (int i = 0; i < CRYPTO_EPK_N; i++)
    buf[i] = (uint8_t)strtoul(hex.substring(i * 2, i * 2 + 2).c_str(), nullptr, 16);
  return mbedtls_ecp_point_read_binary(&grp, p, buf, sizeof(buf)) == 0;
}

bool cryptoBegin() {
  if (!ensureGroup()) return false;
  mbedtls_mpi_init(&privKey);
  mbedtls_ecp_point_init(&pubKey);

  if (netGet("cryptoSalt").length() != 32) {
    uint8_t salt[16];
    fillRandom(salt, sizeof(salt));
    String s;
    for (int i = 0; i < 16; i++) { char h[3]; snprintf(h, sizeof(h), "%02x", salt[i]); s += h; }
    netSet("cryptoSalt", s);
  }

  String pubHex = netGet("cryptoPub");
  if (pubHex.length() == CRYPTO_EPK_N * 2 && pointFromHex(pubHex, &pubKey)) {
    haveKey = true;
    return true;
  }
  /* No keypair yet. Deliberately not generated here: this is called from a
     screen the user opened, but it was once called from setup(), where the
     work is invisible and indistinguishable from a crash. The caller decides
     when to pay for it, with something on screen saying so. */
  return false;
}

bool cryptoHasKey() { return haveKey; }

bool cryptoPinIsDefault() { return netGet("cryptoDefault", "1") == "1"; }

bool cryptoSetPin(const String& pin) {
  if (!ensureGroup() || pin.length() < 4) return false;
  mbedtls_mpi d; mbedtls_mpi_init(&d);
  mbedtls_ecp_point Q; mbedtls_ecp_point_init(&Q);
  bool ok = false;

  if (deriveScalar(pin, &d) &&
      mbedtls_ecp_mul(&grp, &Q, &d, &grp.G, rngWrap, nullptr) == 0) {
    String hex = pointToHex(&Q);
    if (hex.length() == CRYPTO_EPK_N * 2) {
      netSet("cryptoPub", hex);
      netSet("cryptoDefault", pin == DEFAULT_PIN ? "1" : "0");
      mbedtls_ecp_copy(&pubKey, &Q);
      mbedtls_mpi_copy(&privKey, &d);
      unlocked = true;                    /* setting it implies knowing it */
      haveKey = true;
      ok = true;
    }
  }
  mbedtls_mpi_free(&d);
  mbedtls_ecp_point_free(&Q);
  return ok;
}

bool cryptoUnlock(const String& pin) {
  if (!ensureGroup() || !haveKey) return false;
  mbedtls_mpi d; mbedtls_mpi_init(&d);
  mbedtls_ecp_point Q; mbedtls_ecp_point_init(&Q);
  bool ok = false;

  if (deriveScalar(pin, &d) &&
      mbedtls_ecp_mul(&grp, &Q, &d, &grp.G, rngWrap, nullptr) == 0) {
    /* The derived key is right only if it reproduces the stored public key.
       Checking here means a wrong PIN is refused rather than quietly
       decrypting every note into noise. */
    if (mbedtls_ecp_point_cmp(&Q, &pubKey) == 0) {
      mbedtls_mpi_copy(&privKey, &d);
      unlocked = true;
      ok = true;
    }
  }
  mbedtls_mpi_free(&d);
  mbedtls_ecp_point_free(&Q);
  return ok;
}

void cryptoLock() {
  mbedtls_mpi_free(&privKey);
  mbedtls_mpi_init(&privKey);
  unlocked = false;
}

bool cryptoUnlocked() { return unlocked; }

/* Shared secret to an AES key. HKDF rather than using the raw x-coordinate,
   which is not uniformly distributed and should never be a key directly. */
static bool sharedToKey(const mbedtls_mpi* d, const mbedtls_ecp_point* peer, uint8_t key[32]) {
  mbedtls_mpi z; mbedtls_mpi_init(&z);
  bool ok = false;
  if (mbedtls_ecdh_compute_shared(&grp, &z, peer, d, rngWrap, nullptr) == 0) {
    uint8_t zb[32];
    if (mbedtls_mpi_write_binary(&z, zb, sizeof(zb)) == 0) {
      const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
      ok = mbedtls_hkdf(md, nullptr, 0, zb, sizeof(zb),
                        (const unsigned char*)"mono-note-mini", 14, key, 32) == 0;
      memset(zb, 0, sizeof(zb));
    }
  }
  mbedtls_mpi_free(&z);
  return ok;
}

bool cryptoIsEncrypted(const String& path) {
  File f = SD_MMC.open(path, "r");
  if (!f) return false;
  char m[CRYPTO_MAGIC_N + 1] = {0};
  int n = f.read((uint8_t*)m, CRYPTO_MAGIC_N);
  f.close();
  return n == CRYPTO_MAGIC_N && memcmp(m, CRYPTO_MAGIC, CRYPTO_MAGIC_N) == 0;
}

bool cryptoEncryptFile(const String& plainPath, const String& outPath) {
  if (!haveKey || !ensureGroup()) return false;
  File in = SD_MMC.open(plainPath, "r");
  if (!in) return false;
  File out = SD_MMC.open(outPath, "w");
  if (!out) { in.close(); return false; }

  bool ok = false;
  mbedtls_mpi e; mbedtls_mpi_init(&e);
  mbedtls_ecp_point E; mbedtls_ecp_point_init(&E);
  mbedtls_gcm_context gcm; mbedtls_gcm_init(&gcm);
  uint8_t key[32], iv[CRYPTO_IV_N], tag[CRYPTO_TAG_N];

  /* A fresh ephemeral key per file, so two notes never share a key even
     though they share a recipient. */
  if (mbedtls_ecp_gen_keypair(&grp, &e, &E, rngWrap, nullptr) == 0 &&
      sharedToKey(&e, &pubKey, key)) {
    fillRandom(iv, sizeof(iv));
    uint8_t epk[CRYPTO_EPK_N]; size_t elen = 0;
    if (mbedtls_ecp_point_write_binary(&grp, &E, MBEDTLS_ECP_PF_UNCOMPRESSED,
                                       &elen, epk, sizeof(epk)) == 0 &&
        elen == CRYPTO_EPK_N &&
        mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256) == 0 &&
        mbedtls_gcm_starts(&gcm, MBEDTLS_GCM_ENCRYPT, iv, sizeof(iv)) == 0) {

      out.write((const uint8_t*)CRYPTO_MAGIC, CRYPTO_MAGIC_N);
      out.write(epk, CRYPTO_EPK_N);
      out.write(iv, sizeof(iv));

      uint8_t bufIn[512], bufOut[512];
      size_t olen = 0;
      ok = true;
      while (true) {
        int n = in.read(bufIn, sizeof(bufIn));
        if (n <= 0) break;
        if (mbedtls_gcm_update(&gcm, bufIn, n, bufOut, sizeof(bufOut), &olen) != 0) { ok = false; break; }
        if (olen && out.write(bufOut, olen) != olen) { ok = false; break; }
      }
      if (ok) {
        if (mbedtls_gcm_finish(&gcm, bufOut, sizeof(bufOut), &olen, tag, sizeof(tag)) != 0) ok = false;
        else {
          if (olen) out.write(bufOut, olen);
          /* The tag goes last: it covers everything and is only known at the
             end, which is what streaming costs. */
          out.write(tag, sizeof(tag));
        }
      }
    }
  }

  memset(key, 0, sizeof(key));
  mbedtls_gcm_free(&gcm);
  mbedtls_mpi_free(&e);
  mbedtls_ecp_point_free(&E);
  in.close();
  out.close();
  if (!ok) SD_MMC.remove(outPath);        /* never leave a half-written file */
  return ok;
}

bool cryptoDecryptFile(const String& encPath, const String& outPath) {
  if (!unlocked || !ensureGroup()) return false;
  File in = SD_MMC.open(encPath, "r");
  if (!in) return false;
  const size_t total = in.size();
  if (total < CRYPTO_HDR_N + CRYPTO_TAG_N) { in.close(); return false; }

  char magic[CRYPTO_MAGIC_N];
  uint8_t epk[CRYPTO_EPK_N], iv[CRYPTO_IV_N];
  in.read((uint8_t*)magic, CRYPTO_MAGIC_N);
  in.read(epk, CRYPTO_EPK_N);
  in.read(iv, CRYPTO_IV_N);
  if (memcmp(magic, CRYPTO_MAGIC, CRYPTO_MAGIC_N) != 0) { in.close(); return false; }

  File out = SD_MMC.open(outPath, "w");
  if (!out) { in.close(); return false; }

  bool ok = false;
  mbedtls_ecp_point E; mbedtls_ecp_point_init(&E);
  mbedtls_gcm_context gcm; mbedtls_gcm_init(&gcm);
  uint8_t key[32], tag[CRYPTO_TAG_N], want[CRYPTO_TAG_N];

  size_t body = total - CRYPTO_HDR_N - CRYPTO_TAG_N;
  if (mbedtls_ecp_point_read_binary(&grp, &E, epk, sizeof(epk)) == 0 &&
      sharedToKey(&privKey, &E, key) &&
      mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256) == 0 &&
      mbedtls_gcm_starts(&gcm, MBEDTLS_GCM_DECRYPT, iv, sizeof(iv)) == 0) {

    uint8_t bufIn[512], bufOut[512];
    size_t olen = 0, done = 0;
    ok = true;
    while (done < body) {
      size_t want_n = body - done < sizeof(bufIn) ? body - done : sizeof(bufIn);
      int n = in.read(bufIn, want_n);
      if (n <= 0) { ok = false; break; }
      if (mbedtls_gcm_update(&gcm, bufIn, n, bufOut, sizeof(bufOut), &olen) != 0) { ok = false; break; }
      if (olen && out.write(bufOut, olen) != olen) { ok = false; break; }
      done += n;
    }
    if (ok) {
      if (in.read(want, sizeof(want)) != sizeof(want)) ok = false;
      else if (mbedtls_gcm_finish(&gcm, bufOut, sizeof(bufOut), &olen, tag, sizeof(tag)) != 0) ok = false;
      else {
        if (olen) out.write(bufOut, olen);
        /* Constant-time compare: a tag check that leaks timing is not a check. */
        uint8_t diff = 0;
        for (size_t i = 0; i < sizeof(tag); i++) diff |= (uint8_t)(tag[i] ^ want[i]);
        ok = (diff == 0);
      }
    }
  }

  memset(key, 0, sizeof(key));
  mbedtls_gcm_free(&gcm);
  mbedtls_ecp_point_free(&E);
  in.close();
  out.close();
  /* A failed tag means altered or corrupt data. Deleting it is the point of
     using GCM: the alternative is handing back plausible-looking noise. */
  if (!ok) SD_MMC.remove(outPath);
  return ok;
}

String cryptoSelfTest() {
  if (!ensureGroup()) return "no curve";

  const char* sample = "the quick brown fox jumps over the lazy dog, twice over, "
                       "with enough text to span more than one buffer of five hundred "
                       "and twelve bytes so the streaming path is genuinely exercised "
                       "rather than only the single-block case that always works. ";
  String big;
  while (big.length() < 2000) big += sample;

  const String p = "/enc_test.tmp", c = "/enc_test.enc", d = "/enc_test.dec";
  SD_MMC.remove(p); SD_MMC.remove(c); SD_MMC.remove(d);
  File f = SD_MMC.open(p, "w");
  if (!f) return "no card";
  f.print(big); f.close();

  String result;
  if (!cryptoEncryptFile(p, c))              result = "encrypt failed";
  else if (!cryptoIsEncrypted(c))            result = "no header";
  else if (!unlocked)                        result = "locked";
  else if (!cryptoDecryptFile(c, d))         result = "decrypt failed";
  else {
    File g = SD_MMC.open(d, "r");
    String back = g ? g.readString() : "";
    if (g) g.close();
    if (back != big)                         result = "round trip differs";
    else {
      /* Tamper with one byte of ciphertext: GCM must refuse it. Encryption
         that cannot detect alteration is not much better than none. */
      File t = SD_MMC.open(c, "r+");
      bool caught = false;
      if (t) {
        t.seek(CRYPTO_HDR_N + 4);
        uint8_t b; t.read(&b, 1);
        t.seek(CRYPTO_HDR_N + 4);
        b ^= 0x40; t.write(&b, 1);
        t.close();
        SD_MMC.remove(d);
        caught = !cryptoDecryptFile(c, d);   /* must fail */
      }
      result = caught ? "ok" : "tamper undetected";
    }
  }
  SD_MMC.remove(p); SD_MMC.remove(c); SD_MMC.remove(d);
  return result;
}
