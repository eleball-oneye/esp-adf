# oneye 例程验证面板（宿主侧小型服务）

> **一句话**：把例程跑出来的功能成果在浏览器里**看得见、可回放、可留证** —— 按键实时状态与历史响应、
> 板级参数核对逐行 PASS/FAIL、链路与设备概况；（随后接入）AEC 采集音频与 SD 卡录音在 web 播放。

## 0. 定位与边界（先读）

| 项 | 口径 |
| --- | --- |
| 服务位置 | **宿主侧**（PC / WSL），Python 3 **标准库零第三方依赖**；浏览器打开 `http://127.0.0.1:8787/` |
| 设备侧配合 | 固件内**本地验证 API**（`examples/oneye/korvo2_oneye/main/panel_api.c`）：`/api/status`、`/api/selftest`、`/api/keys` |
| **不是契约** | 设备侧 `/api/*` 是**台面/联调验证面**，**不是云端设备面契约**：不进 `backend/contracts`、不新增 topic、不写影子新键、不参与 `caps/up` 声明；**量产固件应置 `CONFIG_ONEYE_FW_ENABLE_PANEL_API=n`** |
| 只读性 | 面板只读设备数据；**不改变设备行为**（触发录音/回放等写操作属下一轮的 `POST /api/action`，须先冻结口径） |

## 1. 快速开始

```bash
# ① 真机（设备已联网；IP 见串口日志 / 路由器）
python3 panel.py --device korvo2-0001=http://192.168.1.50
#   → 浏览器打开 http://127.0.0.1:8787/

# ② 多板聚合 + MQTT（dev-stack EMQX；云端视角与设备视角对照）
python3 panel.py --device a=http://192.168.1.50 --device b=http://192.168.1.51 \
                 --mqtt 127.0.0.1:1883

# ③ 无硬件：内置 mock 设备 + mock broker 自检（面板自身链路）
python3 panel.py --self-test

# ④ 串口兜底（无网也能核对；POSIX 用 termios，Windows 需 pip install pyserial）
python3 panel.py --device korvo2-0001=http://192.168.1.50 --serial /dev/ttyUSB0@115200
```

设备清单也可落 `devices.json`（见 `devices.example.json`）；`--device name#node=URL` 可显式指定 MQTT 关联用的 `node_id`。

## 2. 三条取数通道（自动降级，互为兜底）

| 通道 | 取什么 | 不可用时 |
| --- | --- | --- |
| **设备 HTTP**（主） | `/api/status`（固件/板卡/uptime/Wi-Fi/云端链路与收发）、`/api/selftest`（逐行核对）、`/api/keys`（实时 + 历史） | 设备显示离线并给出错误；面板其余部分照常 |
| **MQTT**（云端视角） | 订阅 `rmng/dev/+/event/up`、`rmng/dev/+/status/up`；按事件 `id` 与设备本地历史**关联** ⇒ 判断"云端是否真的收到"并可算端到端时延 | 面板标 `MQTT 未连接`；按键仍走设备 HTTP 显示 |
| **串口**（兜底） | 解析 `[key]`、`[board-check]`、`[sdk-event]` 行 | POSIX 直接可用；Windows 缺 pyserial 时自动禁用并提示 |

> 极简 MQTT 客户端（`panel.py` 内 `MiniMqtt`）只实现面板所需子集（CONNECT/SUBSCRIBE/PUBLISH/PINGREQ），
> 与 SDK 侧"协议自持"的口径一致：**验证工具不引入 pip 依赖**。

## 3. 页面能力（本轮）

1. **按键：实时状态** —— 6 键（REC/MUTE/PLAY/SET/VOL−/VOL+）高亮最近触发键、显示动作（短按/长按/释放）、
   计数（短按/长按/释放）与"云端事件条数"；
2. **按键：历史响应** —— 每行一条事件：`#seq / 本地时间 / 按键 / 动作 / 上行状态 / 云端确认 / 端到端时延`；
   **上行状态三态**来自设备侧：`pending`（本地已检测）→ `sent`（已交 SDK 上报，失败为 `failed`）→ `acked`（收到云端 `event/down` ack，
   按 `data.ref == 事件 id` 关联）；"云端确认"列来自面板自身 MQTT 订阅（**独立于设备自报**，两者对照即端到端证据）；
3. **板级参数核对** —— 设备运行期自检逐行 `项 / 期望 / 实测 / PASS-FAIL`（与串口 `[board-check]` 同源）；
   编译期断言（`board_expect.h`）失败时**固件根本不构建**，因此这里只列运行期项；
