#pragma once
#include "driver/gpio.h"
#include "driver/i2s_types.h"

/* Preserve confirmed wiring from the existing recorder project. */
#define VA_MIC_BCLK_GPIO GPIO_NUM_14
#define VA_MIC_WS_GPIO GPIO_NUM_15
#define VA_MIC_DOUT_GPIO GPIO_NUM_16
#define VA_AMP_BCLK_GPIO GPIO_NUM_17
#define VA_AMP_WS_GPIO GPIO_NUM_18
#define VA_AMP_DIN_GPIO GPIO_NUM_8

#define VA_MIC_SAMPLE_RATE 16000
#define VA_SPK_SAMPLE_RATE 16000
#define VA_OPUS_FRAME_MS 60
#define VA_OPUS_FRAME_SAMPLES ((VA_MIC_SAMPLE_RATE * VA_OPUS_FRAME_MS) / 1000)
#define VA_MIC_I2S I2S_NUM_0
#define VA_SPK_I2S I2S_NUM_1
