#include "pala_voice.h"
#include "pala_net.h"
#include "audio_bsp.h"
#include "esp_heap_caps.h"
#include <SD_MMC.h>
#include <math.h>

/* ---- shape of the analysis ---------------------------------------------
   16 kHz mono, 25 ms frames every 10 ms, which is the standard speech frame
   and what every mel-cepstrum recipe assumes. A 512-point FFT covers a 400
   sample frame with room to pad. */
#define SR          16000
#define FRAME_N     400            /* 25 ms */
#define HOP_N       160            /* 10 ms */
#define FFT_N       512
#define MEL_BANDS   20
#define CEPS_N      13
#define MAX_FRAMES  ((VOICE_SECONDS * SR) / HOP_N + 4)

static const char* TPL_PATH = "/voice";      /* /voice0.tpl .. /voice2.tpl */

/* ---- a 512-point radix-2 FFT -------------------------------------------
   Written out rather than pulled in: the only transform needed here is one
   fixed size, and a dependency for that is not worth carrying. */
static void fft512(float* re, float* im) {
  for (int i = 1, j = 0; i < FFT_N; i++) {           /* bit reversal */
    int bit = FFT_N >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) {
      float t = re[i]; re[i] = re[j]; re[j] = t;
      t = im[i]; im[i] = im[j]; im[j] = t;
    }
  }
  for (int len = 2; len <= FFT_N; len <<= 1) {
    float ang = -2.0f * (float)M_PI / (float)len;
    float wr = cosf(ang), wi = sinf(ang);
    for (int i = 0; i < FFT_N; i += len) {
      float cr = 1.0f, ci = 0.0f;
      for (int k = 0; k < len / 2; k++) {
        float ur = re[i + k], ui = im[i + k];
        float vr = re[i + k + len / 2] * cr - im[i + k + len / 2] * ci;
        float vi = re[i + k + len / 2] * ci + im[i + k + len / 2] * cr;
        re[i + k] = ur + vr;           im[i + k] = ui + vi;
        re[i + k + len / 2] = ur - vr; im[i + k + len / 2] = ui - vi;
        float ncr = cr * wr - ci * wi;
        ci = cr * wi + ci * wr;
        cr = ncr;
      }
    }
  }
}

static float hzToMel(float hz) { return 2595.0f * log10f(1.0f + hz / 700.0f); }
static float melToHz(float m)  { return 700.0f * (powf(10.0f, m / 2595.0f) - 1.0f); }

/* Mel filter edges, worked out once. 80 Hz to 7600 Hz: below 80 is mains hum
   and handling noise, above 7600 is nothing a 16 kHz capture can represent. */
static int melEdge[MEL_BANDS + 2];
static bool melReady = false;
static void buildMel() {
  if (melReady) return;
  float lo = hzToMel(80.0f), hi = hzToMel(7600.0f);
  for (int i = 0; i < MEL_BANDS + 2; i++) {
    float m = lo + (hi - lo) * i / (MEL_BANDS + 1);
    melEdge[i] = (int)floorf((FFT_N + 1) * melToHz(m) / SR);
    if (melEdge[i] > FFT_N / 2) melEdge[i] = FFT_N / 2;
  }
  melReady = true;
}

/* ---- audio to cepstra ---------------------------------------------------
   Returns the number of frames written into `out`, which must hold
   MAX_FRAMES * CEPS_N floats. */
