/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <math.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_log.h"
#include "sdkconfig.h"
#include "audio_vad.h"

static const char *TAG = "audio_vad";

typedef enum {
    VAD_STATE_SILENCE,
    VAD_STATE_SPEAKING,
} vad_state_t;

static vad_state_t s_vad_state = VAD_STATE_SILENCE;
static int s_hangover_samples_remaining = 0;

void audio_vad_reset(void)
{
    s_vad_state = VAD_STATE_SILENCE;
    s_hangover_samples_remaining = 0;
    ESP_LOGI(TAG, "VAD reset to SILENCE");
}

bool audio_vad_detect(const int16_t *pcm_data, int num_samples)
{
    /* Calculate RMS (root mean square) energy */
    double sum_sq = 0.0;
    for (int i = 0; i < num_samples; i++) {
        sum_sq += (double)pcm_data[i] * (double)pcm_data[i];
    }
    double rms = sqrt(sum_sq / num_samples);

    int threshold = CONFIG_VAD_ENERGY_THRESHOLD;
    int hangover_samples = (CONFIG_VAD_HANGOVER_MS * CONFIG_AUDIO_SAMPLE_RATE_HZ) / 1000;

    switch (s_vad_state) {
        case VAD_STATE_SILENCE:
            if (rms > threshold) {
                s_vad_state = VAD_STATE_SPEAKING;
                s_hangover_samples_remaining = hangover_samples;
                ESP_LOGD(TAG, "VAD: SILENCE -> SPEAKING (RMS: %.0f, threshold: %d)", rms, threshold);
            }
            break;

        case VAD_STATE_SPEAKING:
            if (rms > threshold) {
                /* Reset hangover: we're still speaking */
                s_hangover_samples_remaining = hangover_samples;
            } else {
                /* Count down hangover */
                s_hangover_samples_remaining -= num_samples;
                if (s_hangover_samples_remaining <= 0) {
                    s_vad_state = VAD_STATE_SILENCE;
                    s_hangover_samples_remaining = 0;
                    ESP_LOGD(TAG, "VAD: SPEAKING -> SILENCE");
                }
            }
            break;
    }

    return (s_vad_state == VAD_STATE_SPEAKING);
}
