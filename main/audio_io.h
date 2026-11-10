#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t audio_io_init(uint32_t speaker_sample_rate);
esp_err_t audio_io_read_mic(int16_t *dst, size_t sample_count, size_t *read_count, uint32_t timeout_ms);
esp_err_t audio_io_write_speaker(const int16_t *pcm, size_t sample_count, uint32_t timeout_ms);
void audio_io_mute_speaker(void);
