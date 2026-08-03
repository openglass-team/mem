"""
Qwen Omni Realtime 透明中继服务器
ESP32 → 本地 :8765 → Qwen DashScope
APP  → 本地 :8766（实时推送识别/翻译结果）

用法:
    python qwen_asr_server.py                  # 默认自动检测语言
    python qwen_asr_server.py --lang ja        # 指定日语
    python qwen_asr_server.py --lang auto      # 自动检测
    python qwen_asr_server.py --list-langs     # 列出支持的语言
"""

import asyncio
import base64
import json
import struct
import sys
import wave
import websockets
from datetime import datetime

HOST = "0.0.0.0"
PORT = 8765

DASHSCOPE_KEY = "Bearer sk-ws-H.ELRHLIY.YaVi.MEUCIAHZwnqd-6_semqc0QRGaQL2gDLr5Qx87nfqqFPNmg36AiEAv6BY1buNryi0UReao71mO9QtGjA11o1BrJ4bunW6u3M"
DASHSCOPE_URL = "wss://dashscope.aliyuncs.com/api-ws/v1/realtime?model=qwen3.5-omni-plus-realtime"

MIN_SEGMENT_SEC = 0.5
LOG_FILE = "asr_log.txt"

# ======================== 十国语言支持 ========================
LANGUAGES = {
    "auto": "自动检测",
    "zh":   "中文",
    "en":   "English",
    "ja":   "日本語",
    "ko":   "한국어",
    "fr":   "Français",
    "de":   "Deutsch",
    "es":   "Español",
    "ru":   "Русский",
    "ar":   "العربية",
    "pt":   "Português",
}

# 各语言的 instructions 提示 — 翻译模式：把用户说的话翻译成中文
LANG_INSTRUCTIONS = {
    "auto": "You are a real-time speech translator. You can understand speech in Chinese, English, Japanese, Korean, French, German, Spanish, Russian, Arabic, and Portuguese. Translate everything the user says into Simplified Chinese (简体中文). Only output the Chinese translation, nothing else. Do not add explanations or notes.",
    "zh":   "你是实时语音翻译助手。请将用户说的每一句话翻译成简体中文。只输出中文翻译，不要添加任何解释或说明。如果用户说的是中文，原样输出。",
    "en":   "You are a real-time speech translator. Translate everything the user says from English into Simplified Chinese (简体中文). Only output the Chinese translation, nothing else. Do not add explanations or notes.",
    "ja":   "You are a real-time speech translator. Translate everything the user says from Japanese into Simplified Chinese (简体中文). Only output the Chinese translation, nothing else. Do not add explanations or notes.",
    "ko":   "You are a real-time speech translator. Translate everything the user says from Korean into Simplified Chinese (简体中文). Only output the Chinese translation, nothing else. Do not add explanations or notes.",
    "fr":   "You are a real-time speech translator. Translate everything the user says from French into Simplified Chinese (简体中文). Only output the Chinese translation, nothing else. Do not add explanations or notes.",
    "de":   "You are a real-time speech translator. Translate everything the user says from German into Simplified Chinese (简体中文). Only output the Chinese translation, nothing else. Do not add explanations or notes.",
    "es":   "You are a real-time speech translator. Translate everything the user says from Spanish into Simplified Chinese (简体中文). Only output the Chinese translation, nothing else. Do not add explanations or notes.",
    "ru":   "You are a real-time speech translator. Translate everything the user says from Russian into Simplified Chinese (简体中文). Only output the Chinese translation, nothing else. Do not add explanations or notes.",
    "ar":   "You are a real-time speech translator. Translate everything the user says from Arabic into Simplified Chinese (简体中文). Only output the Chinese translation, nothing else. Do not add explanations or notes.",
    "pt":   "You are a real-time speech translator. Translate everything the user says from Portuguese into Simplified Chinese (简体中文). Only output the Chinese translation, nothing else. Do not add explanations or notes.",
}

CURRENT_LANG = "auto"

def get_instructions():
    return LANG_INSTRUCTIONS.get(CURRENT_LANG, LANG_INSTRUCTIONS["auto"])

def parse_args():
    global CURRENT_LANG
    if "--list-langs" in sys.argv:
        print("支持的语言:")
        for code, name in LANGUAGES.items():
            print(f"  {code:4s}  {name}")
        sys.exit(0)
    if "--lang" in sys.argv:
        idx = sys.argv.index("--lang")
        if idx + 1 < len(sys.argv):
            lang = sys.argv[idx + 1]
            if lang in LANGUAGES:
                CURRENT_LANG = lang
            else:
                print(f"未知语言: {lang}，可用: {', '.join(LANGUAGES.keys())}")
                sys.exit(1)

parse_args()

stats = {"connections": 0, "recognitions": 0, "audio_bytes": 0}


# ======================== APP 端 WebSocket 推送 ========================
# 把识别结果实时推送给手机 APP，APP 端连接 ws://<电脑IP>:8766
APP_WS_PORT = 8766
app_clients = set()           # 当前连着的 APP 客户端
_broadcast_tasks = set()      # 持有引用，防止 create_task 被 GC


