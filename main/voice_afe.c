#include "voice_afe.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "model_path.h"
#include "esp_afe_sr_models.h"
#include "esp_afe_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "voice_afe";
static srmodel_list_t *s_models;
static const esp_afe_sr_iface_t *s_afe;
static esp_afe_sr_data_t *s_data;
static int16_t s_feed_pending[2048];
static size_t s_feed_pending_samples;

#define AFE_OUTPUT_MAX_SAMPLES 960
typedef struct {
    size_t samples;
    bool wake;
    bool vad_speech;
    int16_t pcm[AFE_OUTPUT_MAX_SAMPLES];
} afe_output_t;

static QueueHandle_t s_output_queue;
static TaskHandle_t s_fetch_task_handle;
static volatile bool s_fetch_paused;
static afe_output_t s_fetch_item;
static afe_output_t s_output_item;
static afe_output_t s_discard_item;

/* ESP-SR requires feed and fetch to make progress independently. */
static void afe_fetch_task(void *arg)
{
    (void)arg;
    int64_t last_error_log_us = 0;
    for (;;) {
        /* Do not poll an empty AFE ringbuffer while capture is paused for playback. */
        (void)ulTaskNotifyTake(pdFALSE, portMAX_DELAY);
        if (s_fetch_paused) {
            continue;
        }
        afe_fetch_result_t *r = s_afe->fetch_with_delay(s_data, pdMS_TO_TICKS(100));
        if (!r || r->ret_value != ESP_OK || !r->data || r->data_size < 0 ||
            (r->data_size % (int)sizeof(int16_t)) != 0) {
            int64_t now = esp_timer_get_time();
            if (now - last_error_log_us >= 5000000LL) {
                ESP_LOGW(TAG, "AFE fetch has no output yet; continuing to wait for feed data");
                last_error_log_us = now;
            }
            continue;
        }
        size_t samples = (size_t)r->data_size / sizeof(int16_t);
        if (samples > AFE_OUTPUT_MAX_SAMPLES) {
            ESP_LOGE(TAG, "AFE output frame too large: %u samples", (unsigned)samples);
            continue;
        }
        s_fetch_item.samples = samples;
        s_fetch_item.wake = r->wakeup_state == WAKENET_DETECTED;
        s_fetch_item.vad_speech = r->vad_state == VAD_SPEECH;
        memcpy(s_fetch_item.pcm, r->data, r->data_size);
        if (xQueueSend(s_output_queue, &s_fetch_item, 0) != pdPASS) {
            (void)xQueueReceive(s_output_queue, &s_discard_item, 0);
            (void)xQueueSend(s_output_queue, &s_fetch_item, 0);
        }
    }
}

void voice_afe_pause(bool paused)
{
    s_fetch_paused = paused;
    /* Only feed() grants fetch work. Resuming without input must not trigger
       an empty-ringbuffer fetch. */
}

esp_err_t voice_afe_init(void)
{
    s_models = esp_srmodel_init("model");
    if (!s_models) {
        ESP_LOGE(TAG, "ESP-SR model partition/list missing; select AFE/VAD models in ESP-SR menuconfig");
        return ESP_ERR_NOT_FOUND;
    }
    afe_config_t *cfg = afe_config_init("M", s_models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    if (!cfg) return ESP_ERR_NO_MEM;
    cfg->aec_init = false; /* Half-duplex wake/STT flow: capture pauses during speaker playback. */
    cfg->ns_init = CONFIG_VA_NS_ENABLE;
    cfg->ns_model_name = "WEBRTC";
    cfg->afe_ns_mode = AFE_NS_MODE_WEBRTC;
    cfg->agc_init = false; /* Gain is already bounded before Opus encoding. */
    cfg->vad_init = true;
    cfg->wakenet_init = true;
    cfg->wakenet_model_name = esp_srmodel_filter(s_models, ESP_WN_PREFIX, NULL);
    if (!cfg->wakenet_model_name) {
        ESP_LOGE(TAG, "No WakeNet model found; select wn9_hiesp in ESP-SR menuconfig");
        afe_config_free(cfg);
        return ESP_ERR_NOT_FOUND;
    }
    const char *wake_model = cfg->wakenet_model_name;
    s_afe = esp_afe_handle_from_config(cfg);
    if (!s_afe) { afe_config_free(cfg); return ESP_ERR_NOT_FOUND; }
    s_data = s_afe->create_from_config(cfg);
    afe_config_free(cfg);
    if (!s_data) return ESP_ERR_NO_MEM;
    s_feed_pending_samples = 0;
    s_output_queue = xQueueCreate(4, sizeof(afe_output_t));
    if (!s_output_queue) return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "AFE frames: feed=%d samples, fetch=%d samples",
             s_afe->get_feed_chunksize(s_data), s_afe->get_fetch_chunksize(s_data));
    ESP_LOGI(TAG, "AFE ready: mono mic, VAD, WakeNet model=%s, WebRTC NS=%s",
             wake_model, CONFIG_VA_NS_ENABLE ? "enabled" : "disabled");
    /* fetch_with_delay runs ESP-SR NS/VAD/WakeNet processing. Give its task
       enough room for the model call chain, especially at wake detection. */
    if (xTaskCreate(afe_fetch_task, "afe_fetch", 16384, NULL, 5, &s_fetch_task_handle) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

esp_err_t voice_afe_process(const int16_t *mic, size_t samples, int16_t *clean, size_t cap, size_t *clean_samples, bool *wake, bool *vad_speech)
{
    if (!s_afe || !s_data || !mic || !clean || !clean_samples || !wake || !vad_speech) return ESP_ERR_INVALID_STATE;
    *clean_samples = 0;
    *wake = false;
    *vad_speech = false;
    const size_t frame = (size_t)s_afe->get_feed_chunksize(s_data);
    if (!frame || frame > (sizeof(s_feed_pending) / sizeof(s_feed_pending[0]))) return ESP_ERR_INVALID_SIZE;
    if (samples > (sizeof(s_feed_pending) / sizeof(s_feed_pending[0])) - s_feed_pending_samples) return ESP_ERR_INVALID_SIZE;
    memcpy(s_feed_pending + s_feed_pending_samples, mic, samples * sizeof(*mic));
    s_feed_pending_samples += samples;
    unsigned fed_frames = 0;
    while (s_feed_pending_samples >= frame) {
        int ret = s_afe->feed(s_data, s_feed_pending);
        if (ret <= 0) return ESP_FAIL;
        ++fed_frames;
        s_feed_pending_samples -= frame;
        if (s_feed_pending_samples) {
            memmove(s_feed_pending, s_feed_pending + frame,
                    s_feed_pending_samples * sizeof(s_feed_pending[0]));
        }
    }
    for (unsigned i = 0; i < fed_frames; ++i) xTaskNotifyGive(s_fetch_task_handle);

    /* Fetch runs on its own task; output may be delayed by AFE frame buffering. */
    if (xQueueReceive(s_output_queue, &s_output_item, 0) == pdPASS) {
        if (s_output_item.samples > cap) return ESP_ERR_INVALID_SIZE;
        memcpy(clean, s_output_item.pcm, s_output_item.samples * sizeof(*clean));
        *clean_samples = s_output_item.samples;
        *wake = s_output_item.wake;
        *vad_speech = s_output_item.vad_speech;
    }
    return ESP_OK;
}
