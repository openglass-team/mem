/*
 * Qwen Omni Realtime — event-driven WebSocket protocol.
 * Model: qwen3.5-omni-plus-realtime (10-language → Chinese translation)
 *
 * Protocol:
 *   1. WS connected → send session.update (translate to Chinese)
 *   2. Wait for session.updated → ready to send audio
 *   3. Send audio via input_audio_buffer.append (base64 PCM)
 *   4. Server VAD auto-detects speech start/stop
 *   5. Receive transcription.delta (real-time ASR in original language 🔤)
 *   6. Receive transcription.completed (final ASR in original language ✅)
 *   7. Receive response.text.delta (Chinese translation 💬)
 */

#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_tls.h"
#include "esp_crt_bundle.h"
#include "mbedtls/base64.h"
#include "sdkconfig.h"
#include "esp_websocket_client.h"
#include "cloud_uploader.h"
#include "wss_streamer.h"

static const char *TAG = "wss_streamer";

static esp_websocket_client_handle_t s_ws_client = NULL;
static bool s_ws_connected = false;
static bool s_session_ready = false;  // Set true on session.updated (config applied)

typedef struct { uint8_t data[640]; int len; } wss_packet_t;
static QueueHandle_t s_send_queue = NULL;

typedef struct { char text[256]; uint8_t type; } asr_result_t;
static QueueHandle_t s_recv_queue = NULL;

/* Result types for the recv queue */
enum {
    RESULT_ASR_DELTA = 0,    /* Real-time transcription delta 🔤 */
    RESULT_ASR_FINAL = 1,    /* Final transcription ✅ */
    RESULT_RESPONSE  = 2,    /* Model response text 💬 */
};

/* ---- Base64 encode helper ---- */
static int base64_encode(const uint8_t *src, size_t slen, char *dst, size_t dlen) {
    size_t olen = 0;
    int ret = mbedtls_base64_encode((unsigned char *)dst, dlen, &olen, src, slen);
    return (ret == 0) ? (int)olen : -1;
}

/* ---- PCM volume gain (16-bit, anti-clipping) ---- */
#define PCM_GAIN 2  // 2x amplification (was 4x, caused clipping)

static void apply_gain(uint8_t *data, int len) {
    if (PCM_GAIN == 1 || len < 2) return;
    int16_t *samples = (int16_t *)data;
    int count = len / 2;
    for (int i = 0; i < count; i++) {
        int32_t val = (int32_t)samples[i] * PCM_GAIN;
        if (val > 32767) val = 32767;
        else if (val < -32768) val = -32768;
        samples[i] = (int16_t)val;
    }
}

/* ---- JSON value extractor ---- */
static const char *json_val(const char *j, int jl, const char *k, int *ol) {
    char p[64]; int kl = snprintf(p, sizeof(p), "\"%s\":\"", k);
    if (kl < 0 || kl >= (int)sizeof(p)) return NULL;
    const char *s = strstr(j, p); if (!s) return NULL; s += kl;
    const char *e = strchr(s, '"'); if (!e || e >= j + jl) return NULL;
    *ol = (int)(e - s); return s;
}

static void on_final(const char *t, int l);