def _ts():
    """毫秒时间戳"""
    return int(datetime.now().timestamp() * 1000)


async def _safe_send(ws, msg):
    """向单个 APP 发送；失败则将其从客户端集合移除"""
    try:
        await ws.send(msg)
    except Exception:
        app_clients.discard(ws)


def schedule_broadcast(payload: dict):
    """非阻塞地把消息广播给所有 APP 客户端（慢客户端不会阻塞解析）"""
    if not app_clients:
        return
    msg = json.dumps(payload, ensure_ascii=False)
    for ws in list(app_clients):
        task = asyncio.create_task(_safe_send(ws, msg))
        _broadcast_tasks.add(task)
        task.add_done_callback(_broadcast_tasks.discard)


async def handle_app(app_ws):
    """APP 客户端：注册、发送 hello、保持连接"""
    app_clients.add(app_ws)
    addr = app_ws.remote_address
    print(f"\n[{datetime.now().strftime('%H:%M:%S')}] 📱 APP 已连接: {addr} (共{len(app_clients)}个)")
    try:
        await app_ws.send(json.dumps({
            "type": "hello",
            "server": "qwen-asr-relay",
            "lang": CURRENT_LANG,
            "ts": _ts(),
        }, ensure_ascii=False))
        # 保持连接，忽略 APP 发来的任何消息
        async for _ in app_ws:
            pass
    except websockets.exceptions.ConnectionClosed:
        pass
    finally:
        app_clients.discard(app_ws)
        print(f"[{datetime.now().strftime('%H:%M:%S')}] 📱 APP 断开: {addr} (剩{len(app_clients)}个)")


