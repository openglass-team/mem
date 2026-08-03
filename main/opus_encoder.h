/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the Opus encoder.
 *
 * Must be called once before opus_encode_frame().
 * Uses menuconfig parameters: sample rate, complexity, bitrate.
 */
void audio_opus_encoder_init(void);

/**
 * @brief Encode a PCM audio frame into an Opus packet.
 *
 * @param pcm_data       16-bit mono PCM samples (frame duration from menuconfig).
 * @param output         Buffer for the encoded Opus packet.
 * @param max_output_size Maximum size of the output buffer.
 * @return Number of bytes written to output on success, or -1 on error.
 */
int opus_encode_frame(const int16_t *pcm_data, uint8_t *output, int max_output_size);

#ifdef __cplusplus
}
#endif