static int mfcc(const int16_t* pcm, size_t n, float* out) {
  buildMel();
  float* re = (float*)heap_caps_malloc(FFT_N * sizeof(float), MALLOC_CAP_SPIRAM);
  float* im = (float*)heap_caps_malloc(FFT_N * sizeof(float), MALLOC_CAP_SPIRAM);
  if (!re || !im) { heap_caps_free(re); heap_caps_free(im); return 0; }

  int frames = 0;
  for (size_t start = 0; start + FRAME_N <= n && frames < MAX_FRAMES; start += HOP_N) {
    float prev = 0.0f;
    for (int i = 0; i < FFT_N; i++) {
      if (i < FRAME_N) {
        float s = (float)pcm[start + i];
        float pre = s - 0.97f * prev;        /* pre-emphasis lifts the highs */
        prev = s;
        float w = 0.54f - 0.46f * cosf(2.0f * (float)M_PI * i / (FRAME_N - 1));
        re[i] = pre * w;
      } else {
        re[i] = 0.0f;
      }
      im[i] = 0.0f;
    }
    fft512(re, im);

    float energy[MEL_BANDS];
    for (int b = 0; b < MEL_BANDS; b++) {
      float sum = 0.0f;
      int peak = melEdge[b + 1];
      for (int k = melEdge[b]; k < melEdge[b + 2]; k++) {
        float mag = re[k] * re[k] + im[k] * im[k];
        float w;
        if (k <= peak) w = (peak == melEdge[b]) ? 1.0f
                         : (float)(k - melEdge[b]) / (float)(peak - melEdge[b]);
        else           w = (melEdge[b + 2] == peak) ? 1.0f
                         : (float)(melEdge[b + 2] - k) / (float)(melEdge[b + 2] - peak);
        sum += mag * w;
      }
      energy[b] = logf(sum + 1e-10f);
    }
    /* DCT-II down to CEPS_N coefficients. */
    for (int c = 0; c < CEPS_N; c++) {
      float acc = 0.0f;
      for (int b = 0; b < MEL_BANDS; b++)
        acc += energy[b] * cosf((float)M_PI * c * (b + 0.5f) / MEL_BANDS);
      out[frames * CEPS_N + c] = acc;
    }
    frames++;
  }
  heap_caps_free(re);
  heap_caps_free(im);

  /* Cepstral mean normalisation. Subtracting the average over the phrase takes
     out whatever was constant through it - the microphone, the room, how close
     it was held - which is most of what otherwise stops someone matching
     themselves on a different day. */
  if (frames > 0) {
    for (int c = 0; c < CEPS_N; c++) {
      float mean = 0.0f;
      for (int f = 0; f < frames; f++) mean += out[f * CEPS_N + c];
      mean /= (float)frames;
      for (int f = 0; f < frames; f++) out[f * CEPS_N + c] -= mean;
    }
  }
  return frames;
}

/* Trim silence from both ends by frame energy, so a phrase said after a pause
   compares as the same phrase rather than as a longer one. */
static void trimSilence(const int16_t* pcm, size_t n, size_t* from, size_t* to) {
  const size_t win = HOP_N;
  const size_t blocks = n / win;
  float peak = 0.0f;
  for (size_t b = 0; b < blocks; b++) {
    float sum = 0.0f;
    for (size_t i = 0; i < win; i++) { float s = pcm[b * win + i]; sum += s * s; }
    float rms = sqrtf(sum / win);
    if (rms > peak) peak = rms;
  }
  const float gate = peak * 0.10f;
  size_t a = 0, z = blocks ? blocks - 1 : 0;
  while (a < blocks) {
    float sum = 0.0f;
    for (size_t i = 0; i < win; i++) { float s = pcm[a * win + i]; sum += s * s; }
    if (sqrtf(sum / win) > gate) break;
    a++;
  }
  while (z > a) {
    float sum = 0.0f;
    for (size_t i = 0; i < win; i++) { float s = pcm[z * win + i]; sum += s * s; }
    if (sqrtf(sum / win) > gate) break;
    z--;
  }
  *from = a * win;
  size_t end = (z + 1) * win;
  *to = end < n ? end : n;
}

/* ---- dynamic time warping ----------------------------------------------
   People say the same words at different speeds. DTW finds the cheapest
   alignment between two sequences instead of demanding they line up frame for
   frame, and the cost of that alignment is the distance. Normalised by path
   length, so a longer phrase is not automatically a worse match.

   Two rows rather than the whole matrix: 300 frames square in floats is
   360 KB, and only the previous row is ever read. */
static float dtwDistance(const float* a, int na, const float* b, int nb) {
  if (na <= 0 || nb <= 0) return 1e9f;
  float* prev = (float*)heap_caps_malloc((nb + 1) * sizeof(float), MALLOC_CAP_SPIRAM);
  float* cur  = (float*)heap_caps_malloc((nb + 1) * sizeof(float), MALLOC_CAP_SPIRAM);
  if (!prev || !cur) { heap_caps_free(prev); heap_caps_free(cur); return 1e9f; }

  for (int j = 0; j <= nb; j++) prev[j] = 1e9f;
  prev[0] = 0.0f;
  for (int i = 1; i <= na; i++) {
    cur[0] = 1e9f;
    for (int j = 1; j <= nb; j++) {
      float d = 0.0f;
      for (int c = 0; c < CEPS_N; c++) {
        float diff = a[(i - 1) * CEPS_N + c] - b[(j - 1) * CEPS_N + c];
        d += diff * diff;
      }
      d = sqrtf(d);
      float best = prev[j - 1];
      if (prev[j]   < best) best = prev[j];
      if (cur[j - 1] < best) best = cur[j - 1];
      cur[j] = d + best;
    }
    float* t = prev; prev = cur; cur = t;
  }
  float total = prev[nb];
  heap_caps_free(prev);
  heap_caps_free(cur);
  return total / (float)(na + nb);
}

