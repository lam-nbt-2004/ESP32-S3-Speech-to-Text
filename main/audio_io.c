/*
 * Module: I2S audio hardware abstraction
 *
 * Keeps pin/clock/slot configuration and sample-format conversion separate
 * from AFE and recording policy. RX is the INMP441 microphone; TX supports
 * speaker playback paired with the same PCM on the AFE reference channel.
 */
#include "voice_aiot_internal.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "speaker.h"

static const char *TAG = "AUDIO_IO";

/**
 * Set up I2S0 for 32-bit INMP441 samples.
 * The AFE pipeline consumes signed PCM16 at AUDIO_RATE, so conversion is done
 * at the boundary between this driver-facing module and voice_afe.c.
 */
esp_err_t init_i2s_microphone(void)
{
    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);

    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = I2S_READ_SAMPLES;

    ESP_RETURN_ON_ERROR(
        i2s_new_channel(&chan_cfg, NULL, &rx_chan),
        TAG, "i2s_new_channel failed");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MIC_BCLK_GPIO,
            .ws   = MIC_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din  = MIC_DOUT_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    /*
     * INMP441 boards normally select LEFT or RIGHT with their L/R pin.
     * The uploaded project uses LEFT; change CONFIG_APP_MIC_SLOT if your
     * module is wired to RIGHT.
     */
    #if CONFIG_APP_MIC_SLOT_RIGHT
        std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_RIGHT;
    #else
        std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    #endif

    ESP_RETURN_ON_ERROR(
        i2s_channel_init_std_mode(rx_chan, &std_cfg),
        TAG, "i2s_channel_init_std_mode failed");

    ESP_RETURN_ON_ERROR(
        i2s_channel_enable(rx_chan),
        TAG, "i2s_channel_enable failed");

    ESP_LOGI(TAG, "I2S microphone ready: %d Hz, 32-bit input -> PCM16",
             AUDIO_RATE);
    return ESP_OK;
}

/**
 * Set up I2S1 for the MAX98357A speaker output.
 * The matching mono PCM frame is put on playback_ref_queue by
 * speaker_play_pcm16(), allowing the AFE feed task to supply the same signal
 * as its reference input. Correct AEC still depends on timing/alignment.
 */
esp_err_t init_i2s_speaker(void)
{
    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);

    /* Keep TX latency low so the AEC reference stays close to the sound
     * reaching the microphone. One AFE-sized frame may span several DMA blocks.
     */
    chan_cfg.dma_desc_num = 2;
    chan_cfg.dma_frame_num = 128;

    ESP_RETURN_ON_ERROR(
        i2s_new_channel(&chan_cfg, &tx_chan, NULL),
        TAG, "i2s_new_channel TX failed");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_RATE),

        /*
         * Stereo 16-bit.
         * Playback sends the same signal to both L/R slots, so MAX98357A
         * channel selection does not matter.
         */
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT,
            I2S_SLOT_MODE_STEREO),

        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = SPK_BCLK_GPIO,
            .ws   = SPK_WS_GPIO,
            .dout = SPK_DOUT_GPIO,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    ESP_RETURN_ON_ERROR(
        i2s_channel_init_std_mode(tx_chan, &std_cfg),
        TAG, "i2s_channel_init_std_mode TX failed");

    ESP_RETURN_ON_ERROR(
        i2s_channel_enable(tx_chan),
        TAG, "i2s_channel_enable TX failed");

    ESP_LOGI(TAG,
             "I2S speaker ready: %d Hz, 16-bit stereo -> MAX98357A",
             AUDIO_RATE);

    return ESP_OK;
}

/**
 * Convert the significant 16-bit portion of one INMP441 I2S word to PCM16.
 * The microphone driver is configured for 32-bit slots, while ESP-SR's AFE
 * feed format in this application is 16-bit signed PCM.
 */
int16_t inmp441_32_to_pcm16(int32_t raw)
{
    int32_t sample = raw >> 16;

    if (sample > INT16_MAX) {
        sample = INT16_MAX;
    } else if (sample < INT16_MIN) {
        sample = INT16_MIN;
    }

    return (int16_t)sample;
}

/**
 * Fletcher-style checksum of one reference frame. Computed by the playback
 * task when the frame is enqueued and re-computed by the feed task when it is
 * dequeued, so any change of the payload in between is detected.
 */
