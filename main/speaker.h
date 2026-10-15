#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Synchronously play mono signed PCM16 audio at 16 kHz through MAX98357A.
 * The same samples are sent to the ESP-SR AFE playback-reference channel.
 *
 * pcm_data: pointer to sample bytes (does not need to be 16-bit aligned)
 * sample_count: number of mono samples, not bytes
 * sample_rate: must currently be 16000 Hz
 *
 * Call from a separate FreeRTOS task, not from the AFE feed/fetch task.
 */
esp_err_t speaker_play_pcm16(const void *pcm_data,
                             size_t sample_count,
                             uint32_t sample_rate);

#ifdef __cplusplus
}
#endif
