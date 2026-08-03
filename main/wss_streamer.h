/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * Qwen Omni Realtime WebSocket client
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Result types returned by wss_streamer_get_result_typed() */
enum {
    WSS_RESULT_ASR_DELTA = 0,    /* Real-time transcription delta 🔤 */
    WSS_RESULT_ASR_FINAL = 1,    /* Final transcription ✅ */
    WSS_RESULT_RESPONSE  = 2,    /* Model response text 💬 */
};

void wss_streamer_init(void);
void wss_streamer_send_opus_packet(const uint8_t *data, int len);
bool wss_is_connected(void);
void wss_streamer_send_task(void *pvParameters);
bool wss_streamer_get_result(char *buf, int size);
uint8_t wss_streamer_get_result_typed(char *buf, int size);

#ifdef __cplusplus
}
#endif
