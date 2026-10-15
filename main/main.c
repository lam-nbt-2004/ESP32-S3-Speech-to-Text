/*
 * Module: application entry point
 *
 * Starts the services in dependency order: persistent storage, hardware,
 * Wi-Fi, ESP-SR AFE, recording queues, and the long-running processing tasks.
 * Signal processing and HTTP details live in their own modules so the startup
 * sequence remains easy to review.
 */
#include "voice_aiot_internal.h"

#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "VOICE_AIOT";

/**
 * ESP-IDF application entry point.
 *
 * The AFE feed task continuously supplies microphone/reference frames and the
 * AFE fetch task handles wake-word detection and utterance capture. HTTP
 * transmission runs separately so network latency does not block audio input.
 */
void app_main(void)
{
    /* Initialize NVS before Wi-Fi; the Wi-Fi driver uses it for configuration. */
    ESP_ERROR_CHECK(nvs_flash_init());

    ESP_LOGI(TAG, "ESP32-S3 AIoT Voice Control starting");
    ESP_LOGI(TAG, "Mode: one utterance per wake word; STT only, no answer/TTS stage");

    init_led();
    init_record_buffers();

    /* Configure the INMP441 input and MAX98357A output interfaces. */
    ESP_ERROR_CHECK(init_i2s_microphone());
    ESP_ERROR_CHECK(init_i2s_speaker());

    /* Playback serializes access to I2S TX and its AEC reference queue. */
    speaker_playback_mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(speaker_playback_mutex != NULL ? ESP_OK : ESP_ERR_NO_MEM);

    wifi_init_sta();
    afe_init();

    /* One queued reference item corresponds to one AFE feed frame. */
    afe_feed_chunk_samples = afe_handle->get_feed_chunksize(afe_data);
    ESP_ERROR_CHECK(afe_feed_chunk_samples > 0 ? ESP_OK : ESP_FAIL);

    playback_ref_queue = xQueueCreate(
        PLAYBACK_REF_QUEUE_DEPTH,
        (UBaseType_t)PLAYBACK_REF_ITEM_BYTES(afe_feed_chunk_samples));
    ESP_ERROR_CHECK(playback_ref_queue != NULL ? ESP_OK : ESP_ERR_NO_MEM);

    ESP_LOGI(TAG, "AEC reference queue ready: depth %d, %d samples/frame",
             PLAYBACK_REF_QUEUE_DEPTH, afe_feed_chunk_samples);

    /* Keep the real-time audio tasks separate from the network sender. */
    ESP_ERROR_CHECK(xTaskCreatePinnedToCore(
        afe_feed_task, "afe_feed", 8192, NULL, 7, NULL, 0) == pdPASS
            ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(xTaskCreatePinnedToCore(
        afe_fetch_task, "afe_fetch", 12288, NULL, 8, NULL, 1) == pdPASS
            ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(xTaskCreatePinnedToCore(
        http_sender_task, "http_sender", 8192, NULL, 4, NULL, 0) == pdPASS
            ? ESP_OK : ESP_FAIL);

    ESP_LOGI(TAG, "System running; say Hi ESP before each command");
}
