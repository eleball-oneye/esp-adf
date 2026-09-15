#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
panel.py —— oneye 例程**验证面板**宿主侧小型服务（Python 3 标准库，零第三方依赖）

定位
====
把"例程跑出来的功能成果"在浏览器里**看得见、可回放、可留证**：
  * 按键例程：实时按键状态 + **历史响应**（本地检测 → `event/up` 上行 → 云端 ack 三态）
  * 板级核对：设备侧运行期自检逐行 `expect/actual/PASS-FAIL`（与串口 `[board-check]` 同源）
  * 链路/设备概况：固件版本、Wi-Fi IP、云端链路与收发计数
  * （随后接入）AEC 采集音频在 web 播放、SD 卡录音/录像在 web 选择与回放

取数三通道（互为兜底，面板按可用性自动降级）
============================================
  1) **设备 HTTP API**（主）：`GET /api/status|/api/selftest|/api/keys`（设备侧本地验证面）
  2) **MQTT**（云端视角）：订阅 `rmng/dev/+/event/up`（EMQX dev-stack 或内置 mock broker），
     与设备侧 `id` 关联 ⇒ 面板显示"云端是否真的收到"及端到端时延
  3) **串口**（无网兜底）：解析 `[key]` / `[board-check]` / `[sdk-event]` 行（POSIX 用 termios；
     Windows 需 `pip install pyserial`，缺失时自动禁用并提示）

⚠️ 边界：设备侧 `/api/*` 是**本地验证面**，不是云端设备面契约（不进 `backend/contracts`）；
   本面板只读，不改变设备行为；量产固件应关闭设备侧验证 API。

用法
====
  # 直连真机（设备已联网；IP 见串口日志或路由器）
  python3 panel.py --device korvo2-0001=http://192.168.1.50

  # 多板聚合 + MQTT（dev-stack EMQX）
  python3 panel.py --device a=http://192.168.1.50 --device b=http://192.168.1.51 \\
                   --mqtt 127.0.0.1:1883

  # 无硬件自检（内置 mock 设备 + mock broker，验证面板自身链路）
  python3 panel.py --self-test

