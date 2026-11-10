#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "sdkconfig.h"
#ifndef CONFIG_VA_NS_ENABLE
#define CONFIG_VA_NS_ENABLE 0
#endif

esp_err_t voice_afe_init(void);
void voice_afe_pause(bool paused);
esp_err_t voice_afe_process(const int16_t *mic, size_t samples, int16_t *clean, size_t clean_capacity, size_t *clean_samples, bool *wake_detected, bool *vad_speech);
