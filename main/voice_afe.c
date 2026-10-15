/*
 * Module: ESP-SR audio front end and wake/VAD state machine
 *
 * afe_feed_task() reads microphone frames and supplies the optional speaker
 * reference in the AFE's interleaved "MR" format. afe_fetch_task() consumes
 * enhanced audio, reacts to WakeNet detections, captures one command, and uses
 * VAD silence/maximum duration to hand it to the recorder queue.
 *
 * The two tasks run continuously and must stay real-time friendly. Do not put
 * blocking HTTP requests, large allocations, or user-facing output here.
 */
#include "voice_aiot_internal.h"

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "VOICE_AFE";

/*
 * Return true while the current playback run still owes reference frames.
 * final_frames is -1 when idle, the planned frame count during playback, and
 * the actual number of queued frames once playback completes or aborts.
 */
static bool reference_expected(void)
{
    const int32_t final_frames = ref_stats.final_frames;
    return final_frames >= 0 &&
           ref_stats.consumed_frames < (uint32_t)final_frames;
}

/*
 * Acquire the reference that corresponds to the next microphone frame.
 * Waiting is bounded so a delayed speaker task cannot stall I2S RX long enough
 * to overflow the microphone DMA. A missing reference during playback is
 * counted and replaced with silence; this avoids feeding stale samples to AEC.
 * Faults are counted without logging in this real-time task; the speaker
 * playback caller reports the counters after a playback operation completes.
 */
static void acquire_reference(playback_ref_item_t *item, int feed_chunksize)
{
    bool got = xQueueReceive(playback_ref_queue, item, 0) == pdTRUE;

    if (!got && reference_expected()) {
        got = xQueueReceive(playback_ref_queue, item,
                            pdMS_TO_TICKS(PLAYBACK_REF_WAIT_MS)) == pdTRUE;
        if (!got) {
            ref_stats.underruns++;
            memset(item->pcm, 0, (size_t)feed_chunksize * sizeof(int16_t));
            return;
        }
    }

    if (!got) {
        /* No playback is active, so a zero-valued reference is appropriate. */
        memset(item->pcm, 0, (size_t)feed_chunksize * sizeof(int16_t));
        return;
    }

    /* Track gaps and duplicate/out-of-order frames instead of silently hiding them. */
    const uint32_t expected = ref_stats.consumed_frames;
    if (item->seq == expected) {
        ref_stats.consumed_frames = expected + 1;
    } else if (item->seq > expected) {
        ref_stats.seq_gaps += item->seq - expected;
        ref_stats.consumed_frames = item->seq + 1;
    } else if (item->seq + 1 == expected) {
        ref_stats.seq_dups++;
    } else {
        ref_stats.seq_ooo++;
    }

    /* Check that the queued samples have not changed since playback prepared them. */
    if (ref_frame_checksum(item->pcm, (size_t)feed_chunksize) != item->checksum) {
        ref_stats.checksum_errors++;
    }
}

/**
 * AFE producer task: convert one microphone frame to PCM16, pair it with the
 * matching playback reference (or silence when speaker playback is inactive),
 * then submit interleaved M/R samples to ESP-SR.
 */
