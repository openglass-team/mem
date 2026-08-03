"""
ASR 监控仪表盘 — 接收 ESP32 发来的识别文本，网页实时显示

用法:
    pip install flask
    python asr_dashboard.py

然后 ESP32 menuconfig 把 CLOUD_UPLOAD_URL 改为:
    http://<你电脑IP>:5000/asr

电脑 IP 看 ipconfig，手机热点通常是 192.168.137.1
"""
import os
from datetime import datetime
from flask import Flask, request, render_template_string

app = Flask(__name__)

# 存储识别记录
records: list[dict] = []

HTML = r"""
<!DOCTYPE html>
<html lang="zh">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>ASR 监控仪表盘</title>
<style>
  * { margin:0; padding:0; box-sizing:border-box; }
  body { font-family:'Segoe UI',sans-serif; background:#0f172a; color:#e2e8f0; min-height:100vh; }
  .header { background:#1e293b; padding:20px 30px; border-bottom:2px solid #334155;
            display:flex; justify-content:space-between; align-items:center; }
  .header h1 { font-size:1.5em; color:#38bdf8; }
  .stats { display:flex; gap:30px; }
  .stat { text-align:center; }
  .stat .num { font-size:2em; font-weight:bold; color:#38bdf8; }
  .stat .label { font-size:0.8em; color:#94a3b8; }
  .container { max-width:900px; margin:30px auto; padding:0 20px; }
  .card { background:#1e293b; border-radius:12px; padding:20px; margin-bottom:12px;
          border-left:4px solid #38bdf8; animation:slideIn .3s ease; }
  @keyframes slideIn { from{opacity:0;transform:translateY(-10px);} to{opacity:1;transform:translateY(0);} }
  .card .time { color:#64748b; font-size:0.85em; }
  .card .text { font-size:1.2em; margin-top:6px; }
  .empty { text-align:center; color:#64748b; padding:60px; font-size:1.1em; }
</style>
<script>
  setTimeout(()=>location.reload(), 3000);
</script>
</head>
<body>
<div class="header">
  <h1>🎤 ASR 实时监控</h1>
  <div class="stats">
    <div class="stat"><div class="num">{{ total }}</div><div class="label">总句数</div></div>
    <div class="stat"><div class="num">{{ today }}</div><div class="label">今日</div></div>
  </div>
</div>
<div class="container">
  {% if records %}
    {% for r in records %}
    <div class="card">
      <div class="time">{{ r.time }}</div>
      <div class="text">{{ r.text }}</div>
    </div>
    {% endfor %}
  {% else %}
    <div class="empty">等待识别结果... 对着 ESP32 说话吧 🎙️</div>
  {% endif %}
</div>
</body>
</html>
"""


@app.route("/")
def index():
    today = datetime.now().strftime("%Y-%m-%d")
    today_count = sum(1 for r in records if r["time"].startswith(today))
    return render_template_string(HTML, records=reversed(records[-100:]),
                                  total=len(records), today=today_count)


@app.route("/asr", methods=["PUT", "POST"])
def receive_asr():
    text = request.get_data(as_text=True).strip()
    if text:
        now = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        records.append({"time": now, "text": text})
        print(f"[{now}] {text}")
        # Write to local asr_log.txt
        with open("asr_log.txt", "a", encoding="utf-8") as f:
            f.write(f"[{now}] {text}\n")
        return "OK", 200
    return "empty", 400


if __name__ == "__main__":
    print("=" * 55)
    print("  ASR 监控仪表盘")
    print(f"  仪表盘: http://0.0.0.0:5000")
    print(f"  上报接口: POST http://0.0.0.0:5000/asr")
    print("=" * 55)
    app.run(host="0.0.0.0", port=5000, debug=False)
