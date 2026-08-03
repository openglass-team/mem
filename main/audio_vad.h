/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Reset the VAD state machine to SILENCE.
 */
void audio_vad_reset(void);

/**
 * @brief Detect voice activity in a PCM audio frame.
 *
 * Computes RMS energy and compares against the configured threshold.
 * Uses a hangover counter to avoid cutting off speech tails.
 *
 * @param pcm_data   16-bit PCM audio samples.
 * @param num_samples Number of samples in the frame.
 * @return true if speech is detected (state = SPEAKING), false otherwise.
 */
bool audio_vad_detect(const int16_t *pcm_data, int num_samples);

#ifdef __cplusplus
}
#endif
