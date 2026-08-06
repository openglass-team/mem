/*
 * ESP32 + Qwen3.5 实时会议记录 — Arduino IDE 版
 * 自实现 WebSocket 客户端 (不依赖 WebSocketsClient 库，避免 fork 版兼容性问题)
 *
 * 架构:
 *   XIAO ESP32S3 PDM麦克风 → WebSocket(wss://自实现) → DashScope Qwen3.5 直连
 *   ESP32 直接 WSS 连 DashScope，API Key 在固件内，TLS 跳过证书验证
 *
 * 硬件: Seeed XIAO ESP32S3 Sense
 *   PDM CLK = GPIO42, PDM DIN = GPIO41
 *
 * 仅依赖 Arduino-ESP32 核心 (WiFiClientSecure)，无需安装第三方库
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include "driver/i2s.h"
#include "mbedtls/base64.h"
#include <time.h>

// ======================== 配置区 (必改) ========================

#define WIFI_SSID       "wwd"
#define WIFI_PASSWORD   "12345678"

// DashScope 直连
#define DASHSCOPE_HOST    "dashscope.aliyuncs.com"
#define DASHSCOPE_PORT    443
#define DASHSCOPE_PATH    "/api-ws/v1/realtime?model=qwen3.5-omni-plus-realtime"
#define DASHSCOPE_API_KEY "sk-ws-H.ELPPXMP.PDbb.MEYCIQDfaw7gLMBQshrmVyhm5yA6XSd9EJQ_Hnz98lpRSWtELQIhAOot_RJNycDzhtL_rFSRYu2tH5GDAOYFBrCQwJ6onSZ8"

// PDM 麦克风引脚 (XIAO ESP32S3 Sense)
#define PDM_CLK_PIN     42
#define PDM_DIN_PIN     41

// 音频参数
#define SAMPLE_RATE     16000
#define FRAME_MS        20
#define FRAME_SAMPLES   (SAMPLE_RATE * FRAME_MS / 1000)   // 320
#define FRAME_BYTES     (FRAME_SAMPLES * 2)                // 640
#define PCM_GAIN        2

// ======================== 全局状态 ========================

WiFiClientSecure wss;
static bool wsConnected  = false;
static bool sessionReady = false;

static QueueHandle_t pcmQueue    = NULL;
static QueueHandle_t resultQueue = NULL;
static SemaphoreHandle_t wssMutex = NULL;   // 保护 wss 读写 (多任务)

enum {
    RESULT_ASR_DELTA = 0,     // 原文转写-增量
    RESULT_ASR_FINAL = 1,     // 原文转写-最终
    RESULT_TRANS_DELTA = 2,   // 翻译-增量
    RESULT_TRANS_FINAL = 3,   // 翻译-最终
};
typedef struct { char text[256]; uint8_t type; } asrResult_t;

static i2s_port_t i2sNum = I2S_NUM_0;

// ======================== 工具函数 ========================

static int b64Encode(const uint8_t *src, size_t slen, char *dst, size_t dlen) {
    size_t olen = 0;
    int ret = mbedtls_base64_encode((unsigned char *)dst, dlen, &olen, src, slen);
    return (ret == 0) ? (int)olen : -1;
}

static void applyGain(uint8_t *data, int len) {
    if (PCM_GAIN == 1 || len < 2) return;
    int16_t *s = (int16_t *)data;
    int n = len / 2;
    for (int i = 0; i < n; i++) {
        int32_t v = (int32_t)s[i] * PCM_GAIN;
        if (v > 32767) v = 32767;
        else if (v < -32768) v = -32768;
        s[i] = (int16_t)v;
    }
}

// 简单 JSON 值提取: "key":"value" → value
static const char *jsonVal(const char *j, const char *k, int *ol) {
    char p[64];
    int kl = snprintf(p, sizeof(p), "\"%s\":\"", k);
    if (kl <= 0 || kl >= (int)sizeof(p)) return NULL;
    const char *s = strstr(j, p);
    if (!s) return NULL;
    s += kl;
    const char *e = strchr(s, '"');
    if (!e) return NULL;
    *ol = (int)(e - s);
    return s;
}

// 持锁读一字节 (调用者必须已持有 wssMutex)
static int readByteLocked(unsigned long timeoutMs) {
    unsigned long t0 = millis();
    while (millis() - t0 < timeoutMs) {
        if (wss.available()) return wss.read();
        delay(1);
    }
    return -1;
}

// ======================== WebSocket 实现 ========================

// 建立 WSS 连接 + WebSocket 升级握手 (在持有 wssMutex 的前提下、或单线程 setup 中调用)
static bool wsConnect() {
    Serial.println("[WSS] Connecting (TLS)...");
    wss.stop();
    wss.setInsecure();
    wss.setTimeout(5000);
    if (!wss.connect(DASHSCOPE_HOST, DASHSCOPE_PORT)) {
        Serial.println("[WSS] ❌ TLS connect failed");
        return false;
    }

    // 生成随机 Sec-WebSocket-Key (base64 of 16 bytes)
    uint8_t keyBytes[16];
    for (int i = 0; i < 16; i++) keyBytes[i] = (uint8_t)random(0, 256);
    char wsKey[32] = {0};
    size_t klen = 0;
    mbedtls_base64_encode((unsigned char *)wsKey, sizeof(wsKey), &klen, keyBytes, 16);

    char req[512];
    int rlen = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Authorization: Bearer %s\r\n"
        "\r\n",
        DASHSCOPE_PATH, DASHSCOPE_HOST, wsKey, DASHSCOPE_API_KEY);
    if (wss.write((const uint8_t *)req, rlen) != rlen) {
        Serial.println("[WSS] ❌ write request failed");
        return false;
    }

    // 读状态行，确认 101
    String statusLine = wss.readStringUntil('\n');
    if (statusLine.indexOf("101") < 0) {
        Serial.printf("[WSS] ❌ Upgrade failed: %s\n", statusLine.c_str());
        wss.stop();
        return false;
    }
    // 读完剩余 header (到空行)
    while (wss.connected()) {
        String h = wss.readStringUntil('\n');
        if (h.length() <= 2) break;   // "\r" 或 ""
    }
    Serial.println("[WSS] ✅ WebSocket connected (101)");
    return true;
}

// 发送 WebSocket 文本帧 (客户端必须 mask)。调用者持锁。
static void wsSendText(const char *text) {
    size_t len = strlen(text);
    uint8_t header[14];
    int hlen = 0;
    header[hlen++] = 0x81;   // FIN + text opcode
    if (len < 126) {
        header[hlen++] = 0x80 | (uint8_t)len;
    } else if (len < 65536) {
        header[hlen++] = 0x80 | 126;
        header[hlen++] = (len >> 8) & 0xFF;
        header[hlen++] = len & 0xFF;
    } else {
        header[hlen++] = 0x80 | 127;
        uint64_t l = len;
        for (int i = 7; i >= 0; i--) header[hlen++] = (l >> (i * 8)) & 0xFF;
    }
    uint8_t mask[4];
    for (int i = 0; i < 4; i++) mask[i] = (uint8_t)random(0, 256);
    for (int i = 0; i < 4; i++) header[hlen++] = mask[i];

    wss.write(header, hlen);
    // 分块写 masked payload (避免大块 malloc)
    const uint8_t *src = (const uint8_t *)text;
    uint8_t buf[512];
    size_t sent = 0;
    while (sent < len) {
        size_t chunk = len - sent;
        if (chunk > sizeof(buf)) chunk = sizeof(buf);
        for (size_t i = 0; i < chunk; i++)
            buf[i] = src[sent + i] ^ mask[(sent + i) % 4];
        wss.write(buf, chunk);
        sent += chunk;
    }
}

static void sendSessionUpdate() {
    const char *json =
        "{"
        "\"type\":\"session.update\","
        "\"session\":{"
        "\"modalities\":[\"text\"],"
        "\"input_audio_format\":\"pcm\","
        "\"instructions\":\"You are a real-time speech translation service. The user may speak Chinese, English, Japanese, Korean, French, German, Russian, Spanish, Arabic, or Portuguese. Translate everything into Simplified Chinese (简体中文). If the user speaks Chinese, output as-is. Output only the Chinese text, no explanations.\","
        "\"input_audio_transcription\":{\"model\":\"qwen3-asr-flash-realtime\"},"
        "\"turn_detection\":{"
        "\"type\":\"server_vad\","
        "\"threshold\":0.5,"
        "\"silence_duration_ms\":800,"
        "\"create_response\":true"
        "}"
        "}"
        "}";
    wsSendText(json);
    Serial.println("[WSS] → session.update sent (翻译模式: 十国语言译汉)");
}

// ======================== WebSocket 接收任务 ========================

void wsRxTask(void *pv) {
    while (1) {
        if (!wsConnected) { delay(50); continue; }

        // 非阻塞检查是否有数据 (短暂持锁)
        xSemaphoreTake(wssMutex, portMAX_DELAY);
        int avail = wss.available();
        bool conn = wss.connected();
        xSemaphoreGive(wssMutex);

        if (avail <= 0) {
            if (!conn) {
                Serial.println("[WSS] Rx: disconnected");
                wsConnected = false;
                sessionReady = false;
            }
            delay(2);
            continue;
        }

        // 有数据，持锁读完整帧
        xSemaphoreTake(wssMutex, portMAX_DELAY);
        int b0 = wss.read();
        int b1 = readByteLocked(3000);
        if (b1 < 0) { xSemaphoreGive(wssMutex); continue; }

        int opcode = b0 & 0x0F;
        bool masked = (b1 & 0x80) != 0;
        size_t len = b1 & 0x7F;
        if (len == 126) {
            int h = readByteLocked(2000);
            int l = readByteLocked(2000);
            if (h < 0 || l < 0) { xSemaphoreGive(wssMutex); continue; }
            len = ((size_t)h << 8) | l;
        } else if (len == 127) {
            len = 0;
            bool ok = true;
            for (int i = 0; i < 8; i++) {
                int b = readByteLocked(2000);
                if (b < 0) { ok = false; break; }
                len = (len << 8) | b;
            }
            if (!ok) { xSemaphoreGive(wssMutex); continue; }
        }

        uint8_t mask[4] = {0};
        if (masked) {
            for (int i = 0; i < 4; i++) {
                int b = readByteLocked(2000);
                if (b >= 0) mask[i] = (uint8_t)b;
            }
        }

        String payload;
        bool ok = true;
        if (len > 0 && len < 8192) {
            payload.reserve(len + 1);
            for (size_t i = 0; i < len; i++) {
                int b = readByteLocked(3000);
                if (b < 0) { ok = false; break; }
                if (masked) b ^= mask[i % 4];
                payload += (char)b;
            }
        } else if (len >= 8192) {
            for (size_t i = 0; i < len; i++) readByteLocked(1000);   // 丢弃
            ok = false;
        }
        xSemaphoreGive(wssMutex);

        if (!ok) continue;

        // 处理帧 (不持锁)
        if (opcode == 0x8) {        // close
            Serial.println("[WSS] Server closed");
            wsConnected = false;
            sessionReady = false;
            wss.stop();
            continue;
        }
        if (opcode == 0x9) continue;   // ping (忽略，简化)
        if (opcode != 0x1) continue;   // 只处理文本帧

        const char *raw = payload.c_str();
        if (strstr(raw, "session.updated")) {
            sessionReady = true;
            Serial.println("[WSS] ✅ Session ready — audio streaming enabled");
        }
        else if (strstr(raw, "input_audio_buffer.speech_started")) {
            Serial.println("[WSS] 🎤 speech started");
        }
        else if (strstr(raw, "input_audio_buffer.speech_stopped")) {
            Serial.println("[WSS] 🤚 speech stopped");
        }
        else if (strstr(raw, "input_audio_transcription.delta")) {
            int tl = 0;
            const char *t = jsonVal(raw, "delta", &tl);
            if (t && tl > 0 && tl < 256) {
                asrResult_t r;
                r.type = RESULT_ASR_DELTA;
                memcpy(r.text, t, tl);
                r.text[tl] = 0;
                xQueueSend(resultQueue, &r, 0);
            }
        }
        else if (strstr(raw, "input_audio_transcription.completed")) {
            int tl = 0;
            const char *t = jsonVal(raw, "transcript", &tl);
            if (t && tl > 0 && tl < 256) {
                asrResult_t r;
                r.type = RESULT_ASR_FINAL;
                memcpy(r.text, t, tl);
                r.text[tl] = 0;
                xQueueSend(resultQueue, &r, 0);
            }
        }
        else if (strstr(raw, "response.text.delta")) {
            int tl = 0;
            const char *t = jsonVal(raw, "delta", &tl);
            if (t && tl > 0 && tl < 256) {
                asrResult_t r;
                r.type = RESULT_TRANS_DELTA;
                memcpy(r.text, t, tl);
                r.text[tl] = 0;
                xQueueSend(resultQueue, &r, 0);
            }
        }
        else if (strstr(raw, "response.text.done")) {
            int tl = 0;
            const char *t = jsonVal(raw, "text", &tl);
            if (t && tl > 0 && tl < 256) {
                asrResult_t r;
                r.type = RESULT_TRANS_FINAL;
                memcpy(r.text, t, tl);
                r.text[tl] = 0;
                xQueueSend(resultQueue, &r, 0);
            }
        }
        else if (strstr(raw, "\"error\"")) {
            Serial.printf("[WSS] ❌ Error: %.300s\n", raw);
        }
    }
}

// ======================== I2S PDM 麦克风读取任务 ========================

void i2sReadTask(void *pv) {
    i2s_config_t i2sCfg = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM),
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 512,
        .use_apll = false,
    };
    i2s_pin_config_t pinCfg = {
        .bck_io_num = I2S_PIN_NO_CHANGE,
        .ws_io_num = PDM_CLK_PIN,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = PDM_DIN_PIN,
    };
    if (i2s_driver_install(i2sNum, &i2sCfg, 0, NULL) != ESP_OK) {
        Serial.println("[I2S] driver_install failed!");
        vTaskDelete(NULL);
        return;
    }
    i2s_set_pin(i2sNum, &pinCfg);
    Serial.println("[I2S] PDM mic started (16kHz mono, DMA 512x8)");

    const int READ_SIZE = 2048;
    int16_t *rb = (int16_t *)calloc(1, READ_SIZE);
    int16_t frameBuf[FRAME_SAMPLES];
    int frameIdx = 0;
    size_t rbytes;
    while (1) {
        if (i2s_read(i2sNum, rb, READ_SIZE, &rbytes,
                     pdMS_TO_TICKS(100)) == ESP_OK && rbytes > 0) {
            int total = (int)rbytes / sizeof(int16_t);
            for (int i = 0; i < total; i++) {
                frameBuf[frameIdx++] = rb[i];
                if (frameIdx >= FRAME_SAMPLES) {
                    xQueueSend(pcmQueue, frameBuf, pdMS_TO_TICKS(5));
                    frameIdx = 0;
                }
            }
        }
    }
    free(rb);
    vTaskDelete(NULL);
}

// ======================== setup ========================

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println();
    Serial.println("===========================================");
    Serial.println("  ESP32 Qwen3.5 十国语言译汉 (原文+中文)");
    Serial.println("  PDM Mic → WSS → DashScope (直连，无需服务器)");
    Serial.println("===========================================");

    // --- WiFi ---
    Serial.print("[WiFi] Connecting");
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.printf("\n[WiFi] Connected! IP: %s\n",
                  WiFi.localIP().toString().c_str());

    // --- SNTP (TLS 握手需要正确时间) ---
    Serial.println("[Net] Syncing time...");
    configTime(8 * 3600, 0, "ntp.aliyun.com", "pool.ntp.org", "time.nist.gov");
    time_t now = time(nullptr);
    int ntpTry = 0;
    while (now < 1700000000 && ntpTry < 10) {
        delay(500);
        now = time(nullptr);
        ntpTry++;
    }
    Serial.println(ntpTry < 10 ? "[Net] ✅ Time synced" : "[Net] ⚠️ Time sync fail");

    // --- 队列 + 互斥锁 ---
    pcmQueue    = xQueueCreate(16, FRAME_BYTES);
    resultQueue = xQueueCreate(32, sizeof(asrResult_t));
    wssMutex    = xSemaphoreCreateMutex();
    if (!pcmQueue || !resultQueue || !wssMutex) {
        Serial.println("[ERR] Queue/Mutex create failed!");
        return;
    }

    // --- WSS 连接 + 发 session.update ---
    if (wsConnect()) {
        wsConnected = true;
        sessionReady = false;
        delay(200);                 // 等服务端 session.created 到达
        sendSessionUpdate();        // 单线程，无需锁
    }

    // --- 任务 ---
    xTaskCreate(i2sReadTask, "i2s_read", 4096, NULL, 8, NULL);
    xTaskCreate(wsRxTask,    "ws_rx",    8192, NULL, 5, NULL);

    Serial.println("[Main] Pipeline started. Speak into the mic!\n");
}

// ======================== loop ========================

void loop() {
    // 1. 断线重连
    if (!wsConnected) {
        static unsigned long lastReconnect = 0;
        if (millis() - lastReconnect > 5000) {
            lastReconnect = millis();
            Serial.println("[WSS] Reconnecting...");
            if (wsConnect()) {
                wsConnected = true;
                sessionReady = false;
                delay(200);
                xSemaphoreTake(wssMutex, portMAX_DELAY);
                sendSessionUpdate();
                xSemaphoreGive(wssMutex);
            }
        }
    }

    // 2. 发送音频帧 (仅 session ready 后)
    if (wsConnected && sessionReady) {
        static uint8_t pcmFrame[FRAME_BYTES];
        static char b64Buf[1024];
        static char jsonBuf[1100];

        for (int i = 0; i < 4; i++) {
            if (xQueueReceive(pcmQueue, pcmFrame, 0) != pdTRUE) break;

            applyGain(pcmFrame, FRAME_BYTES);
            int b64Len = b64Encode(pcmFrame, FRAME_BYTES, b64Buf, sizeof(b64Buf));
            if (b64Len < 0) continue;

            int jlen = snprintf(jsonBuf, sizeof(jsonBuf),
                "{\"type\":\"input_audio_buffer.append\",\"audio\":\"%s\"}",
                b64Buf);
            if (jlen > 0 && jlen < (int)sizeof(jsonBuf)) {
                xSemaphoreTake(wssMutex, portMAX_DELAY);
                wsSendText(jsonBuf);
                xSemaphoreGive(wssMutex);
            }
        }
    }

    // 3. 打印识别结果
    asrResult_t r;
    while (xQueueReceive(resultQueue, &r, 0) == pdTRUE) {
        switch (r.type) {
        case RESULT_ASR_DELTA:
            Serial.printf("🔤 %s", r.text);
            break;
        case RESULT_ASR_FINAL:
            Serial.printf("\n📝 原文: %s\n", r.text);
            break;
        case RESULT_TRANS_DELTA:
            Serial.printf("🇨🇳 %s", r.text);
            break;
        case RESULT_TRANS_FINAL:
            Serial.printf("\n✅ 中文: %s\n", r.text);
            break;
        }
    }

    // 4. 心跳
    static int tick = 0;
    if (++tick % 5000 == 0) {
        Serial.printf("[HB] WiFi=%d  WSS=%d  Sess=%d  PCMq=%d\n",
                      WiFi.status() == WL_CONNECTED ? 1 : 0,
                      wsConnected ? 1 : 0,
                      sessionReady ? 1 : 0,
                      pcmQueue ? uxQueueMessagesWaiting(pcmQueue) : 0);
    }

    delay(1);
}
