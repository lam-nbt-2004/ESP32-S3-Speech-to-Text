#include "voice_ws.h"
#include "sdkconfig.h"
#include "audio_io.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "esp_mac.h"
#include "esp_crt_bundle.h"
#include "opus.h"
#include "freertos/event_groups.h"
#include "board_config.h"
#include "voice_afe.h"
#include "esp_heap_caps.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#define WS_READY_BIT BIT0
#define WS_OPUS_MESSAGE_MAX 4096
#define TTS_OPUS_QUEUE_LENGTH 48
#define TTS_PREBUFFER_PACKETS 4
#define TTS_PREBUFFER_MS 240
#define TTS_PCM_MAX_SAMPLES ((VA_SPK_SAMPLE_RATE * 120) / 1000)
static const char *TAG = "voice_ws";
static esp_websocket_client_handle_t s_ws;
static EventGroupHandle_t s_ws_events;
static OpusDecoder *s_decoder;
static volatile bool s_hello;
static volatile bool s_listening;
static volatile bool s_playing;
static volatile bool s_conversation_active;
static char s_ws_uri[320];
static char s_ws_headers[320];
static uint8_t s_ws_opus_message[WS_OPUS_MESSAGE_MAX];
static size_t s_ws_message_size;
static size_t s_ws_frame_received;
static int s_ws_frame_size;
static bool s_ws_binary_active;
static bool s_ws_binary_overflow;
typedef struct {
    uint32_t generation;
    size_t bytes;
    uint8_t opus[];
} tts_opus_chunk_t;
static QueueHandle_t s_tts_opus_queue;
static volatile uint32_t s_playback_generation;
static volatile bool s_stop_after_queue;
static volatile bool s_abort_playback;
/* Owned by playback_task; keep the 120 ms decode buffer off its stack. */
static int16_t s_tts_pcm[TTS_PCM_MAX_SAMPLES];

static void discard_queued_opus(void)
{
    tts_opus_chunk_t *chunk;
    while (xQueueReceive(s_tts_opus_queue, &chunk, 0) == pdTRUE) {
        heap_caps_free(chunk);
    }
}

static void decode_and_play(const tts_opus_chunk_t *chunk)
{
    int samples = opus_packet_get_nb_samples(chunk->opus, (opus_int32)chunk->bytes,
                                             VA_SPK_SAMPLE_RATE);
    if (samples <= 0 || samples > TTS_PCM_MAX_SAMPLES) {
        ESP_LOGW(TAG, "Dropping invalid TTS Opus packet: bytes=%u samples=%d",
                 (unsigned)chunk->bytes, samples);
        return;
    }
    int decoded = opus_decode(s_decoder, chunk->opus, (opus_int32)chunk->bytes,
                              s_tts_pcm, TTS_PCM_MAX_SAMPLES, 0);
    if (decoded <= 0) {
        ESP_LOGW(TAG, "Opus decode failed: %d (message=%u bytes, samples=%d)",
                 decoded, (unsigned)chunk->bytes, samples);
        opus_decoder_ctl(s_decoder, OPUS_RESET_STATE);
        return;
    }
    esp_err_t err = audio_io_write_speaker(s_tts_pcm, (size_t)decoded, 1000);
    if (err != ESP_OK) ESP_LOGW(TAG, "Speaker write failed: %s", esp_err_to_name(err));
}

