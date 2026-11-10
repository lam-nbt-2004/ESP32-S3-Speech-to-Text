#include "audio_io.h"
#include "board_config.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if CONFIG_VA_STAGE_AFE_TEST || CONFIG_VA_STAGE_SERVER
#include "voice_afe.h"
#endif
#if CONFIG_VA_STAGE_SERVER
#include "voice_ws.h"
#include "wifi_connect.h"
#include "device_discovery.h"
#include "opus.h"
#include "sdkconfig.h"
#endif

#define MIC_BLOCK_SAMPLES 480
#define MAX_LOCAL_SECONDS 10
#define SILENCE_END_SAMPLES (VA_MIC_SAMPLE_RATE * 1200 / 1000)
#define COMMAND_START_TIMEOUT_MS 4000
#define WAKE_GUARD_MS 300
#define OPUS_MAX_PACKET 1500
#define WAKE_STREAM_TASK_STACK 40960
#define OPUS_TX_QUEUE_LENGTH 8

static const char *TAG = "voice_app";

#if CONFIG_VA_STAGE_SERVER
typedef struct {
    size_t len;
    uint8_t data[OPUS_MAX_PACKET];
} opus_tx_packet_t;

static QueueHandle_t s_opus_tx_queue;
static opus_tx_packet_t s_opus_tx_item;
/* Keep the real-time stream buffers out of the task's limited stack. */
static int16_t s_wake_mic[MIC_BLOCK_SAMPLES];
static int16_t s_wake_clean[960];
static int16_t s_opus_input[VA_OPUS_FRAME_SAMPLES];
static opus_tx_packet_t s_wake_packet;
static uint16_t s_mic_gain_q8 = 256; /* Q8 gain, initially 1.0x */
#endif

