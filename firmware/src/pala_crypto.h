#ifndef PALA_CRYPTO_H
#define PALA_CRYPTO_H
#include <Arduino.h>

/* Notes encrypted on the card, with the key derived from a PIN.

   The awkward requirement is that recording must work while locked - catching
   a thought is the entire point of the device and cannot wait for a PIN - but
   the key must not exist on the device when it is stolen. Those two are
   impossible with ordinary symmetric encryption: something has to hold a
   secret to encrypt with.

   Public-key crypto resolves it exactly. The device stores a public key in the
   clear, which is useless to a thief, and encrypts every new note with it. The
   matching private key is derived from the PIN and exists only in RAM, only
   while browsing. A stolen device keeps recording into a vault it cannot open.

   Per file: a fresh ephemeral keypair, ECDH against the device public key, and
   HKDF to a 256-bit AES-GCM key. GCM rather than plain AES so tampering is
   detected instead of quietly producing noise. The ephemeral public key rides
   in the file header; nothing secret is stored anywhere.

   Honest limit: because the public key is stored, someone who dumps the flash
   can try PINs offline against it. PBKDF2 is deliberately slow to make that
   expensive, but a short PIN is still a short PIN. This defeats a thief who
   finds a gadget. It does not defeat a laboratory. */

/* File header, written before the ciphertext. */
#define CRYPTO_MAGIC   "MNM1"
#define CRYPTO_MAGIC_N 4
#define CRYPTO_EPK_N   65      /* uncompressed secp256r1 point */
#define CRYPTO_IV_N    12
#define CRYPTO_TAG_N   16
#define CRYPTO_HDR_N   (CRYPTO_MAGIC_N + CRYPTO_EPK_N + CRYPTO_IV_N)

bool cryptoBegin();                       /* load or create the device keypair */
bool cryptoHasKey();                      /* a public key exists to encrypt to */

/* Sets the PIN for the first time, or changes it. Changing re-derives the
   keypair, so everything already on the card must be re-encrypted - the caller
   does that, and must have unlocked with the old PIN first. */
bool cryptoSetPin(const String& pin);
bool cryptoPinIsDefault();

/* Derives the private key and checks it against the stored public key. Wrong
   PIN fails here rather than producing garbage later. */
bool cryptoUnlock(const String& pin);
void cryptoLock();                        /* wipes the private key from RAM */
bool cryptoUnlocked();

/* Whole-file operations. Both stream, so a four megabyte recording never has
   to fit in memory. Decrypt needs the device unlocked; encrypt never does. */
bool cryptoEncryptFile(const String& plainPath, const String& outPath);
bool cryptoDecryptFile(const String& encPath, const String& outPath);

/* True when the file carries our header. Anything else is treated as plain,
   so notes made before encryption was turned on still open. */
bool cryptoIsEncrypted(const String& path);

/* Round-trip check against the real primitives, run before any real note is
   encrypted. Returns a short human-readable result. */
String cryptoSelfTest();

#endif