static void playback_task(void *arg)
{
    (void)arg;
    tts_opus_chunk_t *chunk;
    uint32_t decoder_generation = UINT32_MAX;
    uint32_t buffer_generation = UINT32_MAX;
    bool primed = false;
    bool buffering = false;
    TickType_t buffer_started = 0;
    for (;;) {
        if (buffer_generation != s_playback_generation) {
            buffer_generation = s_playback_generation;
            primed = false;
            buffering = false;
        }
        if (s_playing && !primed && !s_abort_playback) {
            UBaseType_t pending = uxQueueMessagesWaiting(s_tts_opus_queue);
            if (pending > 0 && !buffering) {
                buffer_started = xTaskGetTickCount();
                buffering = true;
            }
            /* Bound the wait for short replies; tts/stop releases even a
               single queued packet. Rebuffer after a network gap. */
            if (pending >= TTS_PREBUFFER_PACKETS || s_stop_after_queue ||
                (buffering && xTaskGetTickCount() - buffer_started >= pdMS_TO_TICKS(TTS_PREBUFFER_MS))) {
                primed = true;
            } else {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
        }
        if (xQueueReceive(s_tts_opus_queue, &chunk, pdMS_TO_TICKS(20)) == pdTRUE) {
            if (s_playing && chunk->generation == s_playback_generation) {
                /* A new TTS stream or reconnect must not inherit SILK history
                   from an interrupted stream. Only this task owns the decoder. */
                if (decoder_generation != chunk->generation) {
                    opus_decoder_ctl(s_decoder, OPUS_RESET_STATE);
                    decoder_generation = chunk->generation;
                }
                decode_and_play(chunk);
            }
            heap_caps_free(chunk);
            continue;
        }

        primed = false;
        buffering = false;

        /* The server sends tts/stop after the last binary packet. Drain the
           queue before muting so queued tail words are not discarded. */
        if ((s_stop_after_queue || s_abort_playback) &&
            uxQueueMessagesWaiting(s_tts_opus_queue) == 0) {
            audio_io_mute_speaker();
            s_abort_playback = false;
            s_stop_after_queue = false;
            s_playing = false;
        }
    }
}

static void abort_playback(void)
{
    ++s_playback_generation;
    s_stop_after_queue = false;
    s_abort_playback = true;
}

static esp_err_t send_control(const char *json);

static void handle_binary_audio(const esp_websocket_event_data_t *ev)
{
    if (ev->op_code == 0x2 && ev->payload_offset == 0) {
        s_ws_message_size = 0;
        s_ws_frame_received = 0;
        s_ws_frame_size = ev->payload_len;
        s_ws_binary_active = true;
        s_ws_binary_overflow = false;
    } else if (ev->op_code == 0x0 && ev->payload_offset == 0 && s_ws_binary_active) {
        /* Continuation WebSocket frame in a fragmented binary message. */
        s_ws_frame_received = 0;
        s_ws_frame_size = ev->payload_len;
    }

    if (!s_ws_binary_active || s_ws_binary_overflow || ev->data_len <= 0 ||
        ev->payload_offset != (int)s_ws_frame_received || ev->payload_len < 0 ||
        s_ws_frame_received + (size_t)ev->data_len > (size_t)ev->payload_len ||
        s_ws_message_size + (size_t)ev->data_len > sizeof(s_ws_opus_message)) {
        if (s_ws_binary_active && !s_ws_binary_overflow) {
            ESP_LOGW(TAG, "Dropping malformed/oversized TTS binary message (frame=%d, offset=%d, chunk=%d)",
                     ev->payload_len, ev->payload_offset, ev->data_len);
        }
        s_ws_binary_overflow = true;
        if (ev->fin) s_ws_binary_active = false;
        return;
    }

    memcpy(s_ws_opus_message + s_ws_message_size, ev->data_ptr, (size_t)ev->data_len);
    s_ws_message_size += (size_t)ev->data_len;
    s_ws_frame_received += (size_t)ev->data_len;

    if (s_ws_frame_received == (size_t)s_ws_frame_size && ev->fin) {
        size_t chunk_size = sizeof(tts_opus_chunk_t) + s_ws_message_size;
        tts_opus_chunk_t *chunk = heap_caps_malloc(chunk_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!chunk) chunk = heap_caps_malloc(chunk_size, MALLOC_CAP_8BIT);
        if (chunk) {
            chunk->generation = s_playback_generation;
            chunk->bytes = s_ws_message_size;
            memcpy(chunk->opus, s_ws_opus_message, s_ws_message_size);
            if (xQueueSend(s_tts_opus_queue, &chunk, 0) != pdTRUE) {
                heap_caps_free(chunk);
                ESP_LOGW(TAG, "TTS Opus queue full; dropping one packet");
            }
        } else {
            ESP_LOGW(TAG, "No memory for TTS Opus packet");
        }
        s_ws_binary_active = false;
        s_ws_binary_overflow = false;
        s_ws_message_size = 0;
    }
}

static void handle_json(const char *text, int len)
{
    cJSON *root = cJSON_ParseWithLength(text, len);
    if (!root) return;
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    const cJSON *state = cJSON_GetObjectItemCaseSensitive(root, "state");
    const cJSON *word = cJSON_GetObjectItemCaseSensitive(root, "text");
    if (cJSON_IsString(type) && strcmp(type->valuestring, "hello") == 0) {
        s_hello = true;
        xEventGroupSetBits(s_ws_events, WS_READY_BIT);
        ESP_LOGI(TAG, "Voice server hello received");
    } else if (cJSON_IsString(type) && strcmp(type->valuestring, "stt") == 0 && cJSON_IsString(word)) {
        ESP_LOGI(TAG, "STT: %s", word->valuestring);
    } else if (cJSON_IsString(type) && strcmp(type->valuestring, "llm") == 0 && cJSON_IsString(word)) {
        ESP_LOGI(TAG, "Assistant emotion: %s", word->valuestring);
    } else if (cJSON_IsString(type) && strcmp(type->valuestring, "tts") == 0 && cJSON_IsString(state)) {
        if (strcmp(state->valuestring, "start") == 0) {
            ESP_LOGI(TAG, "TTS playback started");
            s_listening = false;
            ++s_playback_generation;
            discard_queued_opus();
            s_stop_after_queue = false;
            s_abort_playback = false;
            s_playing = true;
            voice_afe_pause(true);
        } else if (strcmp(state->valuestring, "sentence_start") == 0) {
            if (cJSON_IsString(word)) ESP_LOGI(TAG, "Assistant says: %s", word->valuestring);
            else ESP_LOGI(TAG, "TTS sentence started");
        } else if (strcmp(state->valuestring, "stop") == 0) {
            s_stop_after_queue = true;
            /* The capture task resumes AFE/NS when it opens the next turn. */
            if (!s_conversation_active) voice_afe_pause(false);
            ESP_LOGI(TAG, "TTS finished; conversation session remains active");
        } else {
            ESP_LOGI(TAG, "TTS state: %s", state->valuestring);
        }
    } else if (cJSON_IsString(type) && strcmp(type->valuestring, "error") == 0) {
        const cJSON *message = cJSON_GetObjectItemCaseSensitive(root, "message");
        ESP_LOGE(TAG, "Server error: %s", cJSON_IsString(message) ? message->valuestring : "unknown");
    }
    cJSON_Delete(root);
}

static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    (void)arg;
    (void)base;
    esp_websocket_event_data_t *ev = event_data;
    if (id == WEBSOCKET_EVENT_CONNECTED) {
        const char *hello = "{\"type\":\"hello\",\"version\":1,\"features\":{\"mcp\":false},\"transport\":\"websocket\",\"audio_params\":{\"format\":\"opus\",\"sample_rate\":16000,\"channels\":1,\"frame_duration\":60}}";
        esp_websocket_client_send_text(s_ws, hello, strlen(hello), pdMS_TO_TICKS(1000));
    } else if (id == WEBSOCKET_EVENT_DATA && ev) {
        if (ev->op_code == 0x1 && ev->data_len > 0) handle_json(ev->data_ptr, ev->data_len);
        else if ((ev->op_code == 0x2 || ev->op_code == 0x0) && s_decoder)
            handle_binary_audio(ev);
    } else if (id == WEBSOCKET_EVENT_DISCONNECTED || id == WEBSOCKET_EVENT_ERROR) {
        abort_playback();
        s_hello = false;
        s_listening = false;
        s_playing = false;
        s_conversation_active = false;
        voice_afe_pause(false);
        xEventGroupClearBits(s_ws_events, WS_READY_BIT);
        ESP_LOGW(TAG, "WebSocket disconnected/error");
    }
}