def save_wav(filename: str, pcm_data: bytes):
    with wave.open(filename, "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(16000)
        wf.writeframes(pcm_data)


def calc_rms(pcm_data: bytes) -> float:
    if len(pcm_data) < 2:
        return 0.0
    count = len(pcm_data) // 2
    total = sum(
        struct.unpack_from("<h", pcm_data, i)[0] ** 2
        for i in range(0, len(pcm_data) - 1, 2)
    )
    return (total / count) ** 0.5


async def handle_audio(esp32_ws):
    """透明中继：ESP32 ↔ Qwen，同时采集音频保存 WAV"""
    stats["connections"] += 1
    addr = esp32_ws.remote_address
    print(f"\n[{datetime.now().strftime('%H:%M:%S')}] ESP32 已连接: {addr}")

    audio_pcm = bytearray()   # 收集的原始 PCM
    headers = {"Authorization": DASHSCOPE_KEY}

    try:
        async with websockets.connect(
            DASHSCOPE_URL, additional_headers=headers,
            ping_interval=20, close_timeout=5
        ) as qwen_ws:

            print(f"  🔗 已连接千问")

            # 两个方向同时转发
            async def esp32_to_qwen():
                """ESP32 发来的消息 → 转发给千问"""
                nonlocal audio_pcm
                while True:
                    try:
                        msg = await asyncio.wait_for(esp32_ws.recv(), timeout=0.5)
                    except asyncio.TimeoutError:
                        continue

                    if isinstance(msg, bytes):
                        # 尝试解析为文本（ESP32 发的是 text frame）
                        try:
                            text = msg.decode("utf-8")
                        except UnicodeDecodeError:
                            continue
                    elif isinstance(msg, str):
                        text = msg
                    else:
                        continue

                    # 转发给千问
                    # 如果是 session.update，注入当前语言的 instructions
                    try:
                        fwd_evt = json.loads(text)
                        fwd_type = fwd_evt.get("type", "?")
                        if fwd_type == "session.update" and "session" in fwd_evt:
                            fwd_evt["session"]["instructions"] = get_instructions()
                            text = json.dumps(fwd_evt, ensure_ascii=False)
                            print(f"  ➡️ 转发: {fwd_type} (lang={CURRENT_LANG})")
                        elif fwd_type != "input_audio_buffer.append":
                            print(f"  ➡️ 转发: {fwd_type}")
                    except json.JSONDecodeError:
                        pass

                    await qwen_ws.send(text)

                    # 提取音频数据
                    try:
                        evt = json.loads(text)
                        if evt.get("type") == "input_audio_buffer.append":
                            b64 = evt.get("audio", "")
                            if b64:
                                pcm = base64.b64decode(b64)
                                audio_pcm.extend(pcm)
                                stats["audio_bytes"] += len(pcm)
                    except (json.JSONDecodeError, base64.binascii.Error):
                        pass

            async def qwen_to_esp32():
                """千问的回复 → 转发给 ESP32"""
                async for raw in qwen_ws:
                    # 转发原始文本给 ESP32
                    await esp32_ws.send(raw)

                    # 解析识别结果并显示
                    try:
                        evt = json.loads(raw)
                    except json.JSONDecodeError:
                        continue

                    etype = evt.get("type", "")

                    if etype == "session.updated":
                        sess = evt.get("session", {})
                        vad = sess.get("turn_detection", {})
                        mods = sess.get("modalities", "?")
                        instr = sess.get("instructions", "")[:40]
                        print(f"  ✅ 会话就绪 (modalities={mods}, "
                              f"VAD={vad.get('type','?')}, "
                              f"threshold={vad.get('threshold','?')}, "
                              f"silence={vad.get('silence_duration_ms','?')}ms)")
                        print(f"     instructions: {instr}...")

                    elif etype == "session.created":
                        print(f"  🔗 千问会话: {evt.get('session', {}).get('model', '?')}")

                    elif etype == "input_audio_buffer.speech_started":
                        print(f"  🎤 语音开始")
                        schedule_broadcast({"type": "speech_start", "ts": _ts()})

                    elif etype == "input_audio_buffer.speech_stopped":
                        print(f"  🤚 语音结束")
                        schedule_broadcast({"type": "speech_end", "ts": _ts()})

                    elif etype == "input_audio_buffer.committed":
                        print(f"  📦 音频已提交")

                    elif etype == "response.created":
                        print(f"  ⏳ 开始生成响应")

                    elif etype == "conversation.item.input_audio_transcription.delta":
                        delta = evt.get("delta", "")
                        print(f"  🔤 原文: {delta}", end="", flush=True)
                        schedule_broadcast({"type": "asr_delta", "text": delta, "lang": CURRENT_LANG, "ts": _ts()})

                    elif etype == "conversation.item.input_audio_transcription.completed":
                        transcript = evt.get("transcript", "")
                        print(f"\n  ✅ 原文: {transcript}")
                        schedule_broadcast({"type": "asr_final", "text": transcript, "lang": CURRENT_LANG, "ts": _ts()})
                        if transcript.strip():
                            stats["recognitions"] += 1
                            with open(LOG_FILE, "a", encoding="utf-8") as f:
                                f.write(f"[{datetime.now().strftime('%Y-%m-%d %H:%M:%S')}] "
                                        f"[{CURRENT_LANG}] {transcript}\n")

                    elif etype == "response.text.delta":
                        delta = evt.get("delta", "")
                        print(f"  💬 中文: {delta}", end="", flush=True)
                        schedule_broadcast({"type": "translation_delta", "text": delta, "ts": _ts()})

                    elif etype == "response.done":
                        print()

                    elif etype == "error":
                        err = evt.get("error", {})
                        print(f"\n  ❌ Qwen 错误: {err}")
                        print(f"     完整事件: {json.dumps(evt, ensure_ascii=False)[:300]}")

                    else:
                        print(f"  📋 {etype}")

            # 并发运行双向转发
            e2q = asyncio.create_task(esp32_to_qwen())
            q2e = asyncio.create_task(qwen_to_esp32())

            # 等任一方结束
            _, pending = await asyncio.wait(
                [e2q, q2e],
                return_when=asyncio.FIRST_COMPLETED
            )
            for t in pending:
                t.cancel()

    except websockets.exceptions.ConnectionClosed:
        print(f"  ⚠ 连接关闭")
    except Exception as e:
        print(f"  ❌ 异常: {type(e).__name__}: {e}")

    finally:
        # 保存 WAV
        buf = bytes(audio_pcm)
        dur = len(buf) / 32000
        rms = calc_rms(buf)
        if dur >= MIN_SEGMENT_SEC and rms >= 100:
            wav_name = f"audio_{datetime.now().strftime('%Y%m%d_%H%M%S')}.wav"
            save_wav(wav_name, buf)
            print(f"  💾 保存: {wav_name}  ({dur:.1f}s, RMS={rms:.0f})")
        else:
            print(f"  🗑️ 丢弃音频 ({dur:.1f}s, RMS={rms:.0f})")

        print(f"[{datetime.now().strftime('%H:%M:%S')}] ESP32 断开: {addr}")
        stats["connections"] -= 1


async def main():
    lang_name = LANGUAGES.get(CURRENT_LANG, "?")
    print("=" * 55)
    print("  千问 Qwen Omni 实时语音翻译中继")
    print(f"  ESP32 监听:  ws://{HOST}:{PORT}")
    print(f"  APP  监听:  ws://{HOST}:{APP_WS_PORT}")
    print(f"  中继到:      {DASHSCOPE_URL}")
    print(f"  语言:        {CURRENT_LANG} ({lang_name}) → 中文翻译")
    print(f"  日志:        {LOG_FILE}")
    print("=" * 55)
    print(f"\n  ✅ 等待 ESP32 + APP 连接...\n")

    async with websockets.serve(
        handle_audio, HOST, PORT,
        max_size=10 * 1024 * 1024,
        ping_interval=None,
        close_timeout=5,
    ), websockets.serve(
        handle_app, HOST, APP_WS_PORT,
        ping_interval=20,
        close_timeout=5,
    ):
        await asyncio.Future()


if __name__ == "__main__":
    asyncio.run(main())
