#ifndef PALA_VOICE_H
#define PALA_VOICE_H
#include <Arduino.h>

/* Saying a passphrase to open your notes, instead of tapping out a PIN on two
   buttons.

   This is text-dependent verification: it checks that the same phrase was said
   in roughly the same voice, by comparing mel-cepstral features with dynamic
   time warping. That is a real lock against someone who picks the device up.
   It is not biometric-grade, and a recording of you saying the phrase would
   pass it.

   Which is why it does not hold the encryption key. The key still comes from
   the PIN, derived fresh each time, so notes on a stolen card stay unreadable
   whatever this does. Voice is the convenience on top; the PIN is the floor.

   Templates live on the card in the clear. They cannot reconstruct your voice
   - they are averaged cepstra, not audio - but treat them as "something the
   device knows", never as a secret. */

#define VOICE_SECONDS   3
#define VOICE_ENROLS    3          /* say it three times, keep all three */

bool  voiceEnabled();
void  voiceSetEnabled(bool on);
bool  voiceHasTemplates();
void  voiceForget();

/* Records VOICE_SECONDS and stores it as enrolment sample `slot` (0..2).
   Returns false if the microphone gave nothing or the phrase was all silence. */
bool  voiceEnrol(int slot);

/* Records and compares against every enrolled sample. `outScore` is the best
   distance found - smaller is more alike. */
bool  voiceVerify(float* outScore);

/* The distance below which a phrase is accepted. Stored, so it can be tuned
   against a real voice rather than guessed at here. */
float voiceThreshold();
void  voiceSetThreshold(float t);

#endif