static void wss_evt(void *a, esp_event_base_t b, int32_t id, void *d) {
    esp_websocket_event_data_t *e = (esp_websocket_event_data_t *)d;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Connected");
        s_ws_connected = true;
        s_session_ready = false;
        {
            /* Step 1: Send session.update to configure Qwen-Realtime
             * NOTE: Do NOT include "input_audio_transcription" — it is NOT a
             * valid raw WebSocket JSON field (only exists in Python SDK).
             * Transcription is enabled by default. Including it causes the
             * server to silently reject session.update (no session.updated,
             * no error event, 60s timeout). */
            const char *json =
                "{"
                "\"type\":\"session.update\","
                "\"session\":{"
                "\"modalities\":[\"text\"],"
                "\"voice\":\"Tina\","
                "\"input_audio_format\":\"pcm\","
                "\"output_audio_format\":\"pcm\","
                "\"instructions\":\"You are a real-time speech translator. You can understand speech in Chinese, English, Japanese, Korean, French, German, Spanish, Russian, Arabic, and Portuguese. Translate everything the user says into Simplified Chinese (简体中文). Only output the Chinese translation, nothing else. Do not add explanations or notes.\","
                "\"turn_detection\":{"
                "\"type\":\"server_vad\","
                "\"threshold\":0.5,"
                "\"silence_duration_ms\":800"
                "}"
                "}"
                "}";
            ESP_LOGI(TAG, "Sending session.update");
            esp_websocket_client_send_text(s_ws_client, json, strlen(json),
                                           portMAX_DELAY);
        }
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Disconnected");
        s_ws_connected = false;
        s_session_ready = false;
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "Error");
        s_ws_connected = false;
        s_session_ready = false;
        break;

    case WEBSOCKET_EVENT_DATA:
        if (e->op_code == 0x01 && e->data_len > 2) {
            /* Text response from Qwen */
            const char *raw = (const char *)e->data_ptr;
            int len = e->data_len;
            ESP_LOGI(TAG, "Qwen[%d]: %.*s", len, len < 200 ? len : 200, raw);

            /* Check for session.updated → ready to send audio
             * Only session.updated means the config has been applied. */
            if (!s_session_ready && strstr(raw, "\"session.updated\"")) {
                s_session_ready = true;
                ESP_LOGI(TAG, "Session ready — audio streaming enabled");
            }

            /* Speech events */
            else if (strstr(raw, "input_audio_buffer.speech_started")) {
                ESP_LOGI(TAG, "🎤 语音开始");
            }
            else if (strstr(raw, "input_audio_buffer.speech_stopped")) {
                ESP_LOGI(TAG, "🤚 语音结束");
            }

            /* Real-time ASR transcription delta 🔤 (original language) */
            else if (strstr(raw, "input_audio_transcription.delta")) {
                int tl = 0;
                const char *txt = json_val(raw, len, "delta", &tl);
                if (txt && tl > 0 && tl < 256) {
                    asr_result_t ar;
                    ar.type = RESULT_ASR_DELTA;
                    memcpy(ar.text, txt, tl); ar.text[tl] = 0;
                    xQueueSend(s_recv_queue, &ar, 0);
                    ESP_LOGI(TAG, "🔤 原文: \"%s\"", ar.text);
                }
            }

            /* Final ASR transcription ✅ (original language) */
            else if (strstr(raw, "input_audio_transcription.completed")) {
                int tl = 0;
                const char *txt = json_val(raw, len, "transcript", &tl);
                if (txt && tl > 0 && tl < 256) {
                    char final_text[256];
                    memcpy(final_text, txt, tl); final_text[tl] = 0;
                    ESP_LOGI(TAG, "✅ 原文: \"%s\"", final_text);
                    asr_result_t ar;
                    ar.type = RESULT_ASR_FINAL;
                    strncpy(ar.text, final_text, 255);
                    ar.text[255] = 0;
                    xQueueSend(s_recv_queue, &ar, 0);
                    on_final(final_text, tl);
                } else {
                    ESP_LOGI(TAG, "✅ 原文: (empty)");
                }
            }

            /* Model response text delta 💬 (Chinese translation) */
            else if (strstr(raw, "\"response.text.delta\"")) {
                int tl = 0;
                const char *txt = json_val(raw, len, "delta", &tl);
                if (txt && tl > 0 && tl < 256) {
                    asr_result_t ar;
                    ar.type = RESULT_RESPONSE;
                    memcpy(ar.text, txt, tl); ar.text[tl] = 0;
                    xQueueSend(s_recv_queue, &ar, 0);
                    ESP_LOGI(TAG, "💬 中文: \"%s\"", ar.text);
                }
            }

            /* Errors */
            else if (strstr(raw, "\"error\"")) {
                ESP_LOGE(TAG, "Qwen error: %.*s", len < 300 ? len : 300, raw);
            }
        } else if (e->op_code == 0x02 && e->data_len > 0) {
            /* Binary response (PCM audio from model) */
            ESP_LOGI(TAG, "Binary audio: %d bytes", e->data_len);
        }
        break;
    }
}

