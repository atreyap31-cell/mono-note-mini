#include "pala_record.h"
#include "audio_bsp.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <math.h>
#include <SD_MMC.h>

#define REC_SAMPLE_RATE 16000
#define REC_MAX_SECONDS 120
#define PLAY_FRAMES 1024

static int16_t* monoBuf = nullptr;
static size_t monoLen = 0;
static bool recording = false;
static int lastLevel = 0;

static File playHandle;
static bool playing = false;
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

bool recBegin() {
  if (playing) playStop();
  monoBuf = (int16_t*)heap_caps_malloc(REC_SAMPLE_RATE * sizeof(int16_t) * REC_MAX_SECONDS, MALLOC_CAP_SPIRAM);
  monoLen = 0;
  recording = (monoBuf != nullptr);
  return recording;
}

void recPoll() {
  if (!recording || !monoBuf) return;
  if (monoLen >= REC_SAMPLE_RATE * REC_MAX_SECONDS) return;
  int16_t stereo[512];
  audio_playback_read(stereo, sizeof(stereo));
  const int frames = sizeof(stereo) / (2 * sizeof(int16_t));
  int64_t sumSq = 0;
  for (int i = 0; i < frames; i++) {
    int16_t m = (int16_t)(((int32_t)stereo[2 * i] + stereo[2 * i + 1]) >> 1);
    monoBuf[monoLen++] = m;
    sumSq += (int32_t)m * m;
  }
  int rms = (int)sqrt((double)(sumSq / frames));
  lastLevel = rms > 3000 ? 100 : rms / 30;
}

int recLevel() { return recording ? lastLevel : 0; }

uint32_t recSeconds() { return monoLen / REC_SAMPLE_RATE; }

bool recSave(const String& wavPath) {
  if (!monoBuf || monoLen == 0) return false;
  recording = false;
  FILE* f = fopen(wavPath.c_str(), "wb");
  if (!f) return false;
  uint32_t dataBytes = monoLen * sizeof(int16_t);
  fwrite("RIFF", 1, 4, f);
  uint32_t v32 = 36 + dataBytes; fwrite(&v32, 4, 1, f);
  fwrite("WAVEfmt ", 1, 8, f);
  v32 = 16; fwrite(&v32, 4, 1, f);
  uint16_t v16 = 1; fwrite(&v16, 2, 1, f);
  fwrite(&v16, 2, 1, f);
  v32 = REC_SAMPLE_RATE; fwrite(&v32, 4, 1, f);
  v32 = REC_SAMPLE_RATE * 2; fwrite(&v32, 4, 1, f);
  v16 = 2; fwrite(&v16, 2, 1, f);
  v16 = 16; fwrite(&v16, 2, 1, f);
  fwrite("data", 1, 4, f);
  fwrite(&dataBytes, 4, 1, f);
  fwrite(monoBuf, sizeof(int16_t), monoLen, f);
  fclose(f);
  heap_caps_free(monoBuf);
  monoBuf = nullptr;
  monoLen = 0;
  return true;
}

void recDiscard() {
  recording = false;
  if (monoBuf) { heap_caps_free(monoBuf); monoBuf = nullptr; }
  monoLen = 0;
}

bool playFile(const String& wavPath) {
  if (recording) return false;
  if (playing) playStop();
  playHandle = SD_MMC.open(wavPath, "r");
  if (!playHandle) return false;
  playHandle.seek(44);
  playing = true;
  return true;
}

void playPoll() {
  if (!playing || !stereoBuf) return;
  int16_t mono[PLAY_FRAMES];
  int got = playHandle.read((uint8_t*)mono, PLAY_FRAMES * sizeof(int16_t)) / sizeof(int16_t);
  if (got <= 0) { playStop(); return; }
  for (int i = 0; i < got; i++) {
    stereoBuf[2 * i] = mono[i];
    stereoBuf[2 * i + 1] = mono[i];
  }
  audio_playback_write(stereoBuf, got * 2 * sizeof(int16_t));
}

void playStop() {
  playing = false;
  if (playHandle) playHandle.close();
}

bool playActive() { return playing; }