浏览器打开 http://127.0.0.1:8787/
"""

from __future__ import annotations

import argparse
import json
import os
import re
import socket
import struct
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PANEL_VERSION = "0.1.0"
DEFAULT_PORT = 8787
DEFAULT_POLL_MS = 600
HISTORY_LIMIT = 200


# ============================================================================
# 极简 MQTT 3.1.1 客户端（订阅+发布，零依赖；仅供验证面板取"云端视角"）
# ============================================================================

class MiniMqtt:
    """只实现面板需要的子集：CONNECT / SUBSCRIBE(通配) / PUBLISH(qos0) / PINGREQ / 收包分发。"""

    def __init__(self, host: str, port: int = 1883, client_id: str = "oneye-panel",
                 keepalive: int = 30, on_message=None, log=print):
        self.host, self.port = host, int(port)
        self.client_id, self.keepalive = client_id, keepalive
        self.on_message = on_message
        self.log = log
        self.sock: socket.socket | None = None
        self.connected = False
        self.last_error: str | None = None
        self.subscribed: list[str] = []
        self._out_pid = 1
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None

    # ---------------------------------------------------------------- 报文编码
    @staticmethod
    def _str(s: str) -> bytes:
        b = s.encode("utf-8")
        return struct.pack("!H", len(b)) + b

    def _packet(self, ptype: int, flags: int, payload: bytes) -> bytes:
        out = bytearray([(ptype << 4) | flags])
        n = len(payload)
        while True:
            digit = n % 128
            n //= 128
            if n > 0:
                digit |= 0x80
            out.append(digit)
            if n == 0:
                break
        out += payload
        return bytes(out)

    # ---------------------------------------------------------------- 连接与循环
    def start(self):
        self._thread = threading.Thread(target=self._run, name="mini-mqtt", daemon=True)
        self._thread.start()

    def stop(self):
        self._stop.set()
        try:
            if self.sock:
                self.sock.close()
        except OSError:
            pass

    def subscribe(self, topic_filter: str, qos: int = 0):
        if topic_filter not in self.subscribed:
            self.subscribed.append(topic_filter)
        if not self.connected or not self.sock:
            return
        pid = self._out_pid
        self._out_pid += 1
        body = struct.pack("!H", pid) + self._str(topic_filter) + bytes([qos])
        self.sock.sendall(self._packet(8, 2, body))

    def publish(self, topic: str, payload: bytes, qos: int = 0):
        if not self.connected or not self.sock:
            return False
        body = self._str(topic) + payload
        try:
            self.sock.sendall(self._packet(3, 0, body))
            return True
        except OSError as exc:
            self.last_error = str(exc)
            return False

    def _connect_packet(self) -> bytes:
        flags = 0x02  # clean session
        var = self._str("MQTT") + bytes([4, flags]) + struct.pack("!H", self.keepalive)
        payload = self._str(self.client_id)
        return self._packet(1, 0, var + payload)

    def _run(self):
        while not self._stop.is_set():
            try:
                self.sock = socket.create_connection((self.host, self.port), timeout=5)
                self.sock.sendall(self._connect_packet())
                self.sock.settimeout(1.0)
                self.connected = True
                self.last_error = None
                self.log(f"[panel] MQTT 已连接 {self.host}:{self.port}")
                for f in list(self.subscribed):
                    self.subscribed.remove(f)
                    self.subscribe(f)
                last_ping = time.time()
                while not self._stop.is_set():
                    try:
                        data = self.sock.recv(4096)
                    except socket.timeout:
                        data = b""
                    if data == b"":
                        if self.sock.fileno() == -1:
                            break
                        continue
                    self._feed(data)
                    if time.time() - last_ping > self.keepalive / 2:
                        try:
                            self.sock.sendall(self._packet(12, 0, b""))
                        except OSError:
                            break
                        last_ping = time.time()
            except Exception as exc:  # noqa: BLE001 - 面板需容错
                self.last_error = str(exc)
                self.log(f"[panel] MQTT 连接失败：{exc}（1 s 后重试）")
            finally:
                self.connected = False
                try:
                    if self.sock:
                        self.sock.close()
                except OSError:
                    pass
                self.sock = None
            self._stop.wait(1.0)

    # ---------------------------------------------------------------- 收包
    def _feed(self, data: bytes):
        """按 MQTT 固定头逐包解析（长度用变长整数）。"""
        buf = getattr(self, "_buf", b"") + data
        while True:
            if len(buf) < 2:
                break
            ptype = buf[0] >> 4
            mult, value, idx = 1, 0, 1
            while True:
                if idx >= len(buf):
                    break
                digit = buf[idx]
                value += (digit & 127) * mult
                mult *= 128
                idx += 1
                if not (digit & 0x80):
                    break
            else:
                pass
            if idx < 2 or len(buf) < idx + value:
                break
            body = buf[idx:idx + value]
            buf = buf[idx + value:]
            if ptype == 3:                     # PUBLISH
                self._handle_publish(body)
            elif ptype == 2:                   # CONNACK
                if len(body) >= 2 and body[1] != 0:
                    self.last_error = f"CONNACK rc={body[1]}"
            elif ptype in (4, 5, 6, 7, 9, 11, 13):
                pass                           # PUBACK/PUBREC/PUBREL/PUBCOMP/SUBACK/UNSUBACK/PINGRESP
        self._buf = buf

    def _handle_publish(self, body: bytes):
        if len(body) < 2:
            return
        tlen = struct.unpack("!H", body[:2])[0]
        topic = body[2:2 + tlen].decode("utf-8", "replace")
        rest = body[2 + tlen:]
        qos = 0
        # puback 不做处理（面板全 QoS0 订阅）
        payload = rest[2:] if qos > 0 else rest
        if self.on_message:
            try:
                self.on_message(topic, payload)
            except Exception as exc:  # noqa: BLE001
                self.log(f"[panel] MQTT 回调异常：{exc}")


# ============================================================================
# 面板状态（设备聚合 + 云端事件关联）
# ============================================================================

def _now_ms() -> int:
    return int(time.time() * 1000)


class DeviceView:
    def __init__(self, name: str, base_url: str, node: str | None = None):
        self.name = name
        self.node = node or name
        self.base_url = base_url.rstrip("/")
        self.online = False
        self.last_error: str | None = None
        self.status: dict = {}
        self.selftest: dict = {}
        self.keys: dict = {}
        self.history: list[dict] = []
        self.media: dict = {}
        self.media_error: str | None = None
        self.cloud: dict[str, dict] = {}      # event id -> {ts_ms, recv_at_ms}
        self.serial_tail: list[str] = []
        self.last_poll_ms = 0
        self._media_ts = 0.0

    def _get(self, path: str, timeout: float = 2.0) -> dict | None:
        url = f"{self.base_url}{path}"
        try:
            with urllib.request.urlopen(url, timeout=timeout) as resp:
                raw = resp.read()
            return json.loads(raw.decode("utf-8", "replace"))
        except (urllib.error.URLError, OSError, ValueError) as exc:
            self.last_error = f"{path}: {exc}"
            return None

    def post_action(self, payload: dict, timeout: float = 5.0) -> dict | None:
        """触发设备侧本地动作（如 AEC 录音启停）。面板只做代理，便于浏览器同源调用。"""
        url = f"{self.base_url}/api/action"
        data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        req = urllib.request.Request(url, data=data, method="POST",
                                     headers={"Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                return json.loads(resp.read().decode("utf-8", "replace"))
        except (urllib.error.URLError, OSError, ValueError) as exc:
            self.media_error = f"/api/action: {exc}"
            return None

    def media_url(self, rel_url: str) -> str:
        """把设备返回的相对 url（media/<alias>/<path>）拼成可直接播放的绝对地址。"""
        return f"{self.base_url}/{rel_url.lstrip('/')}"

    def poll(self, media_interval_s: float = 3.0) -> None:
        st = self._get("/api/status")
        if st is None:
            self.online = False
            self.last_poll_ms = _now_ms()
            return
        self.online = True
        self.last_error = None
        self.status = st
        se = self._get("/api/selftest")
        if se is not None:
            self.selftest = se
        ks = self._get(f"/api/keys?limit={HISTORY_LIMIT}")
        if ks is not None:
            self.keys = ks
            hist = ks.get("history") or []
            for e in hist:
                if e.get("id") in self.cloud:
                    c = self.cloud[e["id"]]
                    e["cloud_seen"] = True
                    e["cloud_recv_ts_ms"] = c.get("recv_at_ms")
                    if c.get("recv_at_ms") and e.get("ts_ms"):
                        e["cloud_delta_ms"] = c["recv_at_ms"] - e["ts_ms"]
                else:
                    e["cloud_seen"] = False
            self.history = hist
        # 媒体列表（含 AEC 录音与 SD 卡媒体）——降频轮询
        now = time.time()
        if now - self._media_ts >= media_interval_s:
            self._media_ts = now
            ml = self._get("/media/list")
            if ml is not None:
                for f in ml.get("files", []):
                    f["play_url"] = self.media_url((f.get("item") or {}).get("url", ""))
                self.media = ml
                self.media_error = None
        self.last_poll_ms = _now_ms()

    def note_cloud_event(self, payload: bytes) -> dict | None:
        """云端视角收到 event/up：记录 id（用于与设备本地历史关联）。"""
        try:
            doc = json.loads(payload.decode("utf-8", "replace"))
        except ValueError:
            return None
        data = doc.get("data") or {}
        items = data.get("items") if isinstance(data, dict) else None
        recv_at = _now_ms()
        recorded = []
        for item in (items or []):
            eid = item.get("id")
            if not eid:
                continue
            self.cloud[eid] = {"ts_ms": doc.get("ts"), "recv_at_ms": recv_at}
            recorded.append({"id": eid, "type": item.get("type"), "data": item.get("data")})
        return {"raw": doc, "items": recorded} if recorded else None


class PanelState:
    def __init__(self, log=print):
        self.devices: dict[str, DeviceView] = {}
        self.log = log
        self.mqtt = {"enabled": False, "host": None, "connected": False,
                     "subscribed": [], "last_error": None, "cloud_events": 0}
        self.serial = {"enabled": False, "port": None, "connected": False, "last_error": None}
        self.serial_tail: list[str] = []
        self.started_at_ms = _now_ms()

    def add_device(self, name: str, base_url: str, node: str | None = None) -> DeviceView:
        dv = DeviceView(name, base_url, node)
        self.devices[name] = dv
        return dv

    def snapshot(self, history_limit: int = 100) -> dict:
        # 同步底层组件的实时状态（连接/错误）到对外快照
        if getattr(self, "mqtt_client", None) is not None:
            self.mqtt["connected"] = bool(self.mqtt_client.connected)
            self.mqtt["last_error"] = self.mqtt_client.last_error
        if getattr(self, "serial_reader", None) is not None:
            self.serial["connected"] = bool(self.serial_reader.connected)
            self.serial["last_error"] = self.serial_reader.last_error
        return {
            "panel": {"version": PANEL_VERSION, "now_ms": _now_ms(),
                      "uptime_ms": _now_ms() - self.started_at_ms},
            "mqtt": self.mqtt,
            "serial": self.serial,
            "serial_tail": self.serial_tail[-60:],
            "devices": [{
                "name": d.name, "node": d.node, "http": d.base_url,
                "online": d.online, "last_error": d.last_error,
                "last_poll_ms": d.last_poll_ms,
                "status": d.status, "selftest": d.selftest,
                "keys": d.keys, "history": d.history[:history_limit],
                "media": {
                    "count": (d.media or {}).get("count", 0),
                    "rec_root": (d.media or {}).get("rec_root"),
                    "aec_enabled": (d.media or {}).get("aec_enabled"),
                    "aec_recording": (d.media or {}).get("aec_recording"),
                    "error": d.media_error,
                    "files": list((d.media or {}).get("files", [])),
                },
                "cloud_event_count": len(d.cloud),
                "serial_tail": d.serial_tail[-30:],
            } for d in self.devices.values()],
        }


# ============================================================================
# 串口兜底（POSIX termios；Windows 可选 pyserial）
# ============================================================================

class SerialReader:
    def __init__(self, port: str, baud: int = 115200, on_line=None, log=print):
        self.port, self.baud = port, baud
        self.on_line = on_line
        self.log = log
        self.connected = False
        self.last_error: str | None = None
        self._stop = threading.Event()

    def start(self):
        threading.Thread(target=self._run, name="serial", daemon=True).start()

    def stop(self):
        self._stop.set()

    def _open(self):
        if os.name == "posix":
            import termios  # noqa: PLC0415 - 仅 POSIX 需要
            fd = os.open(self.port, os.O_RDONLY | os.O_NOCTTY | os.O_NONBLOCK)
            attrs = termios.tcgetattr(fd)
            speed = getattr(termios, f"B{self.baud}")
            attrs[4] = attrs[5] = speed
            attrs[2] = termios.CLOCAL | termios.CREAD
            termios.tcsetattr(fd, termios.TCSANOW, attrs)
            return fd
        try:
            import serial  # type: ignore  # noqa: PLC0415
        except ImportError as exc:
            raise RuntimeError("Windows 串口需 pip install pyserial") from exc
        return serial.Serial(self.port, self.baud, timeout=0.5)

    def _read(self, handle) -> bytes:
        if os.name == "posix":
            try:
                return os.read(handle, 4096)
            except BlockingIOError:
                return b""
        n = getattr(handle, "in_waiting", 0)
        return handle.read(n or 1)

    def _run(self):
        buf = b""
        while not self._stop.is_set():
            handle = None
            try:
                handle = self._open()
                self.connected = True
                self.last_error = None
                self.log(f"[panel] 串口已打开 {self.port}@{self.baud}")
                while not self._stop.is_set():
                    chunk = self._read(handle)
                    if not chunk:
                        time.sleep(0.1)
                        continue
                    buf += chunk
                    while b"\n" in buf:
                        line, buf = buf.split(b"\n", 1)
                        text = line.decode("utf-8", "replace").rstrip("\r")
                        if text and self.on_line:
                            self.on_line(text)
            except Exception as exc:  # noqa: BLE001
                self.last_error = str(exc)
                self.log(f"[panel] 串口不可用：{exc}（5 s 后重试）")
                self._stop.wait(5.0)
            finally:
                self.connected = False
                try:
                    if handle is not None:
                        if os.name == "posix":
                            os.close(handle)
                        else:
                            handle.close()
                except Exception:  # noqa: BLE001
                    pass


# ============================================================================
# 页面（单页，内嵌；无外部 CDN）
# ============================================================================

PAGE = r"""<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>oneye 例程验证面板 · Korvo-2</title>
<style>
 :root{--bg:#0f1419;--card:#171d24;--line:#26303a;--fg:#e6edf3;--mut:#8b98a5;
       --ok:#2ea043;--warn:#d29922;--err:#f85149;--acc:#388bfd;}
 *{box-sizing:border-box}
 body{margin:0;background:var(--bg);color:var(--fg);font:14px/1.55 -apple-system,"Segoe UI",Roboto,"Microsoft YaHei",sans-serif}
 header{display:flex;gap:12px;align-items:center;flex-wrap:wrap;padding:12px 18px;border-bottom:1px solid var(--line);background:#12181f;position:sticky;top:0;z-index:5}
 h1{font-size:16px;margin:0;font-weight:600}
 .chip{padding:2px 8px;border:1px solid var(--line);border-radius:999px;color:var(--mut);font-size:12px}
 .chip.on{color:#0b1016;background:var(--ok);border-color:var(--ok)}
 .chip.off{color:var(--mut)}
 .chip.err{color:#0b1016;background:var(--err);border-color:var(--err)}
 main{padding:16px 18px 60px;display:grid;gap:16px;max-width:1180px}
 .grid2{display:grid;gap:16px;grid-template-columns:1fr 1fr}
 @media(max-width:900px){.grid2{grid-template-columns:1fr}}
 .card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:14px}
 .card h2{font-size:14px;margin:0 0 10px;color:var(--fg);font-weight:600;display:flex;gap:8px;align-items:center}
 .card h2 small{color:var(--mut);font-weight:400}
 table{width:100%;border-collapse:collapse;font-size:13px}
 th,td{text-align:left;padding:5px 8px;border-bottom:1px solid var(--line);white-space:nowrap}
 th{color:var(--mut);font-weight:500}
 .keys{display:grid;grid-template-columns:repeat(3,1fr);gap:8px}
 .key{border:1px solid var(--line);border-radius:8px;padding:10px;text-align:center;background:#131920}
 .key.active{border-color:var(--acc);box-shadow:0 0 0 2px rgba(56,139,253,.25)}
 .key b{display:block;font-size:13px}
 .key span{color:var(--mut);font-size:11px}
 .pill{padding:1px 7px;border-radius:999px;font-size:11px;border:1px solid var(--line)}
 .pill.ok{background:rgba(46,160,67,.15);color:#56d364;border-color:rgba(46,160,67,.4)}
 .pill.pend{background:rgba(210,153,34,.15);color:#e3b341;border-color:rgba(210,153,34,.4)}
 .pill.bad{background:rgba(248,81,73,.15);color:#ff7b72;border-color:rgba(248,81,73,.4)}
 .mut{color:var(--mut)}
 .kv{display:grid;grid-template-columns:auto 1fr;gap:4px 12px;font-size:13px}
 .kv div:nth-child(odd){color:var(--mut)}
 pre{margin:0;max-height:220px;overflow:auto;font-size:12px;color:#a9b6c3;background:#111820;border:1px solid var(--line);border-radius:8px;padding:8px}
 button{background:#1f2630;color:var(--fg);border:1px solid var(--line);border-radius:6px;padding:5px 10px;cursor:pointer}
 button:hover{border-color:var(--acc)}
 .flash{animation:fl .6s ease-out}
 @keyframes fl{from{background:rgba(56,139,253,.25)}to{background:transparent}}
</style></head><body>
<header>
  <h1>oneye 例程验证面板 · ESP32-S3-Korvo-2</h1>
  <span class="chip" id="c-mqtt">MQTT</span>
  <span class="chip" id="c-serial">串口</span>
  <span class="chip" id="c-dev">设备</span>
  <span class="mut" id="c-upd">—</span>
  <button id="btn-refresh">立即刷新</button>
</header>
<main>
  <div class="card">
    <h2>按键：实时状态 <small>本地检测（设备 HTTP）</small></h2>
    <div class="keys" id="keys"></div>
    <div class="kv" id="keymeta" style="margin-top:10px"></div>
  </div>

  <div class="grid2">
    <div class="card">
      <h2>按键：历史响应 <small>本地检测 → event/up 上行 → 云端 ack</small></h2>
      <table><thead><tr><th>#</th><th>时间</th><th>按键</th><th>动作</th><th>上行</th><th>云端确认</th><th>端到端</th></tr></thead>
      <tbody id="hist"></tbody></table>
    </div>
    <div class="card">
      <h2>板级参数核对 <small>设备运行期自检（与串口 [board-check] 同源）</small></h2>
      <div class="kv" id="stmeta"></div>
      <table><thead><tr><th>项</th><th>期望</th><th>实测</th><th>结论</th></tr></thead>
      <tbody id="selftest"></tbody></table>
    </div>
  </div>

  <div class="grid2">
    <div class="card">
      <h2>设备与链路</h2>
      <div class="kv" id="dev"></div>
    </div>
    <div class="card">
      <h2>串口兜底 <small>无网时仍可核对</small></h2>
      <pre id="serial">（未启用串口：加 --serial COM3@115200 或 /dev/ttyUSB0@115200）</pre>
    </div>
  </div>

  <div class="card">
    <h2>音频：AEC 采集与 SD 卡媒体 <small>设备 HTTP 直供（支持 Range，可拖动播放）</small></h2>
    <div class="kv" id="aecmeta"></div>
    <div style="display:flex;gap:8px;flex-wrap:wrap;margin:10px 0">
      <button onclick="rec(5)">录 5 s</button>
      <button onclick="rec(10)">录 10 s</button>
      <button onclick="rec(30)">录 30 s</button>
      <button onclick="act('aec_stop')">停止录音</button>
      <button onclick="tick()">刷新列表</button>
      <span class="mut" id="actmsg"></span>
    </div>
    <table><thead><tr><th>文件</th><th>类型</th><th>大小</th><th>修改时间</th><th>操作</th></tr></thead>
    <tbody id="media"></tbody></table>
    <div id="player" style="margin-top:10px"></div>
  </div>
</main>
<script>
const $ = (id)=>document.getElementById(id);
const ACT = {click:"短按", click_release:"短按释放", press:"长按", press_release:"长按释放", unknown:"未知"};
const KEYNAME = {rec:"REC", mute:"MUTE", play:"PLAY", set:"SET", mode:"MODE", volup:"VOL+", voldown:"VOL-", unknown:"?"};
let lastSeq = -1;

async function tick(){
  let st;
  try{ st = await (await fetch('/api/state')).json(); }
  catch(e){ $('c-upd').textContent = '面板服务不可达'; return; }
  const dev = (st.devices||[]).find(d=>d.online) || (st.devices||[])[0] || {};
  currentDev = dev.name || null;
  const keys = dev.keys||{}, ks = keys.history||[];

  // 顶部状态
  $('c-mqtt').className = 'chip ' + (st.mqtt.connected ? 'on' : (st.mqtt.enabled ? 'err' : 'off'));
  $('c-mqtt').textContent = 'MQTT ' + (st.mqtt.enabled ? (st.mqtt.connected ? (st.mqtt.host||'') : '未连接') : '未启用');
  $('c-serial').className = 'chip ' + (st.serial.connected ? 'on' : (st.serial.enabled ? 'err' : 'off'));
  $('c-serial').textContent = '串口 ' + (st.serial.enabled ? (st.serial.connected ? (st.serial.port||'') : '未连接') : '未启用');
  $('c-dev').className = 'chip ' + (dev.online ? 'on' : 'err');
  $('c-dev').textContent = '设备 ' + (dev.name||'—') + (dev.online ? ' 在线' : ' 离线');
  $('c-upd').textContent = new Date().toLocaleTimeString();

  // 按键面板
  const cur = keys.current||null;
  const labels = (keys.keys||[]).map(k=>k.label);
  $('keys').innerHTML = (labels.length?labels:['volup','voldown','set','play','mode','rec']).map(l=>{
    const on = cur && cur.key===l;
    const age = on ? Math.max(0, Date.now()-((dev.last_poll_ms||0)-(0))) : 0;
    return `<div class="key ${on?'active':''}"><b>${KEYNAME[l]||l}</b>
      <span>${on?(ACT[cur.action]||cur.action):'待触发'}</span></div>`;
  }).join('');
  const c = keys.counters||{};
  $('keymeta').innerHTML = `
    <div>当前</div><div>${cur? `${KEYNAME[cur.key]||cur.key} / ${ACT[cur.action]||cur.action} <span class="pill ${cur.uplink==='acked'?'ok':(cur.uplink==='failed'?'bad':'pend')}">${cur.uplink}</span>` : '<span class="mut">尚无按键</span>'}</div>
    <div>计数</div><div>共 ${c.total||0} 次（短按 ${c.click||0} / 长按 ${c.press||0} / 释放 ${(c.click_release||0)+(c.press_release||0)}）</div>
    <div>云端事件</div><div>${dev.cloud_event_count||0} 条（MQTT 视角）</div>`;

  // 历史响应
  const rows = ks.map(e=>{
    const up = e.uplink||'pending';
    const upCls = up==='acked'?'ok':(up==='failed'?'bad':'pend');
    const cloud = e.cloud_seen ? '<span class="pill ok">已收到</span>' : '<span class="pill pend">未见</span>';
    const delta = (e.cloud_delta_ms!==undefined) ? (e.cloud_delta_ms+' ms') : '—';
    const t = e.ts_ms ? new Date(e.ts_ms).toLocaleTimeString() : '—';
    return `<tr class="${e.seq>lastSeq?'flash':''}"><td>${e.seq}</td><td>${t}</td>
      <td>${KEYNAME[e.key]||e.key}</td><td>${ACT[e.action]||e.action}</td>
      <td><span class="pill ${upCls}">${up}</span></td><td>${cloud}</td><td class="mut">${delta}</td></tr>`;
  });
  $('hist').innerHTML = rows.join('') || '<tr><td colspan="7" class="mut">尚无按键事件（按下板上按键，或检查设备 HTTP 是否可达）</td></tr>';
  if(ks.length) lastSeq = Math.max(lastSeq, ks[0].seq||0);

  // 自检表
  const se = dev.selftest||{};
  $('stmeta').innerHTML = `<div>结果</div><div>${se.total!==undefined? `${se.total} 项，失败 ${se.failed}` : '<span class="mut">未取到</span>'}
      ${se.failed===0&&se.total>0?'<span class="pill ok">全通过</span>':(se.failed>0?'<span class="pill bad">有失败</span>':'')}</div>
    <div>说明</div><div class="mut">${se.note||''}</div>`;
  $('selftest').innerHTML = (se.items||[]).map(it=>`<tr><td>${it.item}</td><td class="mut">${it.expect}</td>
      <td class="mut">${it.actual}</td><td>${it.pass?'<span class="pill ok">PASS</span>':'<span class="pill bad">FAIL</span>'}</td></tr>`).join('')
    || '<tr><td colspan="4" class="mut">未取到自检明细</td></tr>';

  // 设备概况
  const s = dev.status||{};
  const wifi = s.wifi||{}, cloud = s.cloud||{};
  $('dev').innerHTML = `
    <div>设备</div><div>${dev.name} <span class="mut">${dev.http||''}</span></div>
    <div>固件 / 板卡</div><div>${s.fw||'—'} / ${s.board||'—'}</div>
    <div>运行时长</div><div>${s.uptime_ms!==undefined? (s.uptime_ms/1000).toFixed(1)+' s':'—'}</div>
    <div>Wi-Fi</div><div>${wifi.connected? '已连接 '+ (wifi.ip||'') : '<span class="pill pend">未连接</span>'}</div>
    <div>云端链路</div><div>${cloud.link_up? '<span class="pill ok">已连接</span> '+ (cloud.transport||'') : '<span class="pill pend">未连接（按键仍可本地核对）</span>'}</div>
    <div>收发帧</div><div>tx ${cloud.tx_frames||0} / rx ${cloud.rx_frames||0}</div>
    <div>错误</div><div class="mut">${dev.last_error||'—'}</div>`;

  // 音频 / 媒体卡片
  const media = dev.media || {};
  const aec = (dev.status && dev.status.aec) || {};
  const sd = (dev.status && dev.status.media && dev.status.media.sd_mounted);
  const pl = (dev.status && dev.status.player) || {};
  $('aecmeta').innerHTML = `
    <div>AEC 采集</div><div>${media.aec_enabled===false? '<span class="pill bad">未启用</span>' :
        (media.aec_recording? '<span class="pill pend">录音中</span>' : '<span class="pill ok">就绪</span>')}
      <span class="mut">落盘：${media.rec_root || '—'}（${sd? 'SD 卡' : 'SPIFFS 兜底'}）</span></div>
    <div>板上回放</div><div>${pl.playing? `<span class="pill ok">播放中</span> ${pl.path||''} <span class="mut">${pl.codec||''} ${pl.rate_hz?pl.rate_hz+' Hz':''} ${pl.channels?pl.channels+'ch':''} ${(pl.elapsed_ms? (pl.elapsed_ms/1000).toFixed(1)+' s':'')}</span>`
        : `<span class="mut">空闲（${pl.msg||'—'}）</span>`}
      音量 <input type="range" min="0" max="100" value="${pl.volume!==undefined?pl.volume:80}"
        onchange="act('set_volume', null, null, this.value)" style="vertical-align:middle;width:110px">
      <button onclick="act('stop')">停止播放</button></div>
    <div>最近录音</div><div>${aec.last_file? `${aec.last_file} <span class="mut">${(aec.last_bytes||0)} 字节</span>` : '<span class="mut">尚无</span>'}</div>
    <div>文件数</div><div>${media.count||0} 个可播放文件${media.error? ` <span class="pill bad">${media.error}</span>`:''}</div>`;
  const rows2 = (media.files||[]).map(f=>{
    const sz = f.size>=1024? (f.size/1024).toFixed(1)+' KB' : (f.size||0)+' B';
    const mt = f.mtime? new Date(f.mtime*1000).toLocaleString() : '—';
    const act = f.kind==='audio' ? `<button onclick="play('${f.play_url}')">网页播放</button>`
              : (f.kind==='image'||f.kind==='video' ? `<button onclick="preview('${f.play_url}','${f.kind}')">网页预览</button>` : '');
    const board = f.playable_on_board
      ? `<button onclick="act('play', null, '${f.device_path}')" title="在开发板扬声器上播放">板上播放</button>` : '';
    return `<tr><td>${f.name}</td><td class="mut">${f.kind}</td><td class="mut">${sz}</td><td class="mut">${mt}</td>
      <td>${act} ${board} <a href="${f.play_url}" download target="_blank"><button>下载</button></a></td></tr>`;
  });
  $('media').innerHTML = rows2.join('') ||
    '<tr><td colspan="5" class="mut">尚无媒体文件（点“录 5 s”触发 AEC 采集；SD 卡文件需插入卡并启用 CONFIG_ONEYE_FW_ENABLE_SDCARD）</td></tr>';

  // 串口
  if((st.serial_tail||[]).length) $('serial').textContent = st.serial_tail.join('\n');
}

let currentDev = null;
function play(url){
  $('player').innerHTML = `<audio controls autoplay style="width:100%" src="${url}"></audio>
    <div class="mut" style="font-size:12px;margin-top:4px">${url}</div>`;
}
function preview(url, kind){
  $('player').innerHTML = kind==='video'
    ? `<video controls autoplay style="max-width:100%" src="${url}"></video>`
    : `<img src="${url}" style="max-width:100%;border-radius:8px">`;
}
async function act(op, duration_s, path, volume){
  if(!currentDev) return;
  $('actmsg').textContent = '执行中…';
  const body = {op};
  if (duration_s) body.duration_s = duration_s;
  if (path) body.path = path;
  if (volume !== undefined && volume !== null) body.volume = Number(volume);
  try{
    const r = await fetch(`/api/action?device=${encodeURIComponent(currentDev)}`, {
      method:'POST', headers:{'Content-Type':'application/json'}, body: JSON.stringify(body)});
    const j = await r.json();
    $('actmsg').textContent = (j.ok? '✔ ' : '✗ ') + (j.msg||'') + (j.file? ` → ${j.file}`:'');
  }catch(e){ $('actmsg').textContent = '✗ ' + e; }
  setTimeout(tick, 400);
}
function rec(seconds){ act('aec_start', seconds); }

$('btn-refresh').onclick = tick;
tick(); setInterval(tick, __POLL_MS__);
</script></body></html>
"""


# ============================================================================
# HTTP 服务
# ============================================================================

def make_handler(state: PanelState, poll_ms: int):
    class Handler(BaseHTTPRequestHandler):
        server_version = f"oneye-panel/{PANEL_VERSION}"

        def log_message(self, fmt, *args):   # 静默访问日志（面板自身可用 --verbose 打开）
            if os.environ.get("PANEL_VERBOSE"):
                state.log("[panel] " + fmt % args)

        def _send(self, code: int, body: bytes, ctype: str = "application/json; charset=utf-8"):
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            try:
                self.wfile.write(body)
            except (BrokenPipeError, ConnectionResetError):
                pass

        def do_GET(self):  # noqa: N802
            parsed = urllib.parse.urlparse(self.path)
            path = parsed.path
            if path in ("/", "/index.html"):
                html = PAGE.replace("__POLL_MS__", str(poll_ms)).encode("utf-8")
                self._send(200, html, "text/html; charset=utf-8")
            elif path == "/api/state":
                self._send(200, json.dumps(state.snapshot(), ensure_ascii=False).encode("utf-8"))
            elif path == "/api/media":
                doc = {d.name: {"http": d.base_url,
                                "files": list((d.media or {}).get("files", [])),
                                "aec": d.status.get("aec", {}),
                                "rec_root": (d.media or {}).get("rec_root"),
                                "error": d.media_error}
                       for d in state.devices.values()}
                self._send(200, json.dumps(doc, ensure_ascii=False).encode("utf-8"))
            elif path == "/api/health":
                self._send(200, json.dumps({"ok": True, "version": PANEL_VERSION}).encode("utf-8"))
            else:
                self._send(404, b'{"error":"not_found"}')

        def do_POST(self):  # noqa: N802
            parsed = urllib.parse.urlparse(self.path)
            q = urllib.parse.parse_qs(parsed.query)
            if parsed.path != "/api/action":
                self._send(404, b'{"error":"not_found"}')
                return
            length = int(self.headers.get("Content-Length") or 0)
            raw = self.rfile.read(length) if length > 0 else b"{}"
            try:
                payload = json.loads(raw.decode("utf-8", "replace") or "{}")
            except ValueError:
                self._send(400, b'{"error":"invalid_json"}')
                return
            name = (q.get("device") or [None])[0]
            dv = state.devices.get(name) if name else next(iter(state.devices.values()), None)
            if dv is None:
                self._send(404, b'{"error":"no_device"}')
                return
            resp = dv.post_action(payload)
            if resp is None:
                resp = {"ok": False, "msg": dv.media_error or "action failed"}
            self._send(200, json.dumps(resp, ensure_ascii=False).encode("utf-8"))

    return Handler


# ============================================================================
# 主流程
# ============================================================================

def start_panel(state: PanelState, port: int, poll_ms: int, devices: list[tuple[str, str, str | None]],
                mqtt: str | None = None, serial: str | None = None, log=print):
    """启动面板全部组件；返回 (httpd, 线程列表) 供自检复用。"""
    for name, url, node in devices:
        state.add_device(name, url, node)

    # 设备轮询（同时把底层组件状态同步到对外快照，供自检与 UI 使用）
    def poller():
        while True:
            for dv in list(state.devices.values()):
                try:
                    dv.poll()
                except Exception as exc:  # noqa: BLE001
                    dv.online = False
                    dv.last_error = str(exc)
            mqtt_c = getattr(state, "mqtt_client", None)
            if mqtt_c is not None:
                state.mqtt["connected"] = bool(mqtt_c.connected)
                state.mqtt["last_error"] = mqtt_c.last_error
            ser = getattr(state, "serial_reader", None)
            if ser is not None:
                state.serial["connected"] = bool(ser.connected)
                state.serial["last_error"] = ser.last_error
            time.sleep(max(0.15, poll_ms / 1000.0))

    threads = [threading.Thread(target=poller, name="poller", daemon=True)]

    # MQTT（云端视角）
    mqtt_client = None
    if mqtt:
        host, _, port_s = mqtt.partition(":")
        mqtt_client = MiniMqtt(host, int(port_s or 1883), on_message=None, log=log)
        state.mqtt.update({"enabled": True, "host": f"{host}:{port_s or 1883}"})
        topics = ["rmng/dev/+/event/up", "rmng/dev/+/status/up"]

        def on_msg(topic: str, payload: bytes):
            parts = topic.split("/")
            if len(parts) >= 5:
                node = parts[2]
                face = "/".join(parts[3:])
                for dv in state.devices.values():
                    if dv.node == node:
                        if face == "event/up":
                            if dv.note_cloud_event(payload):
                                state.mqtt["cloud_events"] = state.mqtt.get("cloud_events", 0) + 1
                        dv.cloud.setdefault("_last_topic", {})["topic"] = topic

        mqtt_client.on_message = on_msg
        for t in topics:
            mqtt_client.subscribe(t)
        state.mqtt["subscribed"] = list(topics)
        mqtt_client.start()

    # 串口兜底
    serial_reader = None
    if serial:
        port, _, baud_s = serial.partition("@")
        baud = int(baud_s or 115200)
        state.serial.update({"enabled": True, "port": f"{port}@{baud}"})

        def on_line(line: str):
            state.serial_tail.append(line)
            if len(state.serial_tail) > 2000:
                del state.serial_tail[:500]
            # 串口行 → 本地历史兜底（当设备 HTTP 不可达时仍有记录）
            m = re.search(r"\[key\]\s+(\S+)/(\S+)", line)
            if m:
                for dv in state.devices.values():
                    if not dv.online:
                        dv.serial_tail.append(line)
                        if len(dv.serial_tail) > 200:
                            del dv.serial_tail[:50]

        serial_reader = SerialReader(port, baud, on_line=on_line, log=log)
        serial_reader.start()

    httpd = ThreadingHTTPServer(("0.0.0.0", port), make_handler(state, poll_ms))
    threads.append(threading.Thread(target=httpd.serve_forever, name="http", daemon=True))
    state.mqtt_client = mqtt_client           # 供 poller/snapshot 同步实时连接状态
    state.serial_reader = serial_reader
    for t in threads:
        t.start()
    log(f"[panel] 面板已启动：http://127.0.0.1:{port}/"
        f"  设备 {len(devices)} 台，MQTT {'开' if mqtt else '关'}，串口 {'开' if serial else '关'}")
    return httpd, mqtt_client, serial_reader


def load_devices(args) -> list[tuple[str, str, str | None]]:
    devices: list[tuple[str, str, str | None]] = []
    if args.devices_file and os.path.exists(args.devices_file):
        with open(args.devices_file, "r", encoding="utf-8") as fh:
            doc = json.load(fh)
        for item in doc.get("devices", []):
            devices.append((item["name"], item["http"], item.get("node")))
    for spec in args.device or []:
        name, _, url = spec.partition("=")
        if not url:
            name, _, url = ("dev", "", spec)
        node = name
        if "#" in name:
            name, _, node = name.partition("#")
        devices.append((name, url, node))
    return devices


def self_test(verbose: bool = True) -> int:
    """无硬件自检：内置 mock 设备 + mock broker，验证面板三条链路与关联逻辑。"""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import mock_device  # noqa: PLC0415

    log = print if verbose else (lambda *a, **k: None)
    checks: list[tuple[str, bool, str]] = []

    def check(name: str, ok: bool, note: str = ""):
        checks.append((name, ok, note))
        log(f"  [{'PASS' if ok else 'FAIL'}] {name} {note}")

    dev_port, broker_port = 18080, 18830
    mock = mock_device.MockDevice(http_port=dev_port, broker_port=broker_port,
                                  node="korvo2-selftest", log=log)
    mock.start()
    time.sleep(0.4)

    state = PanelState(log=log)
    httpd, mqtt_client, _ = start_panel(
        state, port=18787, poll_ms=250,
        devices=[("korvo2-selftest", f"http://127.0.0.1:{dev_port}", "korvo2-selftest")],
        mqtt=f"127.0.0.1:{broker_port}", log=log)
    try:
        # 1) 设备 HTTP 链路
        deadline = time.time() + 8
        dv = state.devices["korvo2-selftest"]
        while time.time() < deadline and not dv.online:
            time.sleep(0.2)
        check("设备 HTTP 可达（/api/status）", dv.online, dv.last_error or "")
        check("自检明细取到（/api/selftest）", bool(dv.selftest.get("items")),
              f"{len(dv.selftest.get('items', []))} 行")
        check("固件标识取到", bool(dv.status.get("fw")), str(dv.status.get("fw")))

        # 2) MQTT 链路
        deadline = time.time() + 8
        while time.time() < deadline and not state.mqtt.get("connected"):
            time.sleep(0.2)
        check("MQTT 已连接（mock broker）", bool(state.mqtt.get("connected")),
              state.mqtt.get("last_error") or "")

        # 3) 触发按键 → 本地历史 + 云端关联
        mock.press("play", "click")
        time.sleep(0.3)
        mock.press("volup", "press")
        time.sleep(0.3)
        mock.press("rec", "click")
        deadline = time.time() + 10
        while time.time() < deadline:
            if len(dv.history) >= 3 and sum(1 for e in dv.history if e.get("cloud_seen")) >= 3:
                break
            time.sleep(0.2)
        check("按键历史 ≥3 条", len(dv.history) >= 3, f"{len(dv.history)} 条")
        check("历史按新→旧排序",
              all((dv.history[i].get("seq", 0) > dv.history[i + 1].get("seq", 0))
                  for i in range(len(dv.history) - 1)) if len(dv.history) > 1 else False)
        cloud_ok = sum(1 for e in dv.history if e.get("cloud_seen"))
        check("云端确认关联（MQTT 与本地 id 对齐）", cloud_ok >= 3, f"{cloud_ok} 条已确认")
        delta_ok = all(e.get("cloud_delta_ms") is not None for e in dv.history if e.get("cloud_seen"))
        check("端到端时延可计算", delta_ok)
        actions = {e["action"] for e in dv.history}
        check("动作类型覆盖 click/press", {"click", "press"} <= actions, str(sorted(actions)))

        # 4) 面板自身 HTTP
        try:
            with urllib.request.urlopen("http://127.0.0.1:18787/api/health", timeout=3) as r:
                health = json.loads(r.read().decode())
            check("/api/health 可用", health.get("ok") is True, str(health))
        except Exception as exc:  # noqa: BLE001
            check("/api/health 可用", False, str(exc))
        try:
            with urllib.request.urlopen("http://127.0.0.1:18787/", timeout=3) as r:
                page = r.read().decode("utf-8", "replace")
            check("首页渲染可用", "按键：实时状态" in page and "__POLL_MS__" not in page)
            check("首页含音频/AEC 卡片", "音频：AEC 采集" in page and "/api/action" in page)
        except Exception as exc:  # noqa: BLE001
            check("首页渲染可用", False, str(exc))

        # 4b) 媒体面：列表 / 播放（Range） / 动作触发
        deadline = time.time() + 6
        while time.time() < deadline and not (dv.media or {}).get("files"):
            time.sleep(0.2)
        files = (dv.media or {}).get("files") or []
        check("媒体列表非空（/media/list）", len(files) >= 1, f"{len(files)} 个文件")
        play_url = files[0].get("play_url") if files else None
        check("播放地址为设备绝对地址", bool(play_url and play_url.startswith("http")))
        head_ok = False
        if play_url:
            try:
                req = urllib.request.Request(play_url, headers={"Range": "bytes=0-43"})
                with urllib.request.urlopen(req, timeout=4) as r:
                    body = r.read()
                    head_ok = (r.status == 206 and r.headers.get("Content-Range") is not None
                               and body[:4] == b"RIFF")
            except Exception as exc:  # noqa: BLE001
                check("Range 请求（拖动播放前提）", False, str(exc))
            else:
                check("Range 请求（拖动播放前提）", head_ok,
                      "206 + Content-Range + RIFF 头" if head_ok else "响应不符合预期")
        before = len(files)
        act = dv.post_action({"op": "aec_start", "duration_s": 1})
        check("触发 AEC 采集（POST /api/action）", bool(act and act.get("ok")),
              (act or {}).get("file") or (act or {}).get("msg", ""))
        time.sleep(1.6)
        dv.poll(media_interval_s=0)
        after = len((dv.media or {}).get("files") or [])
        check("采集后媒体列表增长", after > before, f"{before} → {after}")
        stop = dv.post_action({"op": "aec_stop"})
        check("停止采集（aec_stop）", bool(stop and stop.get("ok")), (stop or {}).get("msg", ""))

        # 4c) 板上回放（SD 卡选择 → 板上播放 → 停止）
        playable = [f for f in files if f.get("playable_on_board")]
        check("SD 卡媒体标记为可板上回放", len(playable) >= 1,
              f"{len(playable)} 个（device_path={playable[0].get('device_path') if playable else '-'}）")
        if playable:
            p = dv.post_action({"op": "play", "path": playable[0]["device_path"]})
            check("触发板上播放（POST /api/action play）", bool(p and p.get("ok")),
                  (p or {}).get("msg", ""))
            time.sleep(0.5)
            dv.poll()
            pl = (dv.status or {}).get("player", {})
            check("播放状态回显（playing=true + 文件名）",
                  bool(pl.get("playing")) and playable[0]["device_path"] in (pl.get("path") or ""),
                  f"{pl.get('playing')} / {pl.get('path')}")
            v = dv.post_action({"op": "set_volume", "volume": 42})
            check("音量设置（set_volume）", bool(v and v.get("ok") and v.get("volume") == 42),
                  str((v or {}).get("volume")))
            s2 = dv.post_action({"op": "stop"})
            check("停止播放（stop）", bool(s2 and s2.get("ok")), (s2 or {}).get("msg", ""))
            bad = dv.post_action({"op": "play", "path": "/spiffs/rec/aec-00001.wav"})
            check("非 SD 路径被拒绝（/spiffs 不上板播放）",
                  bool(bad) and not bad.get("ok"), (bad or {}).get("msg", ""))

        # 5) 设备离线时的降级（不崩、状态可见）
        mock.stop()
        time.sleep(1.2)
        dv.poll()
        check("设备离线时面板仍可用（online=false 且带错误）",
              dv.online is False, dv.last_error or "")
    finally:
        try:
            httpd.shutdown()
        except Exception:  # noqa: BLE001
            pass
        if mqtt_client:
            mqtt_client.stop()
        mock.stop()

    failed = [c for c in checks if not c[1]]
    print(f"\n自检汇总：{len(checks)} 项，通过 {len(checks) - len(failed)}，失败 {len(failed)}")
    for name, _ok, note in failed:
        print(f"  ✗ {name} {note}")
    return 1 if failed else 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="oneye 例程验证面板（宿主侧小型服务；零第三方依赖）")
    ap.add_argument("--device", action="append", metavar="NAME=URL",
                    help="设备（可重复）：name=http://ip；node 可用 name#node 指定")
    ap.add_argument("--devices-file", default=os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                          "devices.json"),
                    help="设备清单 JSON（可选，缺省读同目录 devices.json）")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT, help=f"面板端口（缺省 {DEFAULT_PORT}）")
    ap.add_argument("--poll-ms", type=int, default=DEFAULT_POLL_MS, help="设备轮询间隔（缺省 600 ms）")
    ap.add_argument("--mqtt", metavar="HOST[:PORT]", help="MQTT broker（dev-stack EMQX 或 mock）")
    ap.add_argument("--serial", metavar="PORT[@BAUD]", help="串口兜底（无网时核对）")
    ap.add_argument("--self-test", action="store_true", help="内置 mock 设备自检（无硬件）")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()

    devices = load_devices(args)
    if not devices:
        print("未配置设备：用 --device name=http://<设备IP> 或 devices.json；"
              "无硬件时可用 --self-test 验证面板自身。", file=sys.stderr)
        return 2
    state = PanelState()
    httpd, mqtt_client, serial_reader = start_panel(state, args.port, args.poll_ms, devices,
                                                    mqtt=args.mqtt, serial=args.serial)
    try:
        while True:
            time.sleep(1.0)
    except KeyboardInterrupt:
        print("\n[panel] 退出中…")
    finally:
        httpd.shutdown()
        if mqtt_client:
            mqtt_client.stop()
        if serial_reader:
            serial_reader.stop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
