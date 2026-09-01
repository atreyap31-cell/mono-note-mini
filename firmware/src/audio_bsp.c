#include <stdio.h>
#include "audio_bsp.h"
#include "src/codec_board/codec_board.h"
#include "src/codec_board/codec_init.h"
#include "src/esp_codec_dev/include/esp_codec_dev.h"
#include "esp_heap_caps.h"

esp_codec_dev_handle_t playback = NULL;
esp_codec_dev_handle_t record = NULL;

void audio_bsp_init(void)
{
  set_codec_board_type("S3_ePaper_1_54");
  codec_init_cfg_t codec_cfg = {};
  codec_cfg.in_mode = CODEC_I2S_MODE_STD;
  codec_cfg.out_mode = CODEC_I2S_MODE_STD;
  codec_cfg.in_use_tdm = false;
  codec_cfg.reuse_dev = false;
  ESP_ERROR_CHECK(init_codec(&codec_cfg));
  playback = get_playback_handle();
  record = get_record_handle();
}

void audio_play_init(void)
{
  esp_codec_dev_set_out_vol(playback, 80.0);
  esp_codec_dev_set_in_gain(record, 45.0);
  esp_codec_dev_sample_info_t fs = {};
  fs.sample_rate = 16000;
  fs.channel = 2;
  fs.bits_per_sample = 16;
  esp_codec_dev_open(playback, &fs);
  esp_codec_dev_open(record, &fs);
}

void audio_playback_set_vol(uint8_t vol)
{
  esp_codec_dev_set_out_vol(playback, vol);
}

void audio_playback_read(void *data_ptr, uint32_t len)
{
  esp_codec_dev_read(record, data_ptr, len);
}

/* Returns the codec's own error code instead of discarding it. A write that
   quietly fails looks exactly like a speaker that is not connected, and there
   is no way to tell them apart from outside the device. */
int audio_playback_write(void *data_ptr, uint32_t len)
{
  if (!playback) return -9999;             /* no playback device at all */
  return esp_codec_dev_write(playback, data_ptr, len);
}
