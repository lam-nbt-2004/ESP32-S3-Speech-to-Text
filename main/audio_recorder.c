/*
 * Module: utterance recording and buffer ownership
 *
 * The AFE fetch task appends samples into one active buffer. When an utterance
 * completes, an audio_packet_t is queued for the HTTP sender. That queue handoff
 * transfers ownership of the buffer index to the sender, which must return it
 * to free_buffer_queue after the request. This small pool avoids allocating a
 * multi-second recording buffer on every wake word.
 */
#include "voice_aiot_internal.h"

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "RECORDER";

void set_recording_indicator(bool on)
{
    gpio_set_level(LED_GPIO, on ? 1 : 0);
}

/**
 * Append PCM16 samples without exceeding the configured maximum duration.
 * Any excess input is clipped at PCM_CAPACITY; finish_recording() is called
 * by the AFE module once it observes this limit.
 */
void append_samples(const int16_t *src, size_t samples)
{
    if (!src || samples == 0 || record_buffer == NULL ||
        recorded_samples >= PCM_CAPACITY) {
        return;
    }

    const size_t room = PCM_CAPACITY - recorded_samples;
    const size_t copy_samples = samples < room ? samples : room;
    memcpy(record_buffer + recorded_samples, src,
           copy_samples * sizeof(int16_t));
    recorded_samples += copy_samples;
}

/**
 * Take one buffer index from the free pool and make it the active recorder
 * buffer. A zero-wait receive prevents a real-time AFE task from blocking if
 * every buffer is currently being processed by the sender.
 */
bool acquire_record_buffer(void)
{
    uint8_t index = 0;
    if (free_buffer_queue == NULL ||
        xQueueReceive(free_buffer_queue, &index, 0) != pdTRUE) {
        ESP_LOGW(TAG, "No free audio buffer; this utterance will be skipped");
        skip_until_silence = true;
        return false;
    }

    active_record_buffer_index = (int)index;
    record_buffer = record_buffers[index];
    recorded_samples = 0;
    return true;
}

/** Return the currently active buffer to the free pool, if one is owned. */
void release_active_record_buffer(void)
{
    if (active_record_buffer_index >= 0 && free_buffer_queue != NULL) {
        const uint8_t index = (uint8_t)active_record_buffer_index;
        (void)xQueueSend(free_buffer_queue, &index, 0);
    }
    active_record_buffer_index = -1;
    record_buffer = NULL;
    recorded_samples = 0;
}

/** Return a sender-owned buffer to the pool after its HTTP attempt ends. */
void release_packet_buffer(uint8_t index)
{
    if (free_buffer_queue != NULL && index < AUDIO_BUFFER_COUNT) {
        (void)xQueueSend(free_buffer_queue, &index, portMAX_DELAY);
    }
}

/**
 * Begin one utterance after WakeNet detection and reset all VAD state used to
 * separate the wake phrase from the actual command.
 */
bool start_recording(void)
{
    if (is_recording) {
        return true;
    }
    if (!acquire_record_buffer()) {
        return false;
    }

    silence_frames = 0;
    command_start_tick = xTaskGetTickCount();
    command_speech_started = false;
    post_wake_silence_seen = false;
    is_recording = true;
    set_recording_indicator(true);

    ESP_LOGI(TAG, "Wake accepted; waiting for command speech");
    return true;
}

/** Discard the current utterance, used when no command follows the wake word. */
void cancel_current_recording(void)
{
    if (!is_recording) {
        return;
    }
    is_recording = false;
    silence_frames = 0;
    command_speech_started = false;
    post_wake_silence_seen = false;
    release_active_record_buffer();
    set_recording_indicator(false);
}

/**
 * Stop recording and enqueue a descriptor instead of copying the audio.
 * On a successful queue send the sender owns the buffer; on failure this
 * function returns it immediately so the pool cannot leak capacity.
 */
void finish_recording(void)
{
    if (!is_recording) {
        return;
    }

    is_recording = false;
    set_recording_indicator(false);

    if (recorded_samples == 0 || record_buffer == NULL ||
        active_record_buffer_index < 0) {
        ESP_LOGW(TAG, "Recording ended with zero samples");
        release_active_record_buffer();
        return;
    }

    audio_packet_t packet = {
        .buffer = record_buffer,
        .samples = recorded_samples,
        .buffer_index = (uint8_t)active_record_buffer_index,
    };

    if (xQueueSend(audio_queue, &packet, 0) != pdTRUE) {
        ESP_LOGW(TAG, "STT queue busy; dropping %u samples",
                 (unsigned)recorded_samples);
        release_active_record_buffer();
        return;
    }

    ESP_LOGI(TAG, "Audio queued: %u samples (%.2f s)",
             (unsigned)packet.samples,
             (double)packet.samples / AUDIO_RATE);

    /* Ownership of this buffer now belongs to the HTTP sender task. */
    active_record_buffer_index = -1;
    record_buffer = NULL;
    recorded_samples = 0;
    silence_frames = 0;
    command_speech_started = false;
    post_wake_silence_seen = false;
}

/**
 * Create queues and allocate the fixed recording-buffer pool in PSRAM.
 * One slot is reserved for the active recorder; the remaining pool/queue
 * sizing allows limited overlap between capturing and sending utterances.
 */
void init_record_buffers(void)
{
    const size_t bytes = PCM_CAPACITY * sizeof(int16_t);
    audio_queue = xQueueCreate(AUDIO_BUFFER_COUNT - 1, sizeof(audio_packet_t));
    free_buffer_queue = xQueueCreate(AUDIO_BUFFER_COUNT, sizeof(uint8_t));
    ESP_ERROR_CHECK(audio_queue ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(free_buffer_queue ? ESP_OK : ESP_ERR_NO_MEM);

    for (uint8_t i = 0; i < AUDIO_BUFFER_COUNT; ++i) {
        record_buffers[i] = heap_caps_malloc(
            bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (record_buffers[i] == NULL) {
            ESP_LOGE(TAG, "Cannot allocate audio buffer %u (%u bytes)",
                     (unsigned)i, (unsigned)bytes);
            ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
        }
        ESP_ERROR_CHECK(xQueueSend(free_buffer_queue, &i, 0) == pdTRUE
                            ? ESP_OK : ESP_FAIL);
    }
}

/** Configure the status LED as a simple active-high output. */
void init_led(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&cfg));
    set_recording_indicator(false);
}