esp_err_t voice_ws_start(const char *uri, const char *token)
{
    if (!uri || !*uri || !token) return ESP_ERR_INVALID_ARG;
    int uri_len = snprintf(s_ws_uri, sizeof(s_ws_uri), "%s", uri);
    if (uri_len < 0 || (size_t)uri_len >= sizeof(s_ws_uri)) return ESP_ERR_INVALID_SIZE;
    s_ws_events = xEventGroupCreate();
    if (!s_ws_events) return ESP_ERR_NO_MEM;
    s_tts_opus_queue = xQueueCreate(TTS_OPUS_QUEUE_LENGTH, sizeof(tts_opus_chunk_t *));
    if (!s_tts_opus_queue) return ESP_ERR_NO_MEM;
    /* Keep pointer-rich SILK decoder state in internal, byte-accessible RAM.
       Large codec scratch buffers continue to use PSRAM via micro-opus. */
    int decoder_bytes = opus_decoder_get_size(1);
    if (decoder_bytes <= 0) return ESP_FAIL;
    s_decoder = heap_caps_malloc((size_t)decoder_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_decoder) return ESP_ERR_NO_MEM;
    int error = opus_decoder_init(s_decoder, VA_SPK_SAMPLE_RATE, 1);
    if (error != OPUS_OK) {
        heap_caps_free(s_decoder);
        s_decoder = NULL;
        return ESP_FAIL;
    }
    if (xTaskCreate(playback_task, "tts_playback", 8192, NULL, 5, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
    int header_len = snprintf(s_ws_headers, sizeof(s_ws_headers),
        "Protocol-Version: 1\r\nDevice-Id: %02x:%02x:%02x:%02x:%02x:%02x\r\n",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    if (header_len < 0 || (size_t)header_len >= sizeof(s_ws_headers)) return ESP_ERR_INVALID_SIZE;
    if (token[0]) {
        header_len += snprintf(s_ws_headers + header_len, sizeof(s_ws_headers) - (size_t)header_len,
                               "Authorization: Bearer %s\r\n", token);
    }
    if (header_len < 0 || (size_t)header_len >= sizeof(s_ws_headers)) return ESP_ERR_INVALID_SIZE;
    esp_websocket_client_config_t cfg = { .uri = s_ws_uri,
        .network_timeout_ms = 10000, .reconnect_timeout_ms = 5000, .buffer_size = 4096,
        .task_stack = 12288, .crt_bundle_attach = esp_crt_bundle_attach,
        .headers = s_ws_headers, .disable_auto_reconnect = false };
    s_ws = esp_websocket_client_init(&cfg);
    if (!s_ws) return ESP_ERR_NO_MEM;
    ESP_ERROR_CHECK(esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event, NULL));
    ESP_ERROR_CHECK(esp_websocket_client_start(s_ws));
    EventBits_t bits = xEventGroupWaitBits(s_ws_events, WS_READY_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(15000));
    return (bits & WS_READY_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
}

bool voice_ws_is_ready(void) { return s_hello; }
bool voice_ws_is_listening(void) { return s_listening; }
bool voice_ws_is_playing(void) { return s_playing; }
bool voice_ws_is_conversation_active(void) { return s_conversation_active; }

static esp_err_t send_control(const char *json)
{
    if (!s_ws || !voice_ws_is_ready()) return ESP_ERR_INVALID_STATE;
    int n = esp_websocket_client_send_text(s_ws, json, strlen(json), pdMS_TO_TICKS(3000));
    return n == (int)strlen(json) ? ESP_OK : ESP_FAIL;
}

static esp_err_t start_listening(void)
{
    esp_err_t err = send_control("{\"type\":\"listen\",\"state\":\"start\",\"mode\":\"auto\"}");
    if (err == ESP_OK) {
        voice_afe_pause(!CONFIG_VA_NS_ENABLE);
        s_listening = true;
    }
    return err;
}

esp_err_t voice_ws_wake_start(const char *wake_phrase)
{
    if (!wake_phrase || !*wake_phrase) return ESP_ERR_INVALID_ARG;
    char detect[192];
    int n = snprintf(detect, sizeof(detect),
                     "{\"type\":\"listen\",\"state\":\"detect\",\"text\":\"%s\"}", wake_phrase);
    if (n < 0 || (size_t)n >= sizeof(detect)) return ESP_ERR_INVALID_SIZE;
    esp_err_t err = send_control(detect);
    if (err != ESP_OK) return err;
    err = start_listening();
    if (err == ESP_OK) s_conversation_active = true;
    return err;
}

esp_err_t voice_ws_continue_listening(void)
{
    if (!s_conversation_active || !s_hello) return ESP_ERR_INVALID_STATE;
    if (s_listening) return ESP_OK;
    return start_listening();
}

esp_err_t voice_ws_send_opus(const uint8_t *packet, size_t len)
{
    if (!s_ws || !voice_ws_is_ready() || !packet || !len || len > INT32_MAX) return ESP_ERR_INVALID_STATE;
    int sent = esp_websocket_client_send_bin(s_ws, (const char *)packet, (int)len, pdMS_TO_TICKS(3000));
    return sent == (int)len ? ESP_OK : ESP_FAIL;
}
