/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#pragma once

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the I2S PDM RX channel for microphone input.
 *
 * Configures 16kHz / 16-bit / Mono PDM reception with hardware PCM conversion.
 * Creates a stream buffer for passing PCM frames between tasks.
 * Must be called once before i2s_mic_start_read().
 */
void i2s_mic_init(void);

/**
 * @brief Continuous microphone read task.
 *
 * Reads PCM audio from the I2S PDM RX channel and writes frames
 * into a FreeRTOS Stream Buffer for downstream processing.
 *
 * @param pvParameters  Task parameters (unused).
 */
void i2s_mic_start_read(void *pvParameters);

/**
 * @brief Get the PCM stream buffer handle.
 *
 * Used by the VAD/Encoder task to read audio frames.
 * @return StreamBufferHandle_t or NULL if not initialized.
 */
StreamBufferHandle_t i2s_mic_get_stream_buffer(void);

#ifdef __cplusplus
}
#endif