uint32_t ref_frame_checksum(const int16_t *pcm, size_t samples)
{
    uint32_t a = 1;
    uint32_t b = 0;

    for (size_t i = 0; i < samples; ++i) {
        a = (a + (uint16_t)pcm[i]) % 65521U;
        b = (b + a) % 65521U;
    }
    return (b << 16) | a;
}

/** Clear reference-path counters (call before a playback run starts). */
void ref_stats_reset(void)
{
    ref_stats.enq_frames = 0;
    ref_stats.final_frames = -1;
    ref_stats.enq_timeouts = 0;
    ref_stats.tx_errors = 0;
    ref_stats.consumed_frames = 0;
    ref_stats.underruns = 0;
    ref_stats.seq_gaps = 0;
    ref_stats.seq_dups = 0;
    ref_stats.seq_ooo = 0;
    ref_stats.checksum_errors = 0;
}

/*
 * Fill one reference queue item with frame `frame` of the source. The last
 * partial frame is padded with silence. Returns the number of source samples
 * that went into this frame.
 */
static size_t prepare_ref_item(playback_ref_item_t *item,
                               const uint8_t *source_bytes,
                               size_t sample_count,
                               size_t frame,
                               int frame_samples)
{
    const size_t first = frame * (size_t)frame_samples;
    const size_t remaining = sample_count - first;
    const size_t copy_samples =
        remaining < (size_t)frame_samples ? remaining : (size_t)frame_samples;

    /* Source may be unaligned (hence memcpy) and may live in PSRAM. */
    memcpy(item->pcm,
           source_bytes + first * sizeof(int16_t),
           copy_samples * sizeof(int16_t));
    if (copy_samples < (size_t)frame_samples) {
        memset(item->pcm + copy_samples, 0,
               ((size_t)frame_samples - copy_samples) * sizeof(int16_t));
    }

    item->seq = (uint32_t)frame;
    item->checksum = ref_frame_checksum(item->pcm, (size_t)frame_samples);
    return copy_samples;
}

/**
 * Play mono PCM16 as stereo and queue each corresponding mono frame as the
 * AEC reference. Call this synchronous helper from a separate task because
 * I2S writes and queue waits can block.
 *
 * Reference timing: frame k+1 is enqueued BEFORE frame k is written to the
 * I2S TX DMA (frame 0 and 1 are both queued before the first write). This:
 *   - keeps the reference available at least one AFE frame before the feed
 *     task needs it, so the phase between the TX loop and the RX/feed loop
 *     cannot cause underruns;
 *   - avoids the earlier write-then-enqueue order, which could deliver the
 *     reference to the AFE after the speaker had already started that frame.
 * Consumption is one frame per feed iteration (FIFO). The exact acoustic
 * delay still depends on I2S DMA and the physical speaker/microphone placement.
 *
 * The TX stereo samples and the queued reference come from one and the same
 * mono buffer (item->pcm); nothing is scaled or resampled.
 */
