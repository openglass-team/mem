/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <string.h>
#include "esp_log.h"
#include "sdkconfig.h"
#include "opus_encoder.h"
#include "opus.h"

static const char *TAG = "opus_encoder";

static OpusEncoder *s_opus_enc = NULL;
static int s_frame_samples = 0;

void audio_opus_encoder_init(void)
{
    int err = 0;
    int sample_rate = CONFIG_AUDIO_SAMPLE_RATE_HZ;

    s_frame_samples = (sample_rate * CONFIG_AUDIO_FRAME_DURATION_MS) / 1000;

    /* Create Opus encoder: sample_rate, channels=1, application=VOIP */
    s_opus_enc = opus_encoder_create(sample_rate, 1, OPUS_APPLICATION_VOIP, &err);
    if (err != OPUS_OK) {
        ESP_LOGE(TAG, "opus_encoder_create failed: %d", err);
        return;
    }

    /* Configure encoder parameters */
    opus_encoder_ctl(s_opus_enc, OPUS_SET_BITRATE(CONFIG_OPUS_BITRATE_BPS));
    opus_encoder_ctl(s_opus_enc, OPUS_SET_COMPLEXITY(CONFIG_OPUS_COMPLEXITY));
    opus_encoder_ctl(s_opus_enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    /* Enable FEC (Forward Error Correction) for packet loss resilience */
    opus_encoder_ctl(s_opus_enc, OPUS_SET_INBAND_FEC(1));

    ESP_LOGI(TAG, "Opus encoder ready: %d Hz, %d ms frames (%d samples), %d bps, complexity %d",
             sample_rate, CONFIG_AUDIO_FRAME_DURATION_MS, s_frame_samples,
             CONFIG_OPUS_BITRATE_BPS, CONFIG_OPUS_COMPLEXITY);
}

int opus_encode_frame(const int16_t *pcm_data, uint8_t *output, int max_output_size)
{
    if (s_opus_enc == NULL) {
        return -1;
    }

    int len = opus_encode(s_opus_enc, pcm_data, s_frame_samples,
                          output, max_output_size);
    if (len < 0) {
        ESP_LOGW(TAG, "opus_encode error: %d", len);
        return -1;
    }
    return len;
}