void afe_feed_task(void *arg)
{
    (void)arg;

    if (afe_handle == NULL ||
        afe_data == NULL ||
        rx_chan == NULL ||
        playback_ref_queue == NULL) {
        ESP_LOGE(TAG, "AFE feed task: resource not initialized");
        vTaskDelete(NULL);
        return;
    }

    const int feed_chunksize =
        afe_handle->get_feed_chunksize(afe_data);

    const int feed_channels =
        afe_handle->get_feed_channel_num(afe_data);

    ESP_LOGI(TAG,
             "AFE feed: chunksize=%d, channels=%d",
             feed_chunksize,
             feed_channels);

    /*
     * This task requires AFE input format "MR":
     *   channel 0 = microphone (M)
     *   channel 1 = playback reference (R)
     */
    if (feed_chunksize <= 0 || feed_channels != 2) {
        ESP_LOGE(TAG,
                 "Invalid AFE input configuration: "
                 "expected 2 channels for MR");

        vTaskDelete(NULL);
        return;
    }

    if (afe_feed_chunk_samples != feed_chunksize) {
        ESP_LOGE(TAG,
                 "Reference frame size mismatch: "
                 "global=%d, AFE=%d",
                 afe_feed_chunk_samples,
                 feed_chunksize);

        vTaskDelete(NULL);
        return;
    }

    /* Buffer for 32-bit samples received from INMP441. */
    int32_t *raw32 = heap_caps_malloc(
        (size_t)feed_chunksize * sizeof(int32_t),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    /* Buffer for one queued playback-reference item (seq + checksum + mono PCM). */
    playback_ref_item_t *ref_item = heap_caps_malloc(
        PLAYBACK_REF_ITEM_BYTES(feed_chunksize),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    /*
     * Interleaved AFE input:
     * M0, R0, M1, R1, ...
     */
    int16_t *feed = heap_caps_malloc(
        (size_t)feed_chunksize *
            (size_t)feed_channels *
            sizeof(int16_t),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    if (raw32 == NULL ||
        ref_item == NULL ||
        feed == NULL) {

        ESP_LOGE(TAG, "Failed to allocate AFE feed buffers");

        free(raw32);
        free(ref_item);
        free(feed);

        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "AFE feed task started: input format MR");

    while (true) {
        /*
         * 1. Read exactly one complete microphone frame.
         *
         * i2s_channel_read() normally returns the requested bytes,
         * but accumulate partial reads rather than dropping samples.
         */
        size_t samples_collected = 0;

        while (samples_collected < (size_t)feed_chunksize) {
            const size_t samples_remaining =
                (size_t)feed_chunksize - samples_collected;

            size_t bytes_read = 0;

            esp_err_t err = i2s_channel_read(
                rx_chan,
                &raw32[samples_collected],
                samples_remaining * sizeof(int32_t),
                &bytes_read,
                portMAX_DELAY);

            if (err != ESP_OK) {
                ESP_LOGW(TAG,
                         "I2S microphone read failed: %s",
                         esp_err_to_name(err));

                vTaskDelay(pdMS_TO_TICKS(1));
                continue;
            }

            if (bytes_read == 0) {
                continue;
            }

            const size_t samples_read =
                bytes_read / sizeof(int32_t);

            if (samples_read == 0) {
                continue;
            }

            if (samples_read > samples_remaining) {
                ESP_LOGE(TAG, "Unexpected I2S read size");
                samples_collected = 0;
                break;
            }

            samples_collected += samples_read;
        }

        if (samples_collected != (size_t)feed_chunksize) {
            continue;
        }

        /* 2. Match the next microphone frame with its playback reference. */
        acquire_reference(ref_item, feed_chunksize);
        const int16_t *reference = ref_item->pcm;

        /*
         * 3. Convert microphone PCM and interleave M/R.
         *
         * AFE format "MR":
         * feed[0] = M0
         * feed[1] = R0
         * feed[2] = M1
         * feed[3] = R1
         * ...
         */
        for (int i = 0; i < feed_chunksize; ++i) {
            feed[2 * i] =
                inmp441_32_to_pcm16(raw32[i]);

            feed[2 * i + 1] =
                reference[i];
        }

        /* 4. Submit one complete microphone/reference frame to ESP-SR AFE. */
        afe_handle->feed(afe_data, feed);
    }

    /* This loop is intended to run forever. */
    free(raw32);
    free(ref_item);
    free(feed);
}

/**
 * AFE consumer/state-machine task.
 * State transitions are intentionally one-command-per-wake-word:
 *   idle -> WakeNet detected -> wait for command speech -> record -> finish
 *   (VAD silence or duration limit) -> idle.
 */
void afe_fetch_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "AFE fetch task started; say \"Hi ESP\" before each command");

    while (true) {
        afe_fetch_result_t *res = afe_handle->fetch(afe_data);
        if (!res) {
            ESP_LOGE(TAG, "AFE fetch returned NULL");
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (res->ret_value == ESP_FAIL) {
            ESP_LOGE(TAG, "AFE fetch failed");
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* Use the AFE's VAD state rather than a second software energy detector. */
        const bool vad_speech = (res->vad_state == VAD_SPEECH);

        /* If a prior utterance could not acquire a buffer, wait for silence. */
        if (skip_until_silence) {
            if (!vad_speech) {
                skip_until_silence = false;
            }
            continue;
        }

        /* Every new recording requires a fresh WakeNet detection. */
        if (!is_recording && res->wakeup_state == WAKENET_DETECTED) {
            ESP_LOGI(TAG, "WAKE WORD DETECTED: index=%d model=%d",
                     res->wake_word_index, res->wakenet_model_index);
            (void)start_recording();
            continue;
        }

        if (!is_recording) {
            continue;
        }

        const TickType_t elapsed_ticks = xTaskGetTickCount() - command_start_tick;
        const TickType_t wake_guard_ticks = pdMS_TO_TICKS(COMMAND_WAKE_GUARD_MS);
        const TickType_t start_timeout_ticks = pdMS_TO_TICKS(COMMAND_START_TIMEOUT_MS);

        /*
         * Exclude the wake phrase: WakeNet can trigger while it is still present
         * in AFE output. Wait for trailing silence or the short guard interval,
         * then seed the command with vad_cache if ESP-SR provides it.
         */
        if (!command_speech_started) {
            if (!vad_speech) {
                post_wake_silence_seen = true;
            } else if (post_wake_silence_seen || elapsed_ticks >= wake_guard_ticks) {
                command_speech_started = true;
                if (res->vad_cache && res->vad_cache_size > 0) {
                    append_samples(res->vad_cache,
                                   (size_t)res->vad_cache_size / sizeof(int16_t));
                }
                ESP_LOGI(TAG, "Command speech detected after wake word");
            }

            if (!command_speech_started) {
                if (elapsed_ticks >= start_timeout_ticks) {
                    ESP_LOGW(TAG, "No command after wake word; returning to wake-word standby");
                    cancel_current_recording();
                }
                continue;
            }
        }

        if (res->data && res->data_size > 0) {
            append_samples(res->data,
                           (size_t)res->data_size / sizeof(int16_t));
        }

        /* Convert configured silence milliseconds to a whole number of AFE
         * fetch frames; finish only after consecutive non-speech frames. */
        if (vad_speech) {
            silence_frames = 0;
        } else {
            const int fetch_samples = afe_handle->get_fetch_chunksize(afe_data);
            const int silence_samples = (AUDIO_RATE * SILENCE_MS) / 1000;
            int frames_needed = (silence_samples + fetch_samples - 1) / fetch_samples;
            if (frames_needed < 1) {
                frames_needed = 1;
            }

            silence_frames++;
            if (silence_frames >= frames_needed) {
                ESP_LOGI(TAG, "VAD silence timeout; enqueueing utterance for STT");
                finish_recording();
            }
        }

        if (recorded_samples >= PCM_CAPACITY) {
            ESP_LOGW(TAG, "Maximum utterance duration reached");
            finish_recording();
        }
    }
}


/**
 * Load the models from the dedicated flash partition and construct the AFE.
 * The input format is "MR" even when no audio is playing; in that case the
 * reference channel is zero-filled. AEC only has meaningful playback echo to
 * cancel when the actual speaker signal is supplied on R with correct timing.
 */
void afe_init(void)
{
    sr_models = esp_srmodel_init("model");
    ESP_ERROR_CHECK(sr_models ? ESP_OK : ESP_ERR_NOT_FOUND);

    /* "MR": microphone channel followed by playback-reference channel. */
    afe_config_t *config = afe_config_init(
        "MR",
        sr_models,
        AFE_TYPE_SR,
        AFE_MODE_HIGH_PERF
    );

    ESP_ERROR_CHECK(
        config != NULL ? ESP_OK : ESP_ERR_NO_MEM
    );

    /* Production speech pipeline: high-performance AEC, WebRTC NS, VAD and WakeNet. */
    config->ns_init = true;
    config->afe_ns_mode = AFE_NS_MODE_WEBRTC;
    config->aec_init = true;
    config->aec_mode = AEC_MODE_SR_HIGH_PERF;
    config->aec_filter_length = APP_AEC_FILTER_LENGTH;

    config->pcm_config.sample_rate = AUDIO_RATE;

    config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    config->afe_perferred_core = 1;
    config->afe_perferred_priority = 5;

    /* WakeNet detects the activation phrase; VAD segments the command speech. */
    config->wakenet_init = true;
    config->vad_init = true;

    config->vad_min_speech_ms = CONFIG_APP_VAD_MIN_SPEECH_MS;
    config->vad_min_noise_ms = CONFIG_APP_VAD_MIN_NOISE_MS;
    config->vad_delay_ms = CONFIG_APP_VAD_DELAY_MS;
    config->vad_mode = VAD_MODE_1;

    /*
     * The model is selected through menuconfig and loaded from the "model"
     * partition. Explicitly select the configured WakeNet model here.
     */
    if (config->wakenet_init) {
        char *wakenet_name = esp_srmodel_filter(
            sr_models, ESP_WN_PREFIX, NULL);

        if (!wakenet_name) {
            ESP_LOGE(TAG,
                     "No WakeNet model found. Enable a WakeNet model in "
                     "menuconfig -> ESP Speech Recognition.");
            afe_config_free(config);
            ESP_ERROR_CHECK(ESP_ERR_NOT_FOUND);
        }

        config->wakenet_model_name = wakenet_name;
    }

    /* AEC uses the playback-reference channel supplied by afe_feed_task(). */

    afe_config_print(config);

    afe_handle = esp_afe_handle_from_config(config);
    ESP_ERROR_CHECK(afe_handle ? ESP_OK : ESP_FAIL);

    afe_data = afe_handle->create_from_config(config);
    ESP_ERROR_CHECK(afe_data ? ESP_OK : ESP_FAIL);

    afe_config_free(config);

    ESP_LOGI(TAG, "AFE ready: AEC=SR_HIGH_PERF, NS=WebRTC, VAD=enabled, WakeNet=enabled");
    if (afe_handle->print_pipeline != NULL) {
        afe_handle->print_pipeline(afe_data);
    }
}