#if CONFIG_VA_STAGE_HW_TEST || CONFIG_VA_STAGE_AFE_TEST
static int16_t *alloc_pcm(size_t samples)
{
    int16_t *p = heap_caps_malloc(samples * sizeof(*p), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_malloc(samples * sizeof(*p), MALLOC_CAP_8BIT);
    return p;
}

static void play_pcm_local(const int16_t *pcm, size_t samples)
{
#if CONFIG_VA_STAGE_AFE_TEST
    voice_afe_pause(true);
#endif
    for (size_t off = 0; off < samples; ) {
        size_t n = samples - off;
        if (n > MIC_BLOCK_SAMPLES) n = MIC_BLOCK_SAMPLES;
        ESP_ERROR_CHECK(audio_io_write_speaker(pcm + off, n, 1000));
        off += n;
    }
    audio_io_mute_speaker();
#if CONFIG_VA_STAGE_AFE_TEST
    voice_afe_pause(false);
#endif
}

#endif

#if CONFIG_VA_STAGE_HW_TEST
static void stage1_hardware_check(void)
{
    const size_t capacity = VA_MIC_SAMPLE_RATE * 5;
    int16_t *pcm = alloc_pcm(capacity);
    ESP_ERROR_CHECK(pcm ? ESP_OK : ESP_ERR_NO_MEM);

    ESP_LOGI(TAG, "Stage 1: playing 400 Hz test tone through MAX98357A");
    int16_t tone[320];
    for (size_t i = 0; i < 320; ++i) tone[i] = (i % 40 < 20) ? 9000 : -9000;
    for (int i = 0; i < 20; ++i) ESP_ERROR_CHECK(audio_io_write_speaker(tone, 320, 500));
    audio_io_mute_speaker();

    ESP_LOGI(TAG, "Recording 5 seconds from INMP441; no button or LED is used");
    int16_t block[MIC_BLOCK_SAMPLES];
    size_t captured = 0;
    uint32_t peak = 0;
    uint64_t abs_sum = 0;
    unsigned read_failures = 0;
    int64_t record_start_us = esp_timer_get_time();
    int64_t next_status_us = record_start_us + 1000000LL;
    while (captured < capacity) {
        size_t got = 0;
        esp_err_t read_err = audio_io_read_mic(block, MIC_BLOCK_SAMPLES, &got, 100);
        int64_t now_us = esp_timer_get_time();
        if (read_err != ESP_OK || !got) {
            ++read_failures;
            if (now_us >= next_status_us) {
                ESP_LOGW(TAG, "Mic RX not delivering samples: err=%s, failures=%u, captured=%u/%u",
                         esp_err_to_name(read_err), read_failures, (unsigned)captured, (unsigned)capacity);
                next_status_us = now_us + 1000000LL;
            }
            if (now_us - record_start_us >= 10000000LL) {
                ESP_LOGE(TAG, "Mic capture timed out after 10 seconds; no complete recording to play");
                while (true) vTaskDelay(portMAX_DELAY);
            }
            continue;
        }
        if (got > capacity - captured) got = capacity - captured;
        for (size_t i = 0; i < got; ++i) {
            uint32_t magnitude = block[i] == INT16_MIN ? 32768U : (uint32_t)abs(block[i]);
            if (magnitude > peak) peak = magnitude;
            abs_sum += magnitude;
        }
        memcpy(pcm + captured, block, got * sizeof(*pcm));
        captured += got;
        if (now_us >= next_status_us) {
            ESP_LOGI(TAG, "Mic capture progress: %u/%u samples", (unsigned)captured, (unsigned)capacity);
            next_status_us = now_us + 1000000LL;
        }
    }
    ESP_LOGI(TAG, "Captured %u samples; microphone peak=%u, average absolute=%u",
             (unsigned)captured, (unsigned)peak,
             captured ? (unsigned)(abs_sum / captured) : 0);
    /* INMP441 level is low compared with the test tone; boost the Stage 1
       monitor only, with saturation to avoid wrapping/clipping artifacts. */
    const int32_t playback_gain = 8;
    for (size_t i = 0; i < captured; ++i) {
        int32_t boosted = (int32_t)pcm[i] * playback_gain;
        if (boosted > INT16_MAX) boosted = INT16_MAX;
        if (boosted < INT16_MIN) boosted = INT16_MIN;
        pcm[i] = (int16_t)boosted;
    }
    ESP_LOGI(TAG, "Applying %dx monitor gain; replaying recording", (int)playback_gain);
    ESP_LOGI(TAG, "Starting playback of %u samples", (unsigned)captured);
    play_pcm_local(pcm, captured);
    ESP_LOGI(TAG, "Playback complete; speaker output stopped. Stage 1 finished");
    while (true) vTaskDelay(portMAX_DELAY);
}
#endif

#if CONFIG_VA_STAGE_AFE_TEST
static void stage2_wakenet_loopback(void *arg)
{
    (void)arg;
    const size_t capacity = VA_MIC_SAMPLE_RATE * MAX_LOCAL_SECONDS;
    int16_t *pcm = alloc_pcm(capacity);
    ESP_ERROR_CHECK(pcm ? ESP_OK : ESP_ERR_NO_MEM);
    int16_t mic[MIC_BLOCK_SAMPLES];
    int16_t clean[960];
    size_t captured = 0;
    size_t silence_samples = 0;
    bool armed = false, command_started = false, post_wake_silence = false;
    int64_t wake_us = 0;
    ESP_LOGI(TAG, "Stage 2 ready; say the selected ESP-SR wake phrase (default: Hi ESP)");

    while (true) {
        size_t got = 0, clean_n = 0;
        bool wake = false, vad_speech = false;
        if (audio_io_read_mic(mic, MIC_BLOCK_SAMPLES, &got, 100) != ESP_OK || got != MIC_BLOCK_SAMPLES) continue;
        esp_err_t err = voice_afe_process(mic, got, clean, 960, &clean_n, &wake, &vad_speech);
        if (err != ESP_OK) { ESP_LOGE(TAG, "AFE processing failed: %s", esp_err_to_name(err)); continue; }
        int64_t now = esp_timer_get_time();

        if (!armed && wake) {
            armed = true;
            command_started = false;
            post_wake_silence = false;
            captured = 0;
            silence_samples = 0;
            wake_us = now;
            ESP_LOGI(TAG, "Wake word detected; waiting for command speech");
            continue;
        }
        if (!armed) continue;

        if (!command_started) {
            if (!vad_speech) post_wake_silence = true;
            else if (post_wake_silence || now - wake_us >= WAKE_GUARD_MS * 1000LL) {
                command_started = true;
                ESP_LOGI(TAG, "Command speech started");
            }
            if (!command_started) {
                if (now - wake_us >= COMMAND_START_TIMEOUT_MS * 1000LL) {
                    ESP_LOGW(TAG, "No command after wake phrase; returning to standby");
                    armed = false;
                }
                continue;
            }
        }

        if (clean_n > capacity - captured) clean_n = capacity - captured;
        memcpy(pcm + captured, clean, clean_n * sizeof(*pcm));
        captured += clean_n;
        if (vad_speech) silence_samples = 0;
        else silence_samples += clean_n;

        if (silence_samples >= SILENCE_END_SAMPLES || captured >= capacity) {
            ESP_LOGI(TAG, "Utterance complete: %u samples; playing processed audio locally", (unsigned)captured);
            play_pcm_local(pcm, captured);
            armed = false;
            ESP_LOGI(TAG, "Playback complete; say 'Hi ESP' to start the next recording");
        }
    }
}
#endif

#if CONFIG_VA_STAGE_SERVER
static OpusEncoder *s_encoder;
static char s_discovered_ws_uri[320];
static char s_discovered_device_token[256];

static void apply_mic_gain(int16_t *pcm, size_t count)
{
    if (!count) return;
    uint64_t abs_sum = 0;
    for (size_t i = 0; i < count; ++i) {
        int32_t sample = pcm[i];
        abs_sum += (uint32_t)(sample < 0 ? -sample : sample);
    }
    uint32_t mean_abs = (uint32_t)(abs_sum / count);
    if (mean_abs >= 350) {
        uint32_t target = (4800U * 256U) / mean_abs;
        if (target < 256U) target = 256U;
        if (target > 512U) target = 512U;
        s_mic_gain_q8 = (uint16_t)(((uint32_t)s_mic_gain_q8 + target) / 2U);
    }
    /* Retain the previous gain during silence; limit speech gain to 1–2x. */
    for (size_t i = 0; i < count; ++i) {
        int32_t sample = ((int32_t)pcm[i] * s_mic_gain_q8) / 256;
        if (sample > INT16_MAX) sample = INT16_MAX;
        if (sample < INT16_MIN) sample = INT16_MIN;
        pcm[i] = (int16_t)sample;
    }
}

static void queue_command_frame(int16_t *pcm, opus_tx_packet_t *packet)
{
    apply_mic_gain(pcm, VA_OPUS_FRAME_SAMPLES);
    int bytes = opus_encode(s_encoder, pcm, VA_OPUS_FRAME_SAMPLES,
                            packet->data, sizeof(packet->data));
    if (bytes <= 0) {
        ESP_LOGW(TAG, "Opus encode failed: %d", bytes);
        return;
    }
    packet->len = (size_t)bytes;
    if (xQueueSend(s_opus_tx_queue, packet, 0) != pdPASS) {
        ESP_LOGW(TAG, "Opus TX queue full; dropping one audio frame");
    }
}

static void websocket_start_task(void *arg)
{
    (void)arg;
    esp_err_t err = voice_ws_start(s_discovered_ws_uri, s_discovered_device_token);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Voice WebSocket not ready (%s); automatic reconnect remains enabled",
                 esp_err_to_name(err));
    }
    vTaskDelete(NULL);
}

