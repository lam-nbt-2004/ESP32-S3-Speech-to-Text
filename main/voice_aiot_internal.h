/*
 * Internal contract shared by the firmware modules.
 *
 * Keep hardware/service interfaces and cross-module state declarations here;
 * put each state variable's single definition in voice_aiot_state.c. This is
 * an application-private header, not a stable external library API.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_err.h"
#include "esp_afe_config.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/* ESP-SR and the Python Whisper endpoint exchange 16 kHz mono PCM16 audio. */
#define AUDIO_RATE              16000
#define AUDIO_BITS              16
#define MAX_RECORD_SEC          CONFIG_APP_MAX_RECORD_SEC
#ifndef CONFIG_APP_COMMAND_START_TIMEOUT_MS
#define CONFIG_APP_COMMAND_START_TIMEOUT_MS 4000
#endif
#define COMMAND_START_TIMEOUT_MS CONFIG_APP_COMMAND_START_TIMEOUT_MS
#define COMMAND_WAKE_GUARD_MS    300
#define PCM_CAPACITY            (AUDIO_RATE * MAX_RECORD_SEC)

#define MIC_BCLK_GPIO           ((gpio_num_t)CONFIG_APP_MIC_BCLK_GPIO)
#define MIC_WS_GPIO             ((gpio_num_t)CONFIG_APP_MIC_WS_GPIO)
#define MIC_DOUT_GPIO           ((gpio_num_t)CONFIG_APP_MIC_DOUT_GPIO)
#define SPK_BCLK_GPIO           ((gpio_num_t)CONFIG_APP_SPK_BCLK_GPIO)
#define SPK_WS_GPIO             ((gpio_num_t)CONFIG_APP_SPK_WS_GPIO)
#define SPK_DOUT_GPIO           ((gpio_num_t)CONFIG_APP_SPK_DOUT_GPIO)
#define LED_GPIO                ((gpio_num_t)CONFIG_APP_LED_GPIO)

#define APP_AEC_FILTER_LENGTH   4   /* config->aec_filter_length (SR_HIGH_PERF) */

#define I2S_READ_SAMPLES        512
#define SILENCE_MS              CONFIG_APP_SILENCE_MS
#define MAX_HTTP_TIMEOUT_MS     CONFIG_APP_HTTP_TIMEOUT_MS
#define WIFI_CONNECTED_BIT      BIT0
#define AUDIO_BUFFER_COUNT      3

/*
 * Playback-reference queue geometry. One queue item = one AFE feed frame of
 * the exact mono PCM16 that is also sent to the MAX98357A, plus a sequence
 * number and checksum so the feed task can detect dropped/duplicated/
 * reordered/corrupted frames. The same buffer is used for the TX copy and the
 * reference copy (no independent resampling, scaling or truncation).
 */
typedef struct {
    uint32_t seq;        /* 0-based frame index within one playback call   */
    uint32_t checksum;   /* ref_frame_checksum() of pcm[] as enqueued      */
    int16_t  pcm[];      /* afe_feed_chunk_samples mono PCM16 samples      */
} playback_ref_item_t;

#define PLAYBACK_REF_ITEM_BYTES(n) \
    (sizeof(playback_ref_item_t) + (size_t)(n) * sizeof(int16_t))

/* Bound queued reference audio to limit how far it can lead speaker playback. */
#define PLAYBACK_REF_QUEUE_DEPTH 3
#define PLAYBACK_REF_WAIT_MS 8

/* Reference-path diagnostics. Each field has a single writer task. */
typedef struct {
    volatile uint32_t enq_frames;        /* playback task: frames enqueued           */
    volatile int32_t  final_frames;      /* playback task: total enqueued, -1 = busy */
    volatile uint32_t enq_timeouts;      /* playback task: queue full, run aborted   */
    volatile uint32_t tx_errors;         /* playback task: I2S write failed/short    */
    volatile uint32_t consumed_frames;   /* feed task: highest consumed seq + 1      */
    volatile uint32_t underruns;         /* feed task: ref expected but missing      */
    volatile uint32_t seq_gaps;          /* feed task: frames skipped (sum of gaps)  */
    volatile uint32_t seq_dups;          /* feed task: same seq seen twice           */
    volatile uint32_t seq_ooo;           /* feed task: seq went backwards            */
    volatile uint32_t checksum_errors;   /* feed task: payload changed in queue      */
} ref_stats_t;

/* Standard 44-byte PCM WAV header. Packed layout is sent directly over HTTP. */
typedef struct __attribute__((packed)) {
    char     riff_header[4];
    uint32_t wav_size;
    char     wave_header[4];
    char     fmt_header[4];
    uint32_t fmt_chunk_size;
    uint16_t audio_format;
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bit_depth;
    char     data_header[4];
    uint32_t data_bytes;
} wav_header_t;

/*
 * Descriptor passed through audio_queue. The buffer itself is not copied:
 * buffer_index is the ownership token returned to free_buffer_queue by sender.
 */
typedef struct {
    int16_t *buffer;
    size_t samples;
    uint8_t buffer_index;
} audio_packet_t;

/* Shared state, defined once in voice_aiot_state.c. */
extern EventGroupHandle_t s_wifi_event_group;
extern i2s_chan_handle_t rx_chan;
extern i2s_chan_handle_t tx_chan;
extern QueueHandle_t playback_ref_queue;
extern ref_stats_t ref_stats;
extern SemaphoreHandle_t speaker_playback_mutex;
extern int afe_feed_chunk_samples;
extern const esp_afe_sr_iface_t *afe_handle;
extern esp_afe_sr_data_t *afe_data;
extern srmodel_list_t *sr_models;
extern int16_t *record_buffers[AUDIO_BUFFER_COUNT];
extern int16_t *record_buffer;
extern int active_record_buffer_index;
extern QueueHandle_t audio_queue;
extern QueueHandle_t free_buffer_queue;
extern volatile bool is_recording;
extern size_t recorded_samples;
extern int silence_frames;
extern TickType_t command_start_tick;
extern bool command_speech_started;
extern bool post_wake_silence_seen;
extern bool skip_until_silence;

/* Public entry points between the private firmware modules. */
void wifi_init_sta(void);
esp_err_t init_i2s_microphone(void);
esp_err_t init_i2s_speaker(void);
int16_t inmp441_32_to_pcm16(int32_t raw);
uint32_t ref_frame_checksum(const int16_t *pcm, size_t samples);
void ref_stats_reset(void);
esp_err_t speaker_play_pcm16(const void *mono_pcm16, size_t sample_count, uint32_t sample_rate);
void init_led(void);
void set_recording_indicator(bool on);
void init_record_buffers(void);
void append_samples(const int16_t *src, size_t samples);
bool acquire_record_buffer(void);
void release_active_record_buffer(void);
void release_packet_buffer(uint8_t index);
bool start_recording(void);
void cancel_current_recording(void);
void finish_recording(void);
void http_sender_task(void *arg);
void afe_feed_task(void *arg);
void afe_fetch_task(void *arg);
void afe_init(void);