/* ---- capture ------------------------------------------------------------
   Read straight from the codec rather than going through the recorder: this
   wants a fixed few seconds into memory, not a file on the card, and nothing
   should be drawn on screen while it runs. */
static int captureCepstra(float* out) {
  const size_t want = (size_t)VOICE_SECONDS * SR;
  int16_t* pcm = (int16_t*)heap_caps_malloc(want * sizeof(int16_t), MALLOC_CAP_SPIRAM);
  if (!pcm) return 0;

  size_t got = 0;
  int16_t stereo[256 * 2];
  while (got + 256 <= want) {
    audio_playback_read(stereo, sizeof(stereo));
    for (int i = 0; i < 256; i++)
      pcm[got + i] = (int16_t)(((int32_t)stereo[2 * i] + stereo[2 * i + 1]) >> 1);
    got += 256;
  }

  size_t from = 0, to = got;
  trimSilence(pcm, got, &from, &to);
  int frames = (to - from) >= FRAME_N ? mfcc(pcm + from, to - from, out) : 0;
  heap_caps_free(pcm);
  return frames;
}

/* ---- templates on the card --------------------------------------------- */
static String slotPath(int slot) { return String(TPL_PATH) + String(slot) + ".tpl"; }

static bool saveTemplate(int slot, const float* c, int frames) {
  File f = SD_MMC.open(slotPath(slot), "w");
  if (!f) return false;
  f.write((const uint8_t*)&frames, sizeof(frames));
  f.write((const uint8_t*)c, (size_t)frames * CEPS_N * sizeof(float));
  f.flush();                      /* the tail of a file has gone missing here before */
  f.close();
  return true;
}

static int loadTemplate(int slot, float* c) {
  File f = SD_MMC.open(slotPath(slot), "r");
  if (!f) return 0;
  int frames = 0;
  if (f.read((uint8_t*)&frames, sizeof(frames)) != (int)sizeof(frames) ||
      frames <= 0 || frames > MAX_FRAMES) { f.close(); return 0; }
  size_t want = (size_t)frames * CEPS_N * sizeof(float);
  int got = f.read((uint8_t*)c, want);
  f.close();
  return ((size_t)got == want) ? frames : 0;
}

bool  voiceEnabled()             { return netGet("voiceOn", "0") == "1"; }
void  voiceSetEnabled(bool on)   { netSet("voiceOn", on ? "1" : "0"); }

/* Tenths, because preferences store integers. The default is a starting point
   to tune from, not a measured figure - it wants calibrating against a real
   voice, which is what the score on the test screen is for. */
float voiceThreshold()           { return netGetU32("voiceThr", 55) / 10.0f; }
void  voiceSetThreshold(float t) { netSetU32("voiceThr", (uint32_t)(t * 10.0f + 0.5f)); }

bool voiceHasTemplates() {
  for (int i = 0; i < VOICE_ENROLS; i++)
    if (SD_MMC.exists(slotPath(i))) return true;
  return false;
}

void voiceForget() {
  for (int i = 0; i < VOICE_ENROLS; i++) SD_MMC.remove(slotPath(i));
  voiceSetEnabled(false);
}

bool voiceEnrol(int slot) {
  if (slot < 0 || slot >= VOICE_ENROLS) return false;
  float* c = (float*)heap_caps_malloc((size_t)MAX_FRAMES * CEPS_N * sizeof(float), MALLOC_CAP_SPIRAM);
  if (!c) return false;
  int frames = captureCepstra(c);
  /* Fewer than ten frames after trimming is a tenth of a second of sound: the
     phrase was not said, or the microphone gave nothing. */
  bool ok = frames >= 10 && saveTemplate(slot, c, frames);
  heap_caps_free(c);
  return ok;
}

bool voiceVerify(float* outScore) {
  if (outScore) *outScore = 1e9f;
  float* said = (float*)heap_caps_malloc((size_t)MAX_FRAMES * CEPS_N * sizeof(float), MALLOC_CAP_SPIRAM);
  float* tpl  = (float*)heap_caps_malloc((size_t)MAX_FRAMES * CEPS_N * sizeof(float), MALLOC_CAP_SPIRAM);
  if (!said || !tpl) { heap_caps_free(said); heap_caps_free(tpl); return false; }

  int n = captureCepstra(said);
  float best = 1e9f;
  if (n >= 10) {
    for (int i = 0; i < VOICE_ENROLS; i++) {
      int m = loadTemplate(i, tpl);
      if (m < 10) continue;
      float d = dtwDistance(said, n, tpl, m);
      if (d < best) best = d;
    }
  }
  heap_caps_free(said);
  heap_caps_free(tpl);
  if (outScore) *outScore = best;
  return best <= voiceThreshold();
}