/* HTTPS discovery/TLS has a much deeper call stack than the default main task.
   Keep discovery separate so its large stack is released before WebSocket TLS
   allocates its own task stack and handshake buffers. */
static void server_connect_task(void *arg)
{
    (void)arg;
    esp_err_t err = wifi_connect_sta();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi startup failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    esp_err_t discovery_err = device_discovery_fetch(CONFIG_VA_DEVICE_WS_URI,
                                                       s_discovered_ws_uri, sizeof(s_discovered_ws_uri),
                                                       s_discovered_device_token, sizeof(s_discovered_device_token));
    if (discovery_err != ESP_OK) {
        ESP_LOGW(TAG, "Device endpoint discovery failed (%s); falling back to menuconfig URL/token",
                 esp_err_to_name(discovery_err));
        snprintf(s_discovered_ws_uri, sizeof(s_discovered_ws_uri), "%s", CONFIG_VA_DEVICE_WS_URI);
        snprintf(s_discovered_device_token, sizeof(s_discovered_device_token), "%s", CONFIG_VA_DEVICE_TOKEN);
    }
    if (xTaskCreate(websocket_start_task, "ws_init", 6144, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Could not create WebSocket startup task");
    }
    vTaskDelete(NULL);
}

/* Keep potentially blocking Wi-Fi/TLS sends off the real-time AFE feed task. */
static void opus_tx_task(void *arg)
{
    (void)arg;
    bool logged_first_frame = false;
    for (;;) {
        if (xQueueReceive(s_opus_tx_queue, &s_opus_tx_item, portMAX_DELAY) != pdPASS) continue;
        if (voice_ws_is_playing() || !voice_ws_is_listening()) continue;
        if (voice_ws_send_opus(s_opus_tx_item.data, s_opus_tx_item.len) != ESP_OK) {
            ESP_LOGW(TAG, "Opus packet send failed");
        } else if (!logged_first_frame) {
            logged_first_frame = true;
            ESP_LOGI(TAG, "First Opus command frame sent to server");
        }
    }
}

static void wake_stream_task(void *arg)
{
    (void)arg;
    int16_t *mic = s_wake_mic;
    int16_t *clean = s_wake_clean;
    int16_t *opus_input = s_opus_input;
    opus_tx_packet_t *packet = &s_wake_packet;
    size_t opus_fill = 0;
    bool listening = false;
    while (!voice_ws_is_ready()) vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "Voice client ready; say '%s' to start, then speak normally after each reply",
             CONFIG_VA_WAKE_WORD_TEXT);

    while (true) {
        if (voice_ws_is_playing()) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        if (listening && (!voice_ws_is_ready() || !voice_ws_is_listening())) {
            listening = false;
            opus_fill = 0;
        }

        if (!listening && voice_ws_is_conversation_active() && voice_ws_is_ready()) {
            esp_err_t resume_err = voice_ws_continue_listening();
            if (resume_err == ESP_OK) {
                listening = true;
                opus_fill = 0;
                ESP_LOGI(TAG, "Listening for the next turn; no wake word needed");
            } else {
                ESP_LOGW(TAG, "Could not resume listening: %s", esp_err_to_name(resume_err));
                vTaskDelay(pdMS_TO_TICKS(500));
            }
        }

        size_t got = 0, clean_n = 0;
        bool wake = false, vad_speech = false;
        if (audio_io_read_mic(mic, MIC_BLOCK_SAMPLES, &got, 100) != ESP_OK || got != MIC_BLOCK_SAMPLES) continue;
        if (voice_ws_is_playing()) continue; /* TTS may have started while the I2S read was blocked. */
        esp_err_t err = ESP_OK;
        if (!listening || CONFIG_VA_NS_ENABLE) {
            err = voice_afe_process(mic, got, clean, 960, &clean_n, &wake, &vad_speech);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "AFE processing failed: %s", esp_err_to_name(err));
                continue;
            }
        }
        if (!listening) {
            if (wake && voice_ws_is_ready()) {
                (void)xQueueReset(s_opus_tx_queue);
                err = voice_ws_wake_start(CONFIG_VA_WAKE_WORD_TEXT);
                if (err == ESP_OK) {
                    listening = true;
                    opus_fill = 0;
                    ESP_LOGI(TAG, "Wake word accepted; say your first command now");
                } else {
                    ESP_LOGW(TAG, "Could not start server listening session: %s", esp_err_to_name(err));
                }
            }
            continue; /* Drop the wake frame; stream only the following command. */
        }
        /* Preserve all AFE output, including silence, for server endpointing. */
        const int16_t *stream_pcm = CONFIG_VA_NS_ENABLE ? clean : mic;
        size_t stream_samples = CONFIG_VA_NS_ENABLE ? clean_n : got;
        for (size_t i = 0; i < stream_samples; ++i) {
            opus_input[opus_fill++] = stream_pcm[i];
            if (opus_fill == VA_OPUS_FRAME_SAMPLES) {
                queue_command_frame(opus_input, packet);
                opus_fill = 0;
            }
        }
        /* I2S may already have another DMA block ready; yield so IDLE/WDT and
           networking tasks continue to run during sustained conversations. */
        vTaskDelay(1);
    }
}
#endif

