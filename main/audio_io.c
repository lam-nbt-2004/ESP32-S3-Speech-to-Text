#include "audio_io.h"
#include "board_config.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include <stdbool.h>

#define AUDIO_BLOCK_SAMPLES 480
#define SPEAKER_VOLUME_PERCENT 65
#define SILENCE_BLOCK_SAMPLES 240
#define SILENCE_BLOCK_COUNT 8

static i2s_chan_handle_t s_rx;
static i2s_chan_handle_t s_tx;
static bool s_tx_enabled;
/* Keep the stereo conversion buffer out of the small main-task stack. */
static int16_t s_tx_stereo[AUDIO_BLOCK_SAMPLES * 2];

esp_err_t audio_io_init(uint32_t speaker_sample_rate)
{
    i2s_chan_config_t rx_ch = I2S_CHANNEL_DEFAULT_CONFIG(VA_MIC_I2S, I2S_ROLE_MASTER);
    rx_ch.dma_desc_num = 6;
    rx_ch.dma_frame_num = 240;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&rx_ch, NULL, &s_rx), "audio", "I2S RX channel");

    i2s_chan_config_t tx_ch = I2S_CHANNEL_DEFAULT_CONFIG(VA_SPK_I2S, I2S_ROLE_MASTER);
    tx_ch.dma_desc_num = 6;
    tx_ch.dma_frame_num = 240;
    /* Send silence on underrun instead of replaying stale voice samples. */
    tx_ch.auto_clear_after_cb = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&tx_ch, &s_tx, NULL), "audio", "I2S TX channel");

    i2s_std_config_t rx_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(VA_MIC_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = { .mclk = I2S_GPIO_UNUSED, .bclk = VA_MIC_BCLK_GPIO, .ws = VA_MIC_WS_GPIO,
                      .dout = I2S_GPIO_UNUSED, .din = VA_MIC_DOUT_GPIO,
                      .invert_flags = { false, false, false } },
    };
    rx_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx, &rx_cfg), "audio", "I2S RX init");

    i2s_std_config_t tx_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(speaker_sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = { .mclk = I2S_GPIO_UNUSED, .bclk = VA_AMP_BCLK_GPIO, .ws = VA_AMP_WS_GPIO,
                      .dout = VA_AMP_DIN_GPIO, .din = I2S_GPIO_UNUSED,
                      .invert_flags = { false, false, false } },
    };
    tx_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &tx_cfg), "audio", "I2S TX init");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx), "audio", "I2S RX enable");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), "audio", "I2S TX enable");
    s_tx_enabled = true;
    return ESP_OK;
}

esp_err_t audio_io_read_mic(int16_t *dst, size_t count, size_t *read_count, uint32_t timeout_ms)
{
    static int32_t raw[AUDIO_BLOCK_SAMPLES];
    if (!dst || !read_count) return ESP_ERR_INVALID_ARG;
    *read_count = 0;
    if (count > AUDIO_BLOCK_SAMPLES) return ESP_ERR_INVALID_SIZE;
    size_t bytes = 0;
    /* ESP-IDF I2S APIs take timeout in milliseconds, not FreeRTOS ticks. */
    esp_err_t err = i2s_channel_read(s_rx, raw, count * sizeof(raw[0]), &bytes, timeout_ms);
    if (err != ESP_OK) return err;
    size_t n = bytes / sizeof(raw[0]);
    for (size_t i = 0; i < n; ++i) {
        /* INMP441: signed 24-bit data left-aligned in 32-bit I2S slot. */
        dst[i] = (int16_t)(raw[i] >> 16);
    }
    *read_count = n;
    return ESP_OK;
}

esp_err_t audio_io_write_speaker(const int16_t *pcm, size_t count, uint32_t timeout_ms)
{
    if (!pcm && count) return ESP_ERR_INVALID_ARG;
    if (!s_tx_enabled) {
        esp_err_t enable_err = i2s_channel_enable(s_tx);
        if (enable_err != ESP_OK) return enable_err;
        s_tx_enabled = true;
    }

    /* MAX98357A consumes stereo I2S slots. Duplicate mono PCM into L/R. */
    while (count) {
        size_t frames = count > AUDIO_BLOCK_SAMPLES ? AUDIO_BLOCK_SAMPLES : count;
        for (size_t i = 0; i < frames; ++i) {
            /* Keep voice peaks below MAX98357A/speaker clipping at full scale. */
            int16_t sample = (int16_t)(((int32_t)pcm[i] * SPEAKER_VOLUME_PERCENT) / 100);
            s_tx_stereo[2 * i] = sample;
            s_tx_stereo[2 * i + 1] = sample;
        }
        size_t expected = frames * 2 * sizeof(s_tx_stereo[0]);
        size_t written = 0;
        esp_err_t err = i2s_channel_write(s_tx, s_tx_stereo, expected, &written, timeout_ms);
        if (err != ESP_OK) return err;
        if (written != expected) return ESP_ERR_TIMEOUT;
        pcm += frames;
        count -= frames;
    }
    return ESP_OK;
}

void audio_io_mute_speaker(void)
{
    static const int16_t silence[SILENCE_BLOCK_SAMPLES] = {0};
    /* Flush queued audio with silence (120 ms at 16 kHz), then stop BCLK/WS. */
    for (int i = 0; i < SILENCE_BLOCK_COUNT; ++i) {
        if (audio_io_write_speaker(silence, SILENCE_BLOCK_SAMPLES, 100) != ESP_OK) break;
    }
    if (s_tx_enabled && i2s_channel_disable(s_tx) == ESP_OK) s_tx_enabled = false;
}
