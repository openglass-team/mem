/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 *
 * I2S PDM Microphone → WebSocket Cloud Streaming (Translation Mode)
 *
 * Pipeline:
 *   PDM Mic → I2S RX → Stream Buffer → Send Queue → WSS → Cloud (Qwen Omni)
 *   Server-side VAD handles speech detection (no local VAD needed)
 *   Recognized speech in any language → translated to Chinese (简体中文)
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "i2s_pdm_example.h"
#include "i2s_pdm_mic.h"
#include "wifi_sta.h"
#include "opus_encoder.h"
#include "wss_streamer.h"

static const char *TAG = "app_main";

/*
 * PCM streaming task:
 * Reads PCM frames from the stream buffer and queues them for WebSocket sending.
 * NO local VAD — all frames (including silence) are sent to the server,
 * so the server-side VAD can detect speech start/end properly.
 */
static void pcm_stream_task(void *pvParameters)
{
    int frame_samples = audio_frame_samples();
    int frame_bytes = audio_frame_bytes();
    StreamBufferHandle_t stream_buf = i2s_mic_get_stream_buffer();

    /* Allocate PCM frame buffer */
    int16_t *pcm_frame = (int16_t *)calloc(1, frame_bytes);
    assert(pcm_frame);

    ESP_LOGI(TAG, "PCM stream task started. Frame: %d samples, %d bytes",
             frame_samples, frame_bytes);

    int debug_count = 0;
    while (1) {
        size_t received = xStreamBufferReceive(stream_buf,
                                               (void *)pcm_frame,
                                               frame_bytes,
                                               pdMS_TO_TICKS(100));
        if (received < frame_bytes) {
            static int underrun_count = 0;
            if (++underrun_count % 50 == 1) {
                ESP_LOGW(TAG, "Stream buffer underrun x%d: %d/%d bytes",
                         underrun_count, (int)received, frame_bytes);
            }
            continue;
        }

        /* Print first 3 frames for debugging PCM data */
        if (debug_count < 3) {
            ESP_LOGI(TAG, "PCM[%d]: %d %d %d %d %d ... %d %d",
                     debug_count,
                     pcm_frame[0], pcm_frame[1], pcm_frame[2], pcm_frame[3],
                     pcm_frame[4], pcm_frame[318], pcm_frame[319]);
            debug_count++;
        }

        /* Send ALL frames (including silence) — server VAD needs continuous audio */
        wss_streamer_send_opus_packet((const uint8_t *)pcm_frame, frame_bytes);
    }

    free(pcm_frame);
    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "PDM Mic -> Opus -> WebSocket Streaming");
    ESP_LOGI(TAG, "========================================");

    /* Step 1: Connect Wi-Fi */
    ESP_LOGI(TAG, "[1/5] Connecting Wi-Fi...");
    wifi_init_sta();
    if (!wifi_is_connected()) {
        ESP_LOGE(TAG, "Wi-Fi connection failed. Aborting.");
        return;
    }

    /* Step 2: Initialize I2S PDM microphone (stream buffer) */
    ESP_LOGI(TAG, "[2/5] Initializing I2S PDM microphone...");
    i2s_mic_init();

    /* Step 3: Initialize Opus encoder */
    ESP_LOGI(TAG, "[3/5] Initializing Opus encoder...");
    audio_opus_encoder_init();

    /* Step 4: Initialize WebSocket client */
    ESP_LOGI(TAG, "[4/5] Connecting WebSocket...");
    wss_streamer_init();

    /* Wait a moment for WSS to establish */
    int wait_count = 0;
    while (!wss_is_connected() && wait_count < 50) {
        vTaskDelay(pdMS_TO_TICKS(200));
        wait_count++;
    }
    if (!wss_is_connected()) {
        ESP_LOGW(TAG, "WebSocket not connected yet, will retry in background");
    }

    /* Step 5: Start processing pipeline */
    ESP_LOGI(TAG, "[5/5] Starting audio pipeline tasks...");

    /* Task A: WSS sender (consumes the send queue) */
    xTaskCreate(wss_streamer_send_task, "wss_sender", 16384, NULL, 5, NULL);

    /* Task B: PCM streamer (consumes PCM stream buffer, sends to WSS queue) */
    xTaskCreate(pcm_stream_task, "pcm_stream", 8192, NULL, 6, NULL);

    /* Task C: I2S PDM microphone reader (highest priority to avoid DMA overflow) */
    xTaskCreate(i2s_mic_start_read, "i2s_mic_read", 4096, NULL, 8, NULL);

    ESP_LOGI(TAG, "Audio streaming pipeline started!");
    ESP_LOGI(TAG, "Speak into the microphone to send Opus audio to: %s", CONFIG_WSS_URL);

    /* Main task: monitor status. ASR results are uploaded to cloud automatically
     * by wss_event_handler when final transcription is received. */
    char asr_buf[256];
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(100));

        /* Process ASR and response results */
        while (1) {
            uint8_t rtype = wss_streamer_get_result_typed(asr_buf, sizeof(asr_buf));
            if (rtype == 0xFF) break;
            switch (rtype) {
            case WSS_RESULT_ASR_DELTA:
                ESP_LOGI(TAG, "🔤 原文: \"%s\"", asr_buf);
                break;
            case WSS_RESULT_ASR_FINAL:
                ESP_LOGI(TAG, "✅ 原文: \"%s\"", asr_buf);
                break;
            case WSS_RESULT_RESPONSE:
                ESP_LOGI(TAG, "💬 中文翻译: \"%s\"", asr_buf);
                break;
            }
        }

        /* Heartbeat every 10 seconds */
        static int tick = 0;
        if (++tick % 100 == 0) {
            ESP_LOGI(TAG, "Heartbeat: Wi-Fi=%d, WSS=%d",
                     wifi_is_connected(), wss_is_connected());
        }
    }
}
