# ESP32 + Qwen3.5 实时语音翻译（Arduino 版）

ESP32 + PDM 麦克风 → **直连阿里云 DashScope** → 十国语言实时译汉，无需任何中继服务器。

## 功能

- 🎤 PDM 麦克风实时采集 16kHz PCM 音频
- 🔒 自实现 WebSocket Secure 客户端（不依赖第三方 WebSocket 库）
- 🌍 十国语言 → 简体中文翻译（中/英/日/韩/法/德/俄/西/阿/葡）
- 📝 同时输出原文转写 + 中文翻译
- 🩺 内置网络诊断（SNTP 时间同步 / TCP / TLS / WS 握手检测）

## 硬件

| 元件 | 型号示例 | 说明 |
|------|---------|------|
| 主控 | ESP32 DevKit | 需支持 2.4G WiFi |
| 麦克风 | PDM 数字麦克风（如 SPH0641LU4H-1） | I2S 接口 |

### 接线

| PDM 麦克风 | ESP32 引脚 |
|-----------|-----------|
| VDD | 3.3V |
| GND | GND |
| CLK | GPIO4 |
| DATA | GPIO5 |

> 引脚可在 `qwen_realtime_arduino.ino` 顶部的 `I2S_*` 宏修改。

## 软件依赖

- **Arduino IDE** + **arduino-esp32 核心**（已测试 2.0.17）
- 无需额外库——仅使用核心自带的 `WiFi.h` / `WiFiClientSecure.h` / `driver/i2s.h`

## 配置

打开 `qwen_realtime_arduino/qwen_realtime_arduino.ino`，修改顶部配置区：

```cpp
#define WIFI_SSID        "你的WiFi"
#define WIFI_PASSWORD    "你的密码"
#define DASHSCOPE_API_KEY "sk-你的DashScope密钥"
#define DASHSCOPE_HOST   "dashscope.aliyuncs.com"
#define DASHSCOPE_PORT   443
#define DASHSCOPE_PATH   "/api-ws/v1/realtime?model=qwen3.5-omni-plus-realtime"
```

> DashScope API Key 在 [阿里云控制台 - DashScope](https://dashscope.console.aliyun.com/) 申请。

## 烧录

1. 用 Arduino IDE 打开 `qwen_realtime_arduino/qwen_realtime_arduino.ino`
2. 选对开发板（如 **ESP32 Dev Module**）和端口
3. 点击上传
4. 打开串口监视器，波特率 **115200**

## 串口输出示例

```
===========================================
  ESP32 Qwen3.5 十国语言译汉 (自实现WSS)
  PDM Mic → WSS → DashScope (直连，无需服务器)
===========================================
[WiFi] Connecting......
[WiFi] Connected! IP: 192.168.137.197
[Net] ✅ Time synced
[Net] ✅ TCP 443 reachable
[Net] ✅ TLS handshake OK
[WSS] ✅ WebSocket connected (101)
[WSS] → session.update sent
[WSS] ✅ Session ready — audio streaming enabled
[WSS] 🎤 speech started
📝 原文: Hello everyone
🇨🇳 各位好
✅ 中文: 各位好
```

## 工作原理

```
PDM 麦克风 ──I2S──→ ESP32 ──WSS(443)──→ DashScope Qwen3.5-Omni
                        │                     │
                        │              ┌──────┴──────┐
                        │              │             │
                  原文转写事件    中文翻译事件
              (input_audio_transcription) (response.text)
                        │             │
                        └──────┬──────┘
                          串口输出
```

1. ESP32 采集 PDM 音频 → PCM 16kHz
2. 通过自实现 WebSocket 客户端加密推送到 DashScope
3. 服务端 VAD 自动检测语音起止
4. ASR 引擎输出原文转写，LLM 生成中文翻译
5. 两路结果实时回传串口打印

## 支持语言

| 语种 | → 输出 |
|------|--------|
| 中文 | 原样输出 |
| English / 日本語 / 한국어 / Français / Deutsch / Русский / Español / العربية / Português | → 简体中文 |