4. **音频：AEC 采集与 SD 卡媒体** —— 「录 5/10/30 s」「停止录音」按钮触发设备侧 AEC 采集（AFE：AEC + 降噪 → WAV 落 SD/SPIFFS）；
   文件列表显示名称/类型/大小/时间，并区分**两个播放方向**：
   - **网页播放**（任何文件）：`<audio controls>` 直接向设备拉流（设备实现 `Range` ⇒ 可拖动进度）+「下载」留证；
   - **板上播放**（仅 SD 卡上的 wav/mp3）：多一个「板上播放」按钮 ⇒ 设备扬声器出声，另有「停止播放」与音量滑块，
     状态行显示正在播放的文件、编码、采样率与已播时长（数据来自 `/api/status.player{}`）；
   图片/视频文件提供**网页预览**（板上视频解码本轮不做）；
5. **设备与链路** —— 固件版本、板卡、运行时长、Wi-Fi IP、云端链路与收发帧、最近错误；
6. **Wi-Fi 配网** —— 显示连接状态 / SSID / **凭据来源**（`file:/sdcard/oneye-wifi.txt`（SD 凭据文件）、
   `kconfig`、`api`），并提供运行期改配表单（SSID + 密码 → 设备重连，最长约 20 s 出结果）。
   持久化以 SD 卡凭据文件为准（见 [固件 README `../../examples/oneye/korvo2_oneye/README.md`](../../examples/oneye/korvo2_oneye/README.md)）；
   表单只改本次运行、**不写文件**（明文，仅台面/联调）；
7. **串口兜底** —— 最近日志尾部（无网时仍能核对按键与自检输出）。

## 4. 设备侧 API（本地验证面）

| 路由 | 返回（要点） |
| --- | --- |
| `GET /api/ping` | `{"ok":true,"api_version":"1","role":"local-verification-only"}` |
| `GET /api/status` | `fw`、`board`、`uptime_ms`、`wifi{connected,ip,ssid,source}`、`cloud{link_up,transport,tx_frames,rx_frames}`、`selftest{total,failed}`、`keys{count,events,history_max}`、**`aec{enabled,recording,last_file,last_bytes,total_files,root}`**、**`media{sd_mounted,rec_root}`** |
| `GET /api/selftest` | `{total,failed,note,items:[{item,expect,actual,pass}]}`（运行期核对明细） |
| `GET /api/keys?limit=N` | `keys[]`（6 键标签）、`current{seq,id,key,action,ts_ms,uplink,ack_ms}`、`counters{}`、`history[]`（**新→旧**，含 `uplink`/`ack_ms`） |
| `GET /media/list` | `{count,rec_root,aec_enabled,aec_recording,files:[{alias,name,size,mtime,kind,item{url}}]}`（SD 与 SPIFFS；按扩展名判 `audio`/`image`/`video`） |
| `GET /media/<alias>/<path>` | 文件下载/流式播放，**支持 `Range`**（`206` + `Content-Range` + `Accept-Ranges`）⇒ 浏览器 `<audio>` 可拖动；`alias`=`sdcard`\|`spiffs`；含目录穿越防护 |
| `POST /api/action` | `{"op":"aec_start","duration_s":N}` / `{"op":"aec_stop"}` / `{"op":"play","path":"/sdcard/..."}` / `{"op":"stop"}` / `{"op":"set_volume","volume":0-100}` / `{"op":"wifi_set","ssid":"...","password":"..."}` ⇒ `{ok,msg,file,recording,playing,volume,last_bytes}`（**写操作，仅台面验证**；`play` 仅接受 SD 卡上的 wav/mp3；`wifi_set` 仅当 `CONFIG_ONEYE_FW_ENABLE_WIFI_FILE=y`） |

事件 id 由固件生成（`key-00001`…）并作为 `event/up` 的幂等 `id`；云端 ack 的 `data.ref` 与之对齐 ⇒ **设备侧**能自报三态，
**面板侧**再用 MQTT 独立验证一次。

## 5. 面板自身 API（供脚本/CI 复用）

| 路由 | 说明 |
| --- | --- |
| `GET /` | 单页 UI（内嵌，无 CDN；轮询 `/api/state`，缺省 600 ms，可用 `--poll-ms`） |
| `GET /api/state` | 聚合快照：`mqtt{}`、`serial{}`、`devices[]`（`status`/`selftest`/`keys`/`history`（含云端关联字段）/`serial_tail`） |
| `GET /api/health` | `{"ok":true,"version":"0.1.0"}` |

## 6. 文件

