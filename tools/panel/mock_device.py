#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
mock_device.py —— 验证面板用的**模拟 Korvo-2 设备**（零依赖）

用途
====
没有真机时也能端到端验证面板（以及设备侧 API 口径）：

  * 提供与固件**同形**的本地验证 API：`/api/ping`、`/api/status`、`/api/selftest`、`/api/keys`
  * 内置**极简 MQTT broker**（默认 18830），并自扮云端：收到 `rmng/dev/<node>/event/up` 后，
    回一条 `rmng/dev/<node>/event/down` ack（`data.ref` = 上行事件 id）⇒ 触发设备侧 ack 关联，
    面板即可看到「本地检测 → 已上行 → 云端 ack」三态
  * 触发按键：`GET /mock/press?key=play&action=click`（面板自检在进程内直接调用）

用法
====
  python3 mock_device.py --http-port 18080 --broker-port 18830 --node korvo2-0001
  curl "http://127.0.0.1:18080/mock/press?key=play&action=click"
  python3 panel.py --device korvo2-0001=http://127.0.0.1:18080 --mqtt 127.0.0.1:18830
"""

from __future__ import annotations

import argparse
import json
import math
import os
import socket
import struct
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

KEYS = ["volup", "voldown", "set", "play", "mode", "rec"]
ACTIONS = ("click", "click_release", "press", "press_release")


def _now_ms() -> int:
    return int(time.time() * 1000)


def make_wav(seconds: float = 2.0, rate: int = 16000, freq: float = 1000.0) -> bytes:
    """生成 16 kHz / 16 bit / 单声道正弦 WAV（模拟 AEC 采集产物，用于面板播放/拖动验证）。"""
    n = int(rate * seconds)
    frames = bytearray()
    for i in range(n):
        v = int(0.35 * 32767 * math.sin(2 * math.pi * freq * i / rate))
        frames += struct.pack("<h", v)
    data = bytes(frames)
    hdr = b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt " + \
        struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16) + \
        b"data" + struct.pack("<I", len(data))
    return hdr + data


# ----------------------------------------------------------------------------
# 极简 MQTT broker（仅面板/mock 自检用；不是产品组件）
# ----------------------------------------------------------------------------

class MiniBroker:
    def __init__(self, port: int, log=print, on_publish=None):
        self.port = port
        self.log = log
        self.on_publish = on_publish
        self._subs: list[tuple[socket.socket, str]] = []
        self._lock = threading.Lock()
        self._srv: socket.socket | None = None
        self._stop = threading.Event()

    # ------------------------------------------------------------ 工具
    @staticmethod
    def _read_len(sock: socket.socket, first: int) -> int:
        mult, value = 1, 0
        digit = first
        while True:
            value += (digit & 127) * mult
            mult *= 128
            if not (digit & 0x80):
                return value
            digit = sock.recv(1)[0]

    @staticmethod
    def _match(filt: str, topic: str) -> bool:
        f, t = filt.split("/"), topic.split("/")
        i = 0
        while i < len(f):
            if f[i] == "#":
                return True
            if i >= len(t):
                return False
            if f[i] != "+" and f[i] != t[i]:
                return False
            i += 1
        return len(f) == len(t)

    @staticmethod
    def _packet(ptype: int, payload: bytes, flags: int = 0) -> bytes:
        out = bytearray([(ptype << 4) | flags])
        n = len(payload)
        while True:
            d = n % 128
            n //= 128
            if n > 0:
                d |= 0x80
            out.append(d)
            if n == 0:
                break
        return bytes(out) + payload

    # ------------------------------------------------------------ 生命周期
    def start(self):
        self._srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._srv.bind(("127.0.0.1", self.port))
        self._srv.listen(8)
        self._srv.settimeout(0.5)
        threading.Thread(target=self._accept_loop, name="mini-broker", daemon=True).start()
        self.log(f"[mock] MQTT broker 监听 127.0.0.1:{self.port}")

    def stop(self):
        self._stop.set()
        try:
            if self._srv:
                self._srv.close()
        except OSError:
            pass

    def _accept_loop(self):
        while not self._stop.is_set():
            try:
                conn, _ = self._srv.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            threading.Thread(target=self._client, args=(conn,), daemon=True).start()

    def _client(self, conn: socket.socket):
        conn.settimeout(1.0)
        buf = b""
        try:
            while not self._stop.is_set():
                try:
                    chunk = conn.recv(4096)
                except socket.timeout:
                    continue
                if not chunk:
                    break
                buf += chunk
                while len(buf) >= 2:
                    ptype = buf[0] >> 4
                    mult, value, idx = 1, 0, 1
                    while idx < len(buf):
                        d = buf[idx]
                        value += (d & 127) * mult
                        mult *= 128
                        idx += 1
                        if not (d & 0x80):
                            break
                    if len(buf) < idx + value:
                        break
                    body = buf[idx:idx + value]
                    buf = buf[idx + value:]
                    self._handle(conn, ptype, body)
        except OSError:
            pass
        finally:
            with self._lock:
                self._subs = [(s, f) for (s, f) in self._subs if s is not conn]
            try:
                conn.close()
            except OSError:
                pass

    def _handle(self, conn: socket.socket, ptype: int, body: bytes):
        if ptype == 1:                                  # CONNECT
            conn.sendall(self._packet(2, bytes([0, 0])))
        elif ptype == 8:                                # SUBSCRIBE
            pid = struct.unpack("!H", body[:2])[0]
            tidx = 2
            count = 0
            while tidx < len(body):
                tlen = struct.unpack("!H", body[tidx:tidx + 2])[0]
                filt = body[tidx + 2:tidx + 2 + tlen].decode("utf-8", "replace")
                tidx += 2 + tlen + 1                    # +qos
                with self._lock:
                    self._subs.append((conn, filt))
                count += 1
            conn.sendall(self._packet(9, struct.pack("!H", pid) + bytes([0] * max(count, 1))))
        elif ptype == 3:                                # PUBLISH(qos0)
            tlen = struct.unpack("!H", body[:2])[0]
            topic = body[2:2 + tlen].decode("utf-8", "replace")
            payload = body[2 + tlen:]
            self.log(f"[mock] broker 收到 PUBLISH {topic} ({len(payload)} B)")
            self._dispatch(topic, payload)
            if self.on_publish:
                try:
                    self.on_publish(topic, payload)
                except Exception as exc:  # noqa: BLE001
                    self.log(f"[mock] on_publish 异常：{exc}")
        elif ptype == 12:                               # PINGREQ
            conn.sendall(self._packet(13, b""))

    def _dispatch(self, topic: str, payload: bytes):
        with self._lock:
            targets = [(s, f) for (s, f) in self._subs if self._match(f, topic)]
        for sock, _f in targets:
            try:
                sock.sendall(self._packet(3, struct.pack("!H", len(topic.encode())) +
                                          topic.encode() + payload))
            except OSError:
                pass

    def publish(self, topic: str, payload: bytes):
        self._dispatch(topic, payload)


# ----------------------------------------------------------------------------
# 模拟设备（HTTP API 与固件同形）
# ----------------------------------------------------------------------------

class MockDevice:
    def __init__(self, http_port: int = 18080, broker_port: int = 18830,
                 node: str = "korvo2-mock", fw: str = "0.1.0-mock", log=print):
        self.http_port, self.broker_port = http_port, broker_port
        self.node, self.fw = node, fw
        self.log = log
        self.history: list[dict] = []
        self.counters = {"total": 0, "unknown": 0, "click": 0, "click_release": 0,
                         "press": 0, "press_release": 0}
        self.current: dict | None = None
        self._seq = 0
        self._id = 0
        self._lock = threading.Lock()
        self.checks = self._default_checks()
        self.httpd: ThreadingHTTPServer | None = None
        self.broker = MiniBroker(broker_port, log=log)
        self._mqtt_sock: socket.socket | None = None
        self._stop = threading.Event()
        # 媒体（模拟 AEC 采集产物 + SD 卡媒体）：alias/相对路径 → 字节内容
        self._base_wav = make_wav(2.0)
        self.files: dict[str, bytes] = {}
        self._file_seq = 0
        self._aec_recording = False
        self._rec_timer: threading.Timer | None = None
        self._make_recording()      # 预置一条录音（SPIFFS 兜底路径，网页可播、不上板）
        self.files["sdcard/music/demo.wav"] = make_wav(3.0, freq=440.0)   # SD 卡媒体（可板上回放）
        # 板上回放状态（mock：不真的出声，只维护状态机）
        self.player = {"playing": False, "path": "", "codec": "-", "rate_hz": 0,
                       "channels": 0, "elapsed_ms": 0, "volume": 80, "msg": "就绪"}

    # ------------------------------------------------------------ 媒体（模拟 AEC 产物）
    def _make_recording(self, seconds: float = 2.0) -> str:
        with self._lock:
            self._file_seq += 1
            rel = f"spiffs/rec/aec-{self._file_seq:05d}.wav"
            self.files[rel] = make_wav(seconds)
            return "/spiffs/rec/aec-%05d.wav" % self._file_seq

    def media_list(self) -> dict:
        with self._lock:
            files = []
            for rel, data in sorted(self.files.items()):
                alias, name = rel.split("/", 1)
                dot = os.path.splitext(name)[1].lower()
                files.append({
                    "alias": alias,
                    "name": name,
                    "size": len(data),
                    "mtime": int(time.time()),
                    "kind": "audio",
                    "item": {"url": f"media/{rel}"},
                    "device_path": f"/{alias}/{name}",
                    "playable_on_board": alias == "sdcard" and dot in (".wav", ".mp3"),
                })
            return {
                "count": len(files),
                "rec_root": "/spiffs/rec",
                "aec_enabled": True,
                "aec_recording": self._aec_recording,
                "files": files,
            }

    def play(self, path: str) -> dict:
        """模拟板上回放：仅接受 /sdcard/ 下的 wav/mp3（与固件口径一致）"""
        dot = os.path.splitext(path or "")[1].lower()
        if not path or not path.startswith("/sdcard/"):
            return {"ok": False, "msg": "invalid path (must be /sdcard/...)", "playing": False}
        if dot not in (".wav", ".mp3"):
            return {"ok": False, "msg": "unsupported codec (wav/mp3 only)", "playing": False}
        rel = path.lstrip("/")
        if rel not in self.files:
            return {"ok": False, "msg": "file not found", "playing": False}
        with self._lock:
            self.player.update({"playing": True, "path": path, "codec": dot.lstrip("."),
                                "rate_hz": 16000, "channels": 1, "elapsed_ms": 0, "msg": "播放中"})
        self.log(f"[mock] 板上播放：{path}")
        return {"ok": True, "msg": "playing", "file": path, "playing": True,
                "volume": self.player["volume"]}

    def player_stop(self) -> dict:
        with self._lock:
            self.player.update({"playing": False, "msg": "已停止"})
        return {"ok": True, "msg": "stopped", "playing": False, "volume": self.player["volume"]}

    def set_volume(self, volume: int) -> dict:
        with self._lock:
            self.player["volume"] = max(0, min(100, int(volume)))
        return {"ok": True, "msg": "volume set", "playing": self.player["playing"],
                "volume": self.player["volume"]}

    def aec_start(self, seconds: int = 5) -> dict:
        path = self._make_recording(max(1, min(seconds, 30)))
        with self._lock:
            self._aec_recording = True
        if self._rec_timer:
            self._rec_timer.cancel()
        self._rec_timer = threading.Timer(max(1, seconds), self._aec_finish)
        self._rec_timer.daemon = True
        self._rec_timer.start()
        self.log(f"[mock] AEC 采集开始（{seconds}s）→ {path}")
        return {"ok": True, "msg": "ok", "file": path, "recording": True,
                "last_bytes": len(self.files[sorted(self.files)[-1]])}

    def _aec_finish(self):
        with self._lock:
            self._aec_recording = False
        self.log("[mock] AEC 采集结束")

    def aec_stop(self) -> dict:
        if self._rec_timer:
            self._rec_timer.cancel()
        with self._lock:
            self._aec_recording = False
        return {"ok": True, "msg": "stopped", "file": "", "recording": False,
                "last_bytes": len(self.files[sorted(self.files)[-1]])}

    def media_bytes(self, uri: str):
        """uri 形如 /media/spiffs/rec/aec-00001.wav → 字节内容（None = 不存在/非法）"""
        prefix = "/media/"
        if not uri.startswith(prefix):
            return None
        rel = uri[len(prefix):]
        if ".." in rel:
            return None
        with self._lock:
            return self.files.get(rel)

    @staticmethod
    def _default_checks() -> list[dict]:
        # 与固件 /api/selftest 同形（真机为实际查询结果；mock 取板级期望值）
        rows = [
            ("i2c.sda (rst J18)", "17", "17"), ("i2c.scl (rst J18)", "18", "18"),
            ("i2s0.mclk (rst J15/J16)", "16", "16"), ("i2s0.bck  (rst SCLK)", "9", "9"),
            ("i2s0.ws   (rst LRCK)", "45", "45"), ("i2s0.dout (rst DSDIN)", "8", "8"),
            ("i2s0.din  (rst SDOUT)", "10", "10"), ("pa_enable_gpio (IO48)", "48", "48"),
            ("headphone_detect", "-1", "-1"), ("sdcard_intr_gpio", "-1", "-1"),
            ("sdcard_open_file_num_max", "5", "5"), ("green_led_gpio (无)", "-1", "-1"),
            ("blue_led_gpio (TCA9554 P7)", "128", "128"), ("es8311_mclk_src (ESP MCLK)", "0", "0"),
            ("adc_input_ch_format", "RMNM", "RMNM"),
        ]
        return [{"item": i, "expect": e, "actual": a, "pass": (e == a)} for (i, e, a) in rows]

    # ------------------------------------------------------------ 生命周期
    def start(self):
        self.broker.start()
        time.sleep(0.2)
        self._mqtt_connect()
        self.httpd = ThreadingHTTPServer(("0.0.0.0", self.http_port), self._handler())
        threading.Thread(target=self.httpd.serve_forever, name="mock-http", daemon=True).start()
        self.log(f"[mock] 设备 HTTP 监听 0.0.0.0:{self.http_port}（node={self.node}）")

    def stop(self):
        self._stop.set()
        try:
            if self.httpd:
                self.httpd.shutdown()
        except Exception:  # noqa: BLE001
            pass
        try:
            if self._mqtt_sock:
                self._mqtt_sock.close()
        except OSError:
            pass
        self.broker.stop()

    # ------------------------------------------------------------ MQTT（自扮设备 + 收 ack）
    def _mqtt_connect(self):
        try:
            s = socket.create_connection(("127.0.0.1", self.broker_port), timeout=3)
        except OSError as exc:
            self.log(f"[mock] 连接 broker 失败：{exc}")
            return
        cid = f"mock-{self.node}"
        var = struct.pack("!H", 4) + b"MQTT" + bytes([4, 0x02]) + struct.pack("!H", 30)
        payload = struct.pack("!H", len(cid)) + cid.encode()
        s.sendall(MiniBroker._packet(1, var + payload))
        s.recv(64)                                        # CONNACK
        ack_topic = f"rmng/dev/{self.node}/event/down"
        body = struct.pack("!H", 1) + struct.pack("!H", len(ack_topic)) + ack_topic.encode() + b"\x00"
        s.sendall(MiniBroker._packet(8, body))
        s.settimeout(1.0)
        self._mqtt_sock = s
        threading.Thread(target=self._mqtt_loop, name="mock-mqtt", daemon=True).start()
        self.log(f"[mock] 已订阅 {ack_topic}（用于接收云端 ack）")

    def _mqtt_loop(self):
        buf = b""
        while not self._stop.is_set():
            try:
                chunk = self._mqtt_sock.recv(4096)
            except socket.timeout:
                continue
            except OSError:
                break
            if not chunk:
                break
            buf += chunk
            while len(buf) >= 2:
                ptype = buf[0] >> 4
                mult, value, idx = 1, 0, 1
                while idx < len(buf):
                    d = buf[idx]
                    value += (d & 127) * mult
                    mult *= 128
                    idx += 1
                    if not (d & 0x80):
                        break
                if len(buf) < idx + value:
                    break
                body = buf[idx:idx + value]
                buf = buf[idx + value:]
                if ptype == 3:                            # 云端 ack（event/down）
                    tlen = struct.unpack("!H", body[:2])[0]
                    payload = body[2 + tlen:]
                    try:
                        doc = json.loads(payload.decode("utf-8", "replace"))
                        ref = (doc.get("data") or {}).get("ref") or doc.get("id")
                    except ValueError:
                        ref = None
                    if ref:
                        self.mark_acked(ref)

    def _mqtt_publish(self, topic: str, payload: bytes):
        if not self._mqtt_sock:
            return
        try:
            self._mqtt_sock.sendall(MiniBroker._packet(
                3, struct.pack("!H", len(topic.encode())) + topic.encode() + payload))
        except OSError as exc:
            self.log(f"[mock] 上报失败：{exc}")

    # ------------------------------------------------------------ 业务
    def press(self, key: str = "play", action: str = "click") -> dict:
        """模拟一次按键：本地登记 → 发布 event/up（broker 会回 ack）。"""
        with self._lock:
            self._seq += 1
            self._id += 1
            eid = f"key-{self._id:05d}"
            entry = {"seq": self._seq, "id": eid, "key": key, "action": action,
                     "ts_ms": _now_ms(), "uplink": "sent", "ack_ms": 0}
            self.history.insert(0, entry)
            del self.history[200:]
            self.current = dict(entry)
            self.counters["total"] += 1
            self.counters[action if action in self.counters else "unknown"] += 1
        frame = {
            "v": 1, "id": eid, "seq": self._seq, "ts": entry["ts_ms"], "type": "event",
            "data": {"count": 1, "items": [{"id": eid, "type": "device_event",
                                            "severity": "info", "ts": entry["ts_ms"],
                                            "title": "key",
                                            "data": {"key": key, "action": action}}]},
        }
        self._mqtt_publish(f"rmng/dev/{self.node}/event/up",
                           json.dumps(frame, ensure_ascii=False).encode())
        self.log(f"[mock] 按键 {key}/{action} → event/up id={eid}")
        return entry

    def mark_acked(self, eid: str):
        with self._lock:
            for e in self.history:
                if e["id"] == eid and e["uplink"] != "acked":
                    e["uplink"] = "acked"
                    e["ack_ms"] = _now_ms()
                    if self.current and self.current["id"] == eid:
                        self.current["uplink"] = "acked"
                        self.current["ack_ms"] = e["ack_ms"]
                    self.log(f"[mock] 收到云端 ack：{eid}")
                    break

    # ------------------------------------------------------------ HTTP
    def _status(self) -> dict:
        with self._lock:
            last = sorted(self.files)[-1] if self.files else ""
            return {
                "fw": self.fw, "board": "ESP32-S3-Korvo-2 v3 (mock)",
                "uptime_ms": _now_ms() % 10_000_000,
                "wifi": {"connected": True, "ip": "127.0.0.1"},
                "cloud": {"link_up": True, "transport": "mqtt-tcp",
                          "tx_frames": self.counters["total"], "rx_frames": self.counters["total"]},
                "selftest": {"total": len(self.checks), "failed": 0},
                "keys": {"count": len(KEYS), "events": self.counters["total"],
                         "history_max": 200},
                "aec": {"enabled": True, "recording": self._aec_recording,
                        "last_file": ("/" + last) if last else "",
                        "last_bytes": len(self.files[last]) if last else 0,
                        "total_files": self._file_seq,
                        "root": "/spiffs/rec"},
                "media": {"sd_mounted": True, "rec_root": "/spiffs/rec", "poll_hint_ms": 1000},
                "player": dict(self.player),
                "panel": {"api_version": "1", "scope": "local-verification-only"},
            }

    def _keys(self, limit: int) -> dict:
        with self._lock:
            return {
                "keys": [{"id": i, "label": k} for i, k in enumerate(KEYS)],
                "current": dict(self.current) if self.current else None,
                "counters": dict(self.counters),
                "history_total": self.counters["total"],
                "history": [dict(e) for e in self.history[:limit]],
            }

    def _handler(self):
        mock = self

        class Handler(BaseHTTPRequestHandler):
            server_version = "mock-korvo2/1.0"

            def log_message(self, fmt, *args):
                if self.path.startswith("/mock/"):
                    mock.log("[mock] " + fmt % args)

            def _json(self, doc, code=200):
                body = json.dumps(doc, ensure_ascii=False).encode()
                self.send_response(code)
                self.send_header("Content-Type", "application/json; charset=utf-8")
                self.send_header("Access-Control-Allow-Origin", "*")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def do_GET(self):  # noqa: N802
                parsed = urllib.parse.urlparse(self.path)
                q = urllib.parse.parse_qs(parsed.query)
                if parsed.path == "/api/ping":
                    self._json({"ok": True, "api_version": "1", "role": "mock"})
                elif parsed.path == "/api/status":
                    self._json(mock._status())
                elif parsed.path == "/api/selftest":
                    self._json({"total": len(mock.checks), "failed": 0,
                                "note": "运行期核对项（mock 数据）", "items": mock.checks})
                elif parsed.path == "/api/keys":
                    self._json(mock._keys(int(q.get("limit", ["50"])[0])))
                elif parsed.path == "/media/list":
                    self._json(mock.media_list())
                elif parsed.path.startswith("/media/"):
                    self._send_file(parsed.path)
                elif parsed.path == "/mock/press":
                    key = q.get("key", ["play"])[0]
                    action = q.get("action", ["click"])[0]
                    self._json(mock.press(key, action))
                else:
                    self._json({"error": "not_found"}, 404)

            def _send_file(self, uri: str):
                blob = mock.media_bytes(uri)
                if blob is None:
                    self._json({"error": "file_not_found"}, 404)
                    return
                total = len(blob)
                start, end = 0, total - 1
                rng = self.headers.get("Range")
                if rng and rng.startswith("bytes="):
                    spec = rng[6:]
                    a, _, b = spec.partition("-")
                    try:
                        if a == "":                      # 末尾 N 字节
                            start = max(0, total - int(b))
                            end = total - 1
                        else:
                            start = int(a)
                            end = int(b) if b else total - 1
                            end = min(end, total - 1)
                    except ValueError:
                        start, end = 0, total - 1
                    if start >= total or start > end:
                        self.send_response(416)
                        self.send_header("Content-Range", f"bytes */{total}")
                        self.end_headers()
                        return
                body = blob[start:end + 1]
                self.send_response(206 if (rng and (start > 0 or end < total - 1)) else 200)
                self.send_header("Content-Type", "audio/wav")
                self.send_header("Accept-Ranges", "bytes")
                self.send_header("Access-Control-Allow-Origin", "*")
                self.send_header("Content-Length", str(len(body)))
                if rng and (start > 0 or end < total - 1):
                    self.send_header("Content-Range", f"bytes {start}-{end}/{total}")
                self.end_headers()
                self.wfile.write(body)

            def do_POST(self):  # noqa: N802
                parsed = urllib.parse.urlparse(self.path)
                if parsed.path != "/api/action":
                    self._json({"error": "not_found"}, 404)
                    return
                length = int(self.headers.get("Content-Length") or 0)
                raw = self.rfile.read(length) if length else b"{}"
                try:
                    doc = json.loads(raw.decode("utf-8", "replace") or "{}")
                except ValueError:
                    self._json({"error": "invalid_json"}, 400)
                    return
                op = doc.get("op")
                if op == "aec_start":
                    self._json(mock.aec_start(int(doc.get("duration_s") or 5)))
                elif op == "aec_stop":
                    self._json(mock.aec_stop())
                elif op == "play":
                    self._json(mock.play(doc.get("path") or ""))
                elif op in ("stop", "play_stop"):
                    self._json(mock.player_stop())
                elif op == "set_volume":
                    self._json(mock.set_volume(int(doc.get("volume") or 0)))
                else:
                    self._json({"ok": False, "msg": "unknown op"}, 200)

        return Handler


def main() -> int:
    ap = argparse.ArgumentParser(description="模拟 Korvo-2 设备（验证面板用）")
    ap.add_argument("--http-port", type=int, default=18080)
    ap.add_argument("--broker-port", type=int, default=18830)
    ap.add_argument("--node", default="korvo2-mock")
    ap.add_argument("--auto-press-ms", type=int, default=0,
                    help=">0 时按该周期自动模拟按键（演示用）")
    args = ap.parse_args()

    dev = MockDevice(args.http_port, args.broker_port, args.node)
    dev.start()
    if args.auto_press_ms > 0:
        def auto():
            i = 0
            while True:
                time.sleep(args.auto_press_ms / 1000.0)
                dev.press(KEYS[i % len(KEYS)], ACTIONS[i % len(ACTIONS)])
                i += 1
        threading.Thread(target=auto, daemon=True).start()
    print(f"[mock] 就绪：http://127.0.0.1:{args.http_port}/api/status ; "
          f"按键：curl 'http://127.0.0.1:{args.http_port}/mock/press?key=play&action=click'")
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        dev.stop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
