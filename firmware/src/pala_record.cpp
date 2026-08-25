#include "pala_record.h"
#include "audio_bsp.h"
#include "esp_heap_caps.h"
#include <string.h>

#define REC_SAMPLE_RATE 16000
#define REC_MAX_SECONDS 120

static int16_t* monoBuf = nullptr;
static size_t monoLen = 0;
static bool recording = false;

bool recBegin() {
  audio_play_init();
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
  for (int i = 0; i < frames; i++)
    monoBuf[monoLen++] = (int16_t)(((int32_t)stereo[2 * i] + stereo[2 * i + 1]) >> 1);
}

uint32_t recSeconds() { return monoLen / REC_SAMPLE_RATE; }

static void putStr(FILE* f, const char* s, size_t n) { fwrite(s, 1, n, f); }
static void putU32(FILE* f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void putU16(FILE* f, uint16_t v) { fwrite(&v, 2, 1, f); }

bool recSave(const String& wavPath) {
  if (!monoBuf || monoLen == 0) return false;
  recording = false;
  FILE* f = fopen(wavPath.c_str(), "wb");
  if (!f) return false;
  uint32_t dataBytes = monoLen * sizeof(int16_t);
  putStr(f, "RIFF", 4); putU32(f, 36 + dataBytes); putStr(f, "WAVE", 4);
  putStr(f, "fmt ", 4); putU32(f, 16); putU16(f, 1); putU16(f, 1);
  putU32(f, REC_SAMPLE_RATE); putU32(f, REC_SAMPLE_RATE * 2);
  putU16(f, 2); putU16(f, 16);
  putStr(f, "data", 4); putU32(f, dataBytes);
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
