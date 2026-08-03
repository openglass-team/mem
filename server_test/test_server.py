"""
ESP32 PDM Mic → PCM Audio → WebSocket 测试服务器
持续识别语音片段，每段语音自动保存为独立 WAV 文件

用法:
    pip install websockets
    python test_server.py
"""

import asyncio
import struct
import time
import wave
import websockets
from datetime import datetime

HOST = "0.0.0.0"
PORT = 8765

# 静音间隔阈值（秒）：超过此时间无新数据，认为语音片段结束
SILENCE_TIMEOUT = 1.0

# 最小语音片段要求
MIN_DURATION_SEC = 0.3           # 最短 0.3 秒
MIN_RMS_AMPLITUDE = 150          # 16-bit PCM RMS 低于此值视为静音，丢弃


def calc_rms(pcm_data: bytes) -> float:
    """计算 16-bit PCM 数据的 RMS 振幅"""
    if len(pcm_data) < 2:
        return 0.0
    count = len(pcm_data) // 2
    total = 0
    for i in range(0, len(pcm_data) - 1, 2):
        sample = struct.unpack_from("<h", pcm_data, i)[0]
        total += sample * sample
    return (total / count) ** 0.5

stats = {
    "connections": 0,
    "total_packets": 0,
    "total_bytes": 0,
    "start_time": time.time(),
    "files_saved": 0,
}


async def handle_audio(websocket):
    """接收 ESP32 发来的 PCM 音频包，按语音片段自动保存 WAV"""
    stats["connections"] += 1
    client_addr = websocket.remote_address
    print(f"\n[{datetime.now().strftime('%H:%M:%S')}] 新连接: {client_addr}")

    audio_buffer = bytearray()        # 当前语音片段的 PCM 数据缓冲
    last_data_time = time.time()      # 最后一次收到数据的时间
    segment_active = False            # 是否正在采集语音片段
    last_report = time.time()

    async def finish_segment():
        """保存当前语音片段为 WAV 文件（含振幅检测）"""
        nonlocal segment_active
        if not segment_active:
            return

        buf_bytes = bytes(audio_buffer)
        duration = len(buf_bytes) / 32000

        # 检查 1：时长太短，丢弃
        if duration < MIN_DURATION_SEC:
            print(f"  🗑️ 丢弃片段（太短 {duration:.1f}s）")
            audio_buffer.clear()
            segment_active = False
            return

        # 检查 2：RMS 振幅过低（没有真实声音），丢弃
        rms = calc_rms(buf_bytes)
        if rms < MIN_RMS_AMPLITUDE:
            print(f"  🗑️ 丢弃片段（静音，RMS={rms:.0f}）")
            audio_buffer.clear()
            segment_active = False
            return

        filename = f"audio_{datetime.now().strftime('%Y%m%d_%H%M%S')}.wav"
        with wave.open(filename, "wb") as wf:
            wf.setnchannels(1)
            wf.setsampwidth(2)
            wf.setframerate(16000)
            wf.writeframes(buf_bytes)

        stats["files_saved"] += 1
        print(f"  💾 #{stats['files_saved']} 语音片段: {len(buf_bytes)} bytes, "
              f"{duration:.1f}s, RMS={rms:.0f} → {filename}")

        audio_buffer.clear()
        segment_active = False

    try:
        while True:
            try:
                message = await asyncio.wait_for(websocket.recv(), timeout=0.3)
            except asyncio.TimeoutError:
                # 超时未收到数据，检查是否语音片段结束
                if segment_active and (time.time() - last_data_time > SILENCE_TIMEOUT):
                    await finish_segment()
                continue

            if isinstance(message, bytes):
                stats["total_packets"] += 1
                stats["total_bytes"] += len(message)
                audio_buffer.extend(message)
                last_data_time = time.time()
                segment_active = True

                # 每秒打印一次统计
                now = time.time()
                if now - last_report >= 1.0:
                    elapsed = now - stats["start_time"]
                    kbps = (stats["total_bytes"] * 8 / 1000) / elapsed if elapsed > 0 else 0
                    buf_sec = len(audio_buffer) / 32000
                    print(f"  📦 packets: {stats['total_packets']:6d}  |  "
                          f"📊 {stats['total_bytes']/1024:.1f} KB  |  "
                          f"⚡ {kbps:.1f} kbps  |  "
                          f"🎙️ buf: {buf_sec:.1f}s  |  "
                          f"💾 files: {stats['files_saved']}")
                    last_report = now

            elif isinstance(message, str):
                print(f"  📝 收到文本: {message[:100]}")

    except websockets.exceptions.ConnectionClosed as e:
        code = getattr(e, 'code', '?')
        reason = getattr(e, 'reason', '?')
        print(f"  ⚠ 连接关闭: code={code}, reason={reason}")
    except Exception as e:
        print(f"  ❌ 异常: {type(e).__name__}: {e}")
    finally:
        # 保存最后的语音片段
        await finish_segment()
        print(f"\n[{datetime.now().strftime('%H:%M:%S')}] 断开: {client_addr}")
        stats["connections"] -= 1


async def main():
    print("=" * 55)
    print("  ESP32 PCM Audio 持续录音服务器")
    print(f"  监听地址: ws://{HOST}:{PORT}/audio")
    print(f"  语音片段超时: {SILENCE_TIMEOUT}s")
    print("=" * 55)
    print(f"\n  每个语音片段自动保存为独立 WAV 文件")
    print(f"  等待连接...\n")

    async with websockets.serve(
        handle_audio, HOST, PORT,
        max_size=10 * 1024 * 1024,
        ping_interval=None,
        close_timeout=5,
    ):
        await asyncio.Future()


if __name__ == "__main__":
    asyncio.run(main())