esp_err_t speaker_play_pcm16(const void *mono_pcm16,
                             size_t sample_count,
                             uint32_t sample_rate)
{
    if (mono_pcm16 == NULL || sample_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (sample_rate != AUDIO_RATE) {
        ESP_LOGE(TAG,
                 "Speaker expects %u Hz PCM16, got %u Hz",
                 AUDIO_RATE,
                 (unsigned)sample_rate);
        return ESP_ERR_INVALID_ARG;
    }

    if (tx_chan == NULL ||
        playback_ref_queue == NULL ||
        afe_feed_chunk_samples <= 0 ||
        speaker_playback_mutex == NULL) {
        ESP_LOGE(TAG, "Speaker playback is not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(speaker_playback_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }

    const int frame_samples = afe_feed_chunk_samples;
    const uint8_t *source_bytes = (const uint8_t *)mono_pcm16;
    const size_t stereo_bytes = (size_t)frame_samples * 2U * sizeof(int16_t);

    playback_ref_item_t *item = heap_caps_malloc(
        PLAYBACK_REF_ITEM_BYTES(frame_samples),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    int16_t *stereo = heap_caps_malloc(
        stereo_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    if (item == NULL || stereo == NULL) {
        ESP_LOGE(TAG, "Cannot allocate speaker playback buffers");
        free(item);
        free(stereo);
        xSemaphoreGive(speaker_playback_mutex);
        return ESP_ERR_NO_MEM;
    }

    const size_t frame_count =
        (sample_count + (size_t)frame_samples - 1U) / (size_t)frame_samples;
    size_t source_sample = 0;
    esp_err_t result = ESP_OK;
    uint32_t enqueued = 0;

    ESP_LOGI(TAG,
             "Speaker playback started: %.2f s, %u Hz mono PCM16, %u frames of %d samples",
             (double)sample_count / (double)sample_rate,
             (unsigned)sample_rate,
             (unsigned)frame_count,
             frame_samples);

    /*
     * Empty queue, zeroed counters, then publish the expected total BEFORE the
     * first frame exists: the feed task treats "consumed < final_frames with an
     * empty queue" as an underrun, so a late frame is always detected.
     */
    xQueueReset(playback_ref_queue);
    ref_stats_reset();
    ref_stats.final_frames = (int32_t)frame_count;

    /* Queue frame 0 now; the loop queues frame k+1 before writing frame k. */
    (void)prepare_ref_item(item, source_bytes, sample_count, 0, frame_samples);
    if (xQueueSend(playback_ref_queue, item, pdMS_TO_TICKS(200)) != pdTRUE) {
        ESP_LOGE(TAG, "Cannot enqueue first reference frame");
        ref_stats.enq_timeouts++;
        result = ESP_ERR_TIMEOUT;
    } else {
        enqueued = 1;
        ref_stats.enq_frames = enqueued;
    }

    for (size_t frame = 0; result == ESP_OK && frame < frame_count; ++frame) {
        /* TX copy of frame k: item->pcm still holds exactly frame k here. */
        for (int i = 0; i < frame_samples; ++i) {
            stereo[2 * i] = item->pcm[i];
            stereo[2 * i + 1] = item->pcm[i];
        }
        const size_t first = frame * (size_t)frame_samples;
        const size_t remaining = sample_count - first;
        const size_t this_samples =
            remaining < (size_t)frame_samples ? remaining : (size_t)frame_samples;

        /* Reference for frame k+1, one frame ahead of the TX write. */
        if (frame + 1 < frame_count) {
            (void)prepare_ref_item(item, source_bytes, sample_count,
                                   frame + 1, frame_samples);
            if (xQueueSend(playback_ref_queue, item, pdMS_TO_TICKS(200)) != pdTRUE) {
                ESP_LOGE(TAG, "Reference queue full for 200 ms at frame %u; aborting",
                         (unsigned)(frame + 1));
                ref_stats.enq_timeouts++;
                result = ESP_ERR_TIMEOUT;
                break;
            }
            enqueued++;
            ref_stats.enq_frames = enqueued;
        }

        size_t bytes_written = 0;
        esp_err_t err = i2s_channel_write(
            tx_chan, stereo, stereo_bytes, &bytes_written,
            500U); /* i2s_channel_write timeout is milliseconds, not ticks. */

        if (err != ESP_OK || bytes_written != stereo_bytes) {
            ESP_LOGE(TAG,
                     "Speaker I2S write failed: %s, wrote %u/%u bytes",
                     esp_err_to_name(err),
                     (unsigned)bytes_written,
                     (unsigned)stereo_bytes);
            ref_stats.tx_errors++;
            result = (err != ESP_OK) ? err : ESP_FAIL;
            break;
        }

        source_sample += this_samples;
    }

    /*
     * Flush one frame of silence so the TX DMA does not keep repeating the
     * last audio block after playback ends. Not mirrored on R: after the final
     * reference frame the feed task supplies zeros, which matches this silence.
     */
    memset(stereo, 0, stereo_bytes);
    size_t flushed = 0;
    (void)i2s_channel_write(tx_chan, stereo, stereo_bytes, &flushed, 500U);

    /* Publish the actual count (smaller than planned after an abort). */
    ref_stats.final_frames = (int32_t)enqueued;

    ESP_LOGI(TAG,
             "Speaker playback %s (%u/%u samples, %u frames enqueued)",
             result == ESP_OK ? "finished" : "stopped after an error",
             (unsigned)source_sample,
             (unsigned)sample_count,
             (unsigned)enqueued);
    ESP_LOGI(TAG,
             "AEC reference status: consumed=%u, underruns=%u, gaps=%u, "
             "duplicates=%u, out_of_order=%u, checksum_errors=%u, tx_errors=%u",
             (unsigned)ref_stats.consumed_frames,
             (unsigned)ref_stats.underruns,
             (unsigned)ref_stats.seq_gaps,
             (unsigned)ref_stats.seq_dups,
             (unsigned)ref_stats.seq_ooo,
             (unsigned)ref_stats.checksum_errors,
             (unsigned)ref_stats.tx_errors);

    free(item);
    free(stereo);
    xSemaphoreGive(speaker_playback_mutex);
    return result;
}