| 文件 | 作用 |
| --- | --- |
| `panel.py` | 面板服务（HTTP + 设备轮询 + 极简 MQTT 客户端 + 串口兜底 + 内嵌页面 + `--self-test`） |
| `mock_device.py` | 模拟 Korvo-2 设备（与固件同形的 `/api/*`）+ 极简 MQTT broker + `GET /mock/press?key=&action=` 触发按键 + `GET /mock/wifi-cred?ssid=` 模拟凭据文件配网；并自扮云端回 `event/down` ack |
| `devices.example.json` | 设备清单示例 |
| `README.md` | 本文件 |

## 7. 本轮实测记录（2026-09-15）

| 项 | 命令 | 结果 |
| --- | --- | --- |
| 面板自检（无硬件） | `python3 panel.py --self-test`（内置 mock 设备 + mock broker + 模拟 AEC 录音 WAV + SD 卡媒体 + 凭据文件配网） | **33 项全部通过，rc=0**：设备 HTTP、自检明细 15 行、固件标识、MQTT 连接、按键历史（新→旧、**云端确认 3/3**、端到端时延、动作覆盖）、`/api/health`、首页渲染、**首页含音频/AEC 卡片**、**媒体列表非空**、**绝对播放地址**、**Range 206 + Content-Range + RIFF 头**、**触发 AEC 采集**、**采集后列表增长**、**停止采集**、**SD 卡媒体可板上回放标记**、**触发板上播放**、**播放状态回显**、**音量设置**、**停止播放**、**非 SD 路径被拒绝**、**Wi-Fi 状态含 ssid/source**、**凭据来源标注为 SD 文件**、**运行期改配 wifi_set**、**改配后状态回显新 SSID + api 来源**、**空 SSID 被拒绝**、**改配失败可观测**、**凭据文件配网回落**、**首页含 Wi-Fi 配网卡片**、设备离线降级 |
| 设备侧 API + AEC 采集 + 板上回放 + 配网编译 | `./build-all.sh --toolchains esp32s3@5.5.5 --firmware`（IDF v5.5.5 / esp32s3 / KORVO2_V3；自定义分区表 + esp-sr AFE + Wi-Fi 配网） | 见本页 §7.1 构建记录 |
| 真机联调 | —— | **未做**（本轮只出构建产物；烧录后：SD 卡放 `oneye-wifi.txt` → 复位 → 面板 `--device name=http://<IP>` → 按键三态 + 录 5 s 网页播放 + 插 SD 卡后「板上播放」+ Wi-Fi 卡片核对凭据来源） |

### 7.1 构建记录（AEC 采集 + 媒体 API）

| 项 | 结果 |
| --- | --- |
| 命令 | `idf.py build`（IDF v5.5.5 / esp32s3 / `CONFIG_ESP32_S3_KORVO2_V3_BOARD=y`） |
| 分区 | 自定义 `partitions.csv`：`factory 2M` + **`model 2M`** + `storage(spiffs) 1M` |
| 产物 | `korvo2_oneye.bin` **390,656 B**（分区余量 81%）；**`srmodels/srmodels.bin` 337,952 B**（esp-sr 模型，烧写偏移 `0x210000`，`flash_args` 已包含） |
| 坑位（已修） | esp-sr 只在分区表存在 `model` 分区时才投放模型（上游 AEC/algorithm 例程缺该分区 ⇒ 模型从未投放）；另有 4 类编译期错误（注释内嵌 `/*`、`HTTPD_416_*` 缺失、`I2S_CHANNEL_FMT_*` 需 `driver/i2s.h`、`snprintf` 截断告警） |

复跑：`python3 panel.py --self-test`（日志落 `output/.build/panel-selftest.log`）。

## 8. 后续（同一面板继续接）

1. ~~**AEC 采集音频在 web 播放**~~ **已完成**（2026-09-15）：设备侧 AEC 采集（esp-sr AFE → WAV 落 SD/SPIFFS）
   + `GET /media/list` + `GET /media/<alias>/<path>`（Range）× 面板「音频」卡片（录音/播放/下载）；
   真机录音与出声效果待烧录后验证；
2. **SD 卡录音/录像选择与回放**（下一步）：`GET /media/list` 已能列出 `/sdcard` 媒体（WAV/MP3/JPG/AVI/MJPEG），
   面板已支持音频播放与图片/视频预览；待补：`POST /api/action {op:"play",path}` 触发**板上**播放并回显播放状态；
3. **回归留证**：面板把每次会话的历史（按键三态 + 自检表 + 云端时延 + 录音文件清单）落 JSONL，供验收与回归对照；
4. **多板批量**：`devices.json` 已支持多台；后续加"一键全部刷新/批量按键注入/批量录音"以便产测台复用。
