/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#pragma once

#include "sdkconfig.h"

#define EXAMPLE_BUFF_SIZE   2048

/* Audio frame configuration: 16-bit = 2 bytes per sample */
#define AUDIO_SAMPLE_BYTES      2

/*
 * Calculate frame size based on menuconfig settings.
 * 20ms @ 16kHz mono = 320 samples = 640 bytes
 */
static inline int audio_frame_samples(void) {
    return (CONFIG_AUDIO_SAMPLE_RATE_HZ * CONFIG_AUDIO_FRAME_DURATION_MS) / 1000;
}

static inline int audio_frame_bytes(void) {
    return audio_frame_samples() * AUDIO_SAMPLE_BYTES;
}

/* Forward declarations */
void i2s_mic_init(void);
void i2s_mic_start_read(void *pvParameters);
void wifi_init_sta(void);
bool wifi_is_connected(void);
void audio_opus_encoder_init(void);
int opus_encode_frame(const int16_t *pcm_data, uint8_t *output, int max_output_size);
void wss_streamer_init(void);
void wss_streamer_send_opus_packet(const uint8_t *data, int len);
bool wss_is_connected(void);
void audio_vad_reset(void);
bool audio_vad_detect(const int16_t *pcm_data, int num_samples);