static void on_final(const char *t, int l) {
    ESP_LOGI(TAG, "Doc: \"%.*s\"", l, t);
    cloud_upload_text(t, l);
}

/* ---- Public ---- */
void wss_streamer_init(void) {
    s_send_queue = xQueueCreate(16, sizeof(wss_packet_t));
    s_recv_queue = xQueueCreate(32, sizeof(asr_result_t));

    esp_websocket_client_config_t cfg = {
        .uri = CONFIG_WSS_URL,
        .task_stack = 8192,
        .task_prio = 5,
        .buffer_size = 8192,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms = 10000,
    };
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
#if CONFIG_WSS_SKIP_CERT_VERIFY
    cfg.skip_cert_common_name_check = true;
#endif

    s_ws_client = esp_websocket_client_init(&cfg);
    if (!s_ws_client) { ESP_LOGE(TAG, "init fail"); return; }

    /* Add Authorization header for DashScope */
    esp_websocket_client_append_header(s_ws_client, "Authorization",
                                       CONFIG_VC_API_KEY);
    ESP_LOGI(TAG, "Auth: %s", CONFIG_VC_API_KEY);

    esp_websocket_register_events(s_ws_client, WEBSOCKET_EVENT_ANY, wss_evt, NULL);
    esp_websocket_client_start(s_ws_client);
    ESP_LOGI(TAG, "Started: %s", CONFIG_WSS_URL);
}

void wss_streamer_send_opus_packet(const uint8_t *d, int len) {
    if (!s_ws_client || !s_ws_connected || !s_session_ready || len <= 0 || len > 640)
        return;
    wss_packet_t p;
    memcpy(p.data, d, len);
    p.len = len;
    if (xQueueSend(s_send_queue, &p, 0) != pdTRUE) {
        wss_packet_t o;
        xQueueReceive(s_send_queue, &o, 0);
        xQueueSend(s_send_queue, &p, 0);
    }
}

bool wss_is_connected(void) { return s_ws_connected; }

bool wss_streamer_get_result(char *b, int sz) {
    if (!s_recv_queue || !b || sz <= 0) return false;
    asr_result_t r;
    if (xQueueReceive(s_recv_queue, &r, 0) == pdTRUE) {
        strncpy(b, r.text, sz - 1); b[sz - 1] = 0;
        return true;
    }
    return false;
}

uint8_t wss_streamer_get_result_typed(char *b, int sz) {
    if (!s_recv_queue || !b || sz <= 0) return 0xFF;
    asr_result_t r;
    if (xQueueReceive(s_recv_queue, &r, 0) == pdTRUE) {
        strncpy(b, r.text, sz - 1); b[sz - 1] = 0;
        return r.type;
    }
    return 0xFF;
}

void wss_streamer_send_task(void *pv) {
    wss_packet_t *p = malloc(sizeof(wss_packet_t));
    if (!p) { vTaskDelete(NULL); return; }

    char b64_buf[1024];   // base64 of 640 bytes → max ~860 chars
    char json_buf[1100];  // JSON wrapper

    while (1) {
        if (xQueueReceive(s_send_queue, p, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (s_ws_connected && s_session_ready && s_ws_client) {

                /* Apply volume gain before encoding */
                apply_gain(p->data, p->len);

                /* Base64 encode the PCM chunk */
                int b64_len = base64_encode(p->data, p->len, b64_buf, sizeof(b64_buf));
                if (b64_len < 0) {
                    ESP_LOGE(TAG, "base64 encode failed");
                    continue;
                }

                /* Wrap in input_audio_buffer.append event */
                int json_len = snprintf(json_buf, sizeof(json_buf),
                    "{\"type\":\"input_audio_buffer.append\","
                    "\"audio\":\"%s\"}", b64_buf);

                if (json_len > 0 && json_len < (int)sizeof(json_buf)) {
                    esp_websocket_client_send_text(s_ws_client, json_buf,
                                                   json_len, portMAX_DELAY);
                } else {
                    ESP_LOGE(TAG, "JSON too long: %d", json_len);
                }
            }
        }
    }
    free(p);
    vTaskDelete(NULL);
}