void app_main(void)
{
    ESP_ERROR_CHECK(audio_io_init(VA_SPK_SAMPLE_RATE));
#if CONFIG_VA_STAGE_HW_TEST
    stage1_hardware_check();
#elif CONFIG_VA_STAGE_AFE_TEST
    ESP_ERROR_CHECK(voice_afe_init());
    ESP_ERROR_CHECK(xTaskCreate(stage2_wakenet_loopback, "afe_loop", 12288, NULL, 6, NULL) == pdPASS ? ESP_OK : ESP_FAIL);
#else
    ESP_ERROR_CHECK(voice_afe_init());
    int opus_error = 0;
    s_encoder = opus_encoder_create(VA_MIC_SAMPLE_RATE, 1, OPUS_APPLICATION_VOIP, &opus_error);
    ESP_ERROR_CHECK((s_encoder && opus_error == OPUS_OK) ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(opus_encoder_ctl(s_encoder, OPUS_SET_BITRATE(24000)) == OPUS_OK ? ESP_OK : ESP_FAIL);
    /* Keep per-frame DSP bounded so continuous turns leave CPU time for idle/WDT. */
    ESP_ERROR_CHECK(opus_encoder_ctl(s_encoder, OPUS_SET_COMPLEXITY(1)) == OPUS_OK ? ESP_OK : ESP_FAIL);
    s_opus_tx_queue = xQueueCreate(OPUS_TX_QUEUE_LENGTH, sizeof(opus_tx_packet_t));
    ESP_ERROR_CHECK(s_opus_tx_queue ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreate(opus_tx_task, "opus_tx", 16384, NULL, 5, NULL) == pdPASS ? ESP_OK : ESP_FAIL);
    /* This task runs ESP-SR and Opus work. Allocate its large stack before
       Wi-Fi/TLS consume internal heap. */
    ESP_ERROR_CHECK(xTaskCreate(wake_stream_task, "wake_stream", WAKE_STREAM_TASK_STACK, NULL, 4, NULL) == pdPASS ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(xTaskCreate(server_connect_task, "server_init", 16384, NULL, 5, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
#endif
}
