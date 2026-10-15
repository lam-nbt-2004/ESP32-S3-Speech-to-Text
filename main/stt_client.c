/*
 * Module: ESP32-to-Python speech-to-text client
 *
 * Converts one queued mono PCM16 utterance into a standard WAV request and
 * posts it to CONFIG_APP_SERVER_URL. The HTTP sender is deliberately a separate
 * FreeRTOS task: network latency must not stall the AFE feed/fetch loop.
 */
#include "voice_aiot_internal.h"

#include <string.h>

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "STT_CLIENT";

/**
 * Parse the server's JSON response and print the recognized words/language.
 * This module intentionally does not generate an answer or play audio; STT is
 * the end of the current application pipeline.
 */
static void handle_transcription_response(const char *response)
{
    cJSON *root = cJSON_Parse(response);
    if (root == NULL) {
        ESP_LOGW(TAG, "Could not parse STT JSON response");
        return;
    }

    const cJSON *status = cJSON_GetObjectItemCaseSensitive(root, "status");
    const cJSON *text_item = cJSON_GetObjectItemCaseSensitive(root, "text");
    const cJSON *language_item = cJSON_GetObjectItemCaseSensitive(root, "language");

    if (!cJSON_IsString(status) || strcmp(status->valuestring, "success") != 0 ||
        !cJSON_IsString(text_item) || text_item->valuestring == NULL ||
        text_item->valuestring[0] == '\0') {
        ESP_LOGW(TAG, "STT returned no recognized text");
        cJSON_Delete(root);
        return;
    }

    const char *language = cJSON_IsString(language_item)
                               ? language_item->valuestring : "unknown";
    ESP_LOGI(TAG, "STT transcript (%s): %s", language, text_item->valuestring);
    cJSON_Delete(root);
}


/**
 * Build a PCM WAV header, POST header + samples, then log the HTTP response.
 * ESP32-S3 is little-endian, as required by the PCM WAV fields written here.
 * The caller remains responsible for returning packet->buffer_index to the
 * buffer pool after this function finishes, including on network errors.
 */
static void send_audio_to_server(const audio_packet_t *packet)
{
    const size_t pcm_bytes = packet->samples * sizeof(int16_t);
    const size_t wav_bytes = sizeof(wav_header_t) + pcm_bytes;

    /* WAV contains mono, signed 16-bit PCM at the AFE's 16 kHz sample rate. */
    wav_header_t header = {
        .riff_header = {'R', 'I', 'F', 'F'},
        .wav_size = (uint32_t)(wav_bytes - 8),
        .wave_header = {'W', 'A', 'V', 'E'},
        .fmt_header = {'f', 'm', 't', ' '},
        .fmt_chunk_size = 16,
        .audio_format = 1,
        .num_channels = 1,
        .sample_rate = AUDIO_RATE,
        .byte_rate = AUDIO_RATE * sizeof(int16_t),
        .block_align = sizeof(int16_t),
        .bit_depth = AUDIO_BITS,
        .data_header = {'d', 'a', 't', 'a'},
        .data_bytes = (uint32_t)pcm_bytes,
    };

    ESP_LOGI(TAG, "POST %u-byte WAV -> %s",
             (unsigned)wav_bytes, CONFIG_APP_SERVER_URL);

    esp_http_client_config_t config = {
        .url = CONFIG_APP_SERVER_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = MAX_HTTP_TIMEOUT_MS,
        .keep_alive_enable = true,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "esp_http_client_init failed");
        return;
    }

    esp_err_t err = esp_http_client_set_header(
        client, "Content-Type", "audio/wav");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set Content-Type failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return;
    }

    err = esp_http_client_open(client, (int)wav_bytes);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return;
    }

    int written = esp_http_client_write(
        client, (const char *)&header, sizeof(header));
    if (written != sizeof(header)) {
        ESP_LOGE(TAG, "WAV header write failed: %d", written);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return;
    }

    written = esp_http_client_write(
        client, (const char *)packet->buffer, (int)pcm_bytes);
    if (written != (int)pcm_bytes) {
        ESP_LOGE(TAG, "PCM write failed: %d/%u",
                 written, (unsigned)pcm_bytes);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);

    char response[2048] = {0};
    int read_len = esp_http_client_read_response(
        client, response, sizeof(response) - 1);
    if (read_len > 0 && (size_t)read_len < sizeof(response)) {
        response[read_len] = '\0';
    }

    ESP_LOGI(TAG, "HTTP status=%d content_length=%d response=%.*s",
             status, content_length, read_len > 0 ? read_len : 0, response);

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    /* STT-only stage: print the recognized transcript to the serial monitor. */
    if (status >= 200 && status < 300 && read_len > 0 &&
        (size_t)read_len < sizeof(response)) {
        handle_transcription_response(response);
    }
}

/**
 * Wait for completed utterances, send each to the STT server, and always
 * release its recording buffer afterward so capture can continue.
 */
void http_sender_task(void *arg)
{
    (void)arg;

    audio_packet_t packet;

    while (true) {
        if (xQueueReceive(audio_queue, &packet, portMAX_DELAY) == pdTRUE) {
            send_audio_to_server(&packet);
            release_packet_buffer(packet.buffer_index);
        }
    }
}
