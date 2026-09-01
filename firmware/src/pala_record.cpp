#include "pala_record.h"
#include "audio_bsp.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <math.h>
#include <SD_MMC.h>

#define REC_SAMPLE_RATE 16000
#define REC_MAX_SECONDS 120
#define PLAY_FRAMES 1024

#define REC_CAP ((size_t)REC_SAMPLE_RATE * REC_MAX_SECONDS)
#define REC_FRAMES 256            /* 256 stereo frames = 16 ms at 16 kHz */

static int16_t* monoBuf = nullptr;
static volatile size_t monoLen = 0;
static bool recording = false;
static volatile int lastLevel = 0;
static volatile bool recRun = false;
static TaskHandle_t recTaskH = nullptr;

static File playHandle;
static volatile bool playing = false;
/* Why playback stopped, shown on screen. Guessing at this from the outside
   cost an evening; the device can simply say. */
static const char* playFailed = nullptr;
static volatile bool playRun = false;
static TaskHandle_t playTaskH = nullptr;
static int16_t* stereoBuf = nullptr;
static int16_t* beepBuf = nullptr;
static void makeBeep();

void audioReady() {
  audio_bsp_init();
  audio_play_init();
  stereoBuf = (int16_t*)heap_caps_malloc(PLAY_FRAMES * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
  makeBeep();
}

static void makeBeep() {
  if (beepBuf) return;
  const int N = 960;
  beepBuf = (int16_t*)heap_caps_malloc(N * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
  if (!beepBuf) return;
  for (int i = 0; i < N; i++) {
    double env = exp(-3.0 * i / N);
    double s = 5000.0 * sin(2.0 * M_PI * 1000.0 * i / REC_SAMPLE_RATE) * env;
    int16_t v = (int16_t)s;
    beepBuf[2 * i] = v;
    beepBuf[2 * i + 1] = v;
  }
}

void beep() {
  if (!beepBuf) makeBeep();
  if (beepBuf && !recording && !playing) audio_playback_write(beepBuf, 480 * 2 * sizeof(int16_t));
}

/* Capture runs in its own task, and it has to. esp_codec_dev_read() only
   returns as fast as the microphone fills its DMA ring, while a partial
   e-paper refresh parks the Arduino loop inside read_busy() for a few hundred
   milliseconds at a time. Polled from loop(), the ring overflows during every
   meter update and the note comes back full of holes. Pinned to core 0 so the
   panel's SPI traffic on core 1 cannot stall it either. */
static void recTaskFn(void*) {
  int16_t stereo[REC_FRAMES * 2];
  while (recRun) {
    size_t at = monoLen;
    if (at + REC_FRAMES > REC_CAP) break;      /* full - stop before overrunning */
    audio_playback_read(stereo, sizeof(stereo));
    int64_t sumSq = 0;
    for (int i = 0; i < REC_FRAMES; i++) {
      int16_t m = (int16_t)(((int32_t)stereo[2 * i] + stereo[2 * i + 1]) >> 1);
      monoBuf[at + i] = m;
      sumSq += (int32_t)m * m;
    }
    monoLen = at + REC_FRAMES;                 /* publish only once written */
    int rms = (int)sqrt((double)(sumSq / REC_FRAMES));
    lastLevel = rms > 3000 ? 100 : rms / 30;
  }
  recRun = false;
  recTaskH = nullptr;
  vTaskDelete(NULL);
}

/* Ask the task to finish and wait for it, so nothing is still writing into
   monoBuf when the caller saves or frees it. */
static void recStopTask() {
  recRun = false;
  for (int i = 0; i < 300 && recTaskH; i++) delay(5);
}

bool recBegin() {
  if (playing) playStop();
  if (recRun || recTaskH) return false;
  monoBuf = (int16_t*)heap_caps_malloc(REC_CAP * sizeof(int16_t), MALLOC_CAP_SPIRAM);
  monoLen = 0;
  lastLevel = 0;
  if (!monoBuf) return false;
  recording = true;
  recRun = true;
  /* 6 KB: 1 KB of that is the stereo staging buffer, plus double maths */
  if (xTaskCreatePinnedToCore(recTaskFn, "palarec", 6144, NULL, 5, &recTaskH, 0) != pdPASS) {
    recRun = false;
    recording = false;
    heap_caps_free(monoBuf);
    monoBuf = nullptr;
    return false;
  }
  return true;
}

int recLevel() { return recording ? lastLevel : 0; }

uint32_t recSeconds() { return monoLen / REC_SAMPLE_RATE; }

bool recSave(const String& wavPath) {
  recStopTask();
  recording = false;
  if (!monoBuf || monoLen == 0) return false;
  /* Written through SD_MMC, not fopen(): the card is mounted at /sdcard, so a
     bare "/recordings/..." is a valid Arduino path but not a valid VFS one -
     fopen() on it fails and the note is lost. */
  File f = SD_MMC.open(wavPath, FILE_WRITE);
  if (!f) return false;

  const uint32_t dataBytes = (uint32_t)monoLen * sizeof(int16_t);
  uint8_t h[44];
  auto put32 = [&](int at, uint32_t v){ h[at]=v; h[at+1]=v>>8; h[at+2]=v>>16; h[at+3]=v>>24; };
  auto put16 = [&](int at, uint16_t v){ h[at]=v; h[at+1]=v>>8; };
  memcpy(h, "RIFF", 4);          put32(4, 36 + dataBytes);
  memcpy(h + 8, "WAVE", 4);      memcpy(h + 12, "fmt ", 4);
  put32(16, 16);                 put16(20, 1);                 /* PCM */
  put16(22, 1);                  put32(24, REC_SAMPLE_RATE);   /* mono */
  put32(28, REC_SAMPLE_RATE * 2);put16(32, 2);
  put16(34, 16);                 memcpy(h + 36, "data", 4);
  put32(40, dataBytes);
  if (f.write(h, sizeof(h)) != sizeof(h)) { f.close(); return false; }

  /* chunked so a 3.8 MB PSRAM buffer never goes to the driver in one call */
  const uint8_t* p = (const uint8_t*)monoBuf;
  uint32_t left = dataBytes;
  while (left) {
    size_t n = left > 8192 ? 8192 : left;
    if (f.write(p, n) != n) { f.close(); return false; }
    p += n; left -= n;
  }
  f.close();

  heap_caps_free(monoBuf);
  monoBuf = nullptr;
  monoLen = 0;
  return true;
}

void recDiscard() {
  recStopTask();
  recording = false;
  if (monoBuf) { heap_caps_free(monoBuf); monoBuf = nullptr; }
  monoLen = 0;
}

/* Playback streams from its own task, exactly as recording does.

   It used to be driven from the UI loop - one buffer per pass, with a delay(10)
   between them - which cannot keep up with real-time audio and stops dead for
   the ~2.7 seconds an e-paper refresh takes. Since pressing play redraws the
   screen immediately afterwards, the first thing playback did was stall for
   longer than most notes are worth listening to.

   A task writes continuously and the codec paces it: esp_codec_dev_write blocks
   until the I2S has room, so the loop needs no timing of its own. */
static void playTaskFn(void*) {
  /* Both buffers on the heap. mono[] as a local was 2KB of a 4KB task stack
     before any file I/O had a chance to use it, which is how the task came to
     start and vanish without ever producing sound. PSRAM if it can, internal
     RAM if it cannot - four kilobytes is not worth failing over. */
  int16_t* buf  = (int16_t*)heap_caps_malloc(PLAY_FRAMES * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
  if (!buf)  buf  = (int16_t*)malloc(PLAY_FRAMES * 2 * sizeof(int16_t));
  int16_t* mono = (int16_t*)heap_caps_malloc(PLAY_FRAMES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
  if (!mono) mono = (int16_t*)malloc(PLAY_FRAMES * sizeof(int16_t));
  if (!buf || !mono) playFailed = "no buffer";
  bool first = true;
  while (playRun && buf && mono) {
    int got = playHandle.read((uint8_t*)mono, PLAY_FRAMES * sizeof(int16_t)) / sizeof(int16_t);
    if (got <= 0) { if (first) playFailed = "read 0 bytes"; break; }
    first = false;
    for (int i = 0; i < got; i++) {
      buf[2 * i]     = mono[i];
      buf[2 * i + 1] = mono[i];
    }
    int werr = audio_playback_write(buf, got * 2 * sizeof(int16_t));
    if (werr != 0 && !playFailed) {
      static char msg[24];
      snprintf(msg, sizeof(msg), "wr %d", werr);
      playFailed = msg;
      break;
    }
  }
  if (buf)  free(buf);
  if (mono) free(mono);
  if (playHandle) playHandle.close();
  playing  = false;
  playRun  = false;
  playTaskH = nullptr;
  vTaskDelete(NULL);
}

bool playFile(const String& wavPath) {
  if (recording) return false;
  if (playing) playStop();
  playHandle = SD_MMC.open(wavPath, "r");
  if (!playHandle) return false;
  playHandle.seek(44);                         /* past the RIFF header */
  playFailed = nullptr;
  playing = true;
  playRun = true;
  if (xTaskCreatePinnedToCore(playTaskFn, "play", 8192, nullptr, 4, &playTaskH, 1) != pdPASS) {
    playFailed = "no task";
    playHandle.close();
    playing = false;
    playRun = false;
    return false;
  }
  return true;
}

/* Kept so the UI can call it every pass; the task does the work now. */
void playPoll() {}

void playStop() {
  playRun = false;
  for (int i = 0; i < 200 && playTaskH; i++) delay(5);   /* let it wind down */
  playing = false;
  if (playHandle) playHandle.close();
}

bool playActive() { return playing; }
const char* playError() { return playFailed; }
