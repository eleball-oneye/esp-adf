# korvo2_llm_chat —— 长按说话 · 云端 LLM 实时语音对话（ESP32-S3-Korvo-2）

> **状态**：`进行中（P3）` —— 代码与构建在推进中；**真机取证待补**（见 §7）。
> **上位契约**：
> - 语音面（云端）：[`backend/contracts/api/ws/语音对话.md`](../../../../backend/contracts/api/ws/语音对话.md) +
>   [`voice-frames.json`](../../../../backend/contracts/api/ws/voice-frames.json)（协议 `oneye.voice.v1`）
> - 本地面（配网/链路）：[`contracts/local/`](../../../../contracts/local/)（`link-frames.json`、`ble-gatt.md`、`lan-link.md`、`smartconfig.md`）
> **同板配方来源**：[`examples/oneye/korvo2_oneye`](../korvo2_oneye)（AFE 采集与播放已在真机取过证）

## 1. 例程做什么

一块 Korvo-2 开发板，按住 **REC 键 ≥600 ms** 说话，松开后：

```
REC 长按 ──► AFE(AEC+降噪) 采集 ──► 0x01 二进制上行(60 ms/帧) ──► chatd
                                                                   │
  喇叭 ◄── I2S/ES8311 播放 ◄── 0x02 二进制下行 ◄── TTS ◄── LLM ◄── ASR ◄── input.audio.commit
```

- **打断（barge-in）**：播放中再长按（或短按）→ 立刻发 `input.cancel` 并停止回放，服务端保证 ≤200 ms 停止下行音频；
- **网络**：Wi-Fi 由 **BLE 配网（主）** / **SmartConfig（备）** / **凭据文件（台面）** 三通道并行获得，先成功者胜出；
- **本地面**：设备同时作为 link **设备节点**在线（BLE 自研 GATT + 局域网发现/帧面），供移动端 App 发现与控制。

## 2. 目录

| 文件 | 职责 |
| --- | --- |
| `main/main.c` | 编排：初始化顺序、按键→话轮→上下行的状态机与兜底 |
| `main/voice_io.{h,c}` | 音频出入口（唯一碰 ADF 音频栈的地方）：AFE 采集 + PCM 回放 |
| `main/llm_client.{h,c}` | 云端语音面客户端（唯一碰 WebSocket 的地方）：`oneye.voice.v1` 帧与状态机 |
| `main/key_talk.{h,c}` | 按键语义：REC 长按阈值、短按、按下瞬间（打断） |
| `main/prov_service.{h,c}` | 配网（文件/BLE/SmartConfig）+ 本地面节点注册与 `prov.status` 上报 |
| `main/lan_link.{h,c}` | 本地面 `lan` 信道数据面：UDP 57321 发现 + `POST /api/link/frame` |
| `main/panel_min.{h,c}` | 串口状态呈现（**非契约**，不新增任何 HTTP 端点） |
| `main/Kconfig.projbuild` | 本工程全部可调项（服务地址、阈值、配网开关、合规开关） |
| `partitions.csv` | nvs / phy_init / factory(3M) / model(2M) / storage(1M) |
| `sd-root/oneye-wifi.txt` | 台面配网凭据文件样例（复制到 SD 卡根目录） |

## 3. 与契约的对应关系（评审用）

| 行为 | 契约条目 |
| --- | --- |
| `session.start{device_id,codec}` / `session.ready` | `voice-frames.json` §frames；会话每设备并发 1 |
| 上行 1920 B/帧、下行 1920 B/帧 | `codecs[0]`（pcm 16000/1/16/60 ms）+ `binary_frame_max_bytes` |
| `0x01` 上行 / `0x02` 下行 | `binary_kinds` |
| `input.audio.commit` / `input.cancel` | `frames`（含 `cancelled` 回写字段） |
| 单轮 30 s / 会话 10 min / 上行 64 KB/s | `limits`（固件侧 `ONEYE_LLM_TURN_MAX_MS` 兜底） |
| BLE 配网 GATT/分片/POP 配对 | `contracts/local/ble-gatt.md`（由 `oneye-dev-sdk` 的 `oneye_dev_ble` 实现） |
| 局域网发现与帧面 | `contracts/local/lan-link.md`（本工程 `lan_link.c`） |
| SmartConfig 设备侧 | `contracts/local/smartconfig.md`（**复用 ESP-IDF 内置实现**，端口 7001/ACK 11 B） |
| link 帧信封与节点/组域 | `contracts/local/link-frames.json` + `README.md`（由 `oneye_dev_link` 实现） |

**红线遵守**：不新增 MQTT topic / 影子键 / 能力位；音频与转写正文**不落盘**（`ONEYE_LLM_LOG_TEXT` 缺省 n）；BLE 加密链路与明文 `lan` 明确区分（`lan` 仅台面，Kconfig 可关）。

## 4. 构建

```bash
# 1) 准备 IDF 5.5.x 与 ESP-ADF v2.8（本工程在 IDF v5.5.5 + ADF v2.8 上开发）
export IDF_PATH=<esp-idf 检出>
export ADF_PATH=<本仓库>/embedded/esp-adf

# 2) 构建（首次会经组件管理器下载 espressif/esp_websocket_client，需要网络）
cd <本仓库>/embedded/esp-adf/examples/oneye/korvo2_llm_chat
idf.py set-target esp32s3
idf.py build

# 3) 烧写 + 监视
idf.py -p <PORT> flash monitor
```

> Windows 下同样可用 `idf.py`（需已安装 IDF 的 PowerShell 环境）；本工程文件为 UTF-8/LF，源文件不含中文以外编码问题。
> ⚠️ 首次构建需要访问组件注册表（`espressif/esp_websocket_client`）；离线环境可预先在联网机器上 `idf.py build` 后带回 `managed_components/`。

## 5. 联调（不需要开发板也能先验证服务端）

```bash
# 台面：在开发机上跑语音小服务 chatd（stub 形态无需任何模型密钥）
cd <本仓库>/backend && go run ./src/rmneo/voice/cmd/chatd        # 监听 :9091

# 参照客户端自测（另开一个终端；round1 正常轮 + round2 打断）
go run ./scripts/e2e/voice_client.go -addr 127.0.0.1:9091 -device korvo2-e2e
# 或 Windows:  powershell -File scripts/e2e-voice.ps1
```

设备侧把 Kconfig 的 `ONEYE_LLM_SERVER_HOST` 指向该机器局域网 IP（`ws://<ip>:9091/v1/voice/ws`），Wi-Fi 用凭据文件或 BLE 配网。

## 6. Kconfig 速查

| 项 | 缺省 | 说明 |
| --- | --- | --- |
| `ONEYE_LLM_DEVICE_ID` | `korvo2-llm-0001` | `session.start.device_id`（每设备并发 1） |
| `ONEYE_LLM_SERVER_HOST/_PORT/_WS_PATH` | `192.168.1.100` / `9091` / `/v1/voice/ws` | chatd 地址 |
| `ONEYE_LLM_USE_TLS` / `_TLS_INSECURE` | n / n | 生产必须 `wss` + 受信 CA |
| `ONEYE_LLM_TALK_MIN_PRESS_MS` | 600 | 长按阈值（同时写入板级 ADC 按键 `press_judge_time`） |
| `ONEYE_LLM_TURN_MAX_MS` | 30000 | 单轮上限（超时自动提交） |
| `ONEYE_LLM_PLAY_VOLUME` | 80 | 回放音量 |
| `ONEYE_LLM_ENABLE_WIFI_FILE` | y | 凭据文件配网（**明文，量产置 n**） |
| `ONEYE_LLM_ENABLE_BLE_PROV` | y | BLE 自研 GATT 配网（主通道） |
| `ONEYE_LLM_ENABLE_SMARTCONFIG` | y | ESPTouch v2（备用通道） |
| `ONEYE_LLM_ENABLE_LAN_LINK` | y | 本地面 `lan` 信道（**明文，量产置 n**） |
| `ONEYE_LLM_LOG_TEXT` | n | 是否把 ASR/LLM 文本打串口（PIPL：正文不落盘） |

## 7. 已验证 / 未验证（真机取证记录，2026-09-17）

**构建（已通过）**：ESP-IDF v5.5.5 + ESP-ADF v2.8，`idf.py set-target esp32s3 && idf.py build` 成功，
应用镜像 `0x1d2510`（≈1.82 MB），落在 `factory` 3 MB 分区内（余量 39%）；
`srmodels.bin` 随 `model` 分区投放（`0x310000`）。

**真机（ESP32-S3-Korvo-2，COM12，已烧写取证）**：

| 环节 | 结果 | 证据 |
| --- | --- | --- |
| 启动 / 分区 / PSRAM | ✅ | 分区表与 `partitions.csv` 一致；octal PSRAM 8 MB 探测通过 |
| 板级音频初始化 | ✅ | `ES8311 in Slave mode` / `ES7210 Enable MIC1..3` + `Enable TDM mode` |
| REC 键长按服务 | ✅ | `key_talk: REC 键就绪：长按 ≥ 600 ms 开始收音`（`press_judge_time` 覆写生效）；ADC 校准成功 |
| 凭据文件配网（本板 SD 卡 `/sdcard/oneye-wifi.txt`） | ✅ | `prov_service: 凭据文件命中 … ssid=wanya` → `已联网：ssid=wanya ip=192.168.110.83 source=file` |
| 局域网链路 | ✅ 启动 | `lan_link: UDP 发现已就绪（57321）` + `POST http://<ip>:80/api/link/frame` 已注册 |
| BLE 配网 | ❌ 不可用 | `oneye_dev_ble_init` 返回 `-5`（UNSUPPORTED）——**预编译库缺陷**，见下 |
| 语音面连接 | ⚠️ 已发起、未完成 | `llm_client: ws://…:9091/v1/voice/ws（子协议 oneye.voice.v1）` → `状态=CONNECTING` → `websocket_client: Started`，随后设备复位 |

**开机即复位的缺陷（未解决，已定位到根因方向）**：

- 现象：连上 Wi-Fi 后创建 WebSocket 客户端时复位重启（`Guru Meditation … LoadProhibited` 或 `Interrupt wdt timeout on CPU0`，两者都落在 heap 侧）；
- 根因（已用 `xtensa-esp32s3-elf-addr2line` 解码回溯源确认）：
  `prov_report()` → `oneye_dev_link_prov_report_status()` → `oneye_dev_link_send()` 的组帧路径
  **在栈上开 8 KB**（`oneye_dev_link.c:558 char frame[ONEYE_DEV_LINK_FRAME_MAX_BYTES]`）
  ＋ `oneye_link_frame_build()` 的 `payload[2049]`（`oneye_link_frame.c:152`）⇒ 单次调用栈峰值 ≈**11 KB**；
  最初从**系统事件任务**（3 KB 栈）调用，栈被踩穿并破坏堆，随后任意一次 malloc（如 `xTaskCreate`）即崩；
- 已做的缓解：所有 link 上报改为**独立 12 KB 任务**（`prov_service.c` 的 `link_report_task` + 队列投递）、
  httpd 栈 12 KB、`CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192`、`CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE=4096`；
  复位现象**仍在**（已排除 Wi-Fi/Wi-Fi-LWIP PSRAM 分配这一项），需下一轮继续：建议下一步先在
  `llm_client_start()` 前加内存水位打印、并复核 12 KB/16 KB 任务栈是否真的分配到内部 RAM；
  **同时应向 SDK（P1 交付）提出**：`oneye_dev_link_send()` 不应把 8 KB 缓冲放在栈上（应改堆分配或降低上限），
  否则任何调用方都可能踩栈。

**BLE 预编译库缺陷（已定位）**：`oneye_ble_plat_nimble.c` 的实现体受
`#if defined(ESP_PLATFORM) && defined(CONFIG_BT_ENABLED) && defined(CONFIG_BT_NIMBLE_ENABLED)` 保护，
而 P1 交叉编译产出的 `lib/xtensa-esp32s3-elf-gcc-14.2.0/liboneye_dev_ble.a` 是在**未定义这两个 CONFIG** 的
配置下构建的 ⇒ 库里只有宿主桩（`oneye_ble_plat_supported() == false`），设备上 `oneye_dev_ble_init()` 恒返回
`ERR_UNSUPPORTED`。修法（下一轮）：用开启了 BT/NimBLE 的配置重发该预编译库，并加一条 ABI/自检断言防回归。

**其余待真机取证项**（依赖上述缺陷修好后才有意义）：

1. 长按 600 ms 触发收音的实际时延与阈值行为；短按/播放中长按的打断路径；
2. 单声道→立体声回放（本工程自行复制声道，S3 上 ADF 的 `i2s_mono_fix()` 不参与编译）；
3. 打断时延（`input.cancel` → 停止回播）是否 ≤200 ms；`down_drops` 丢帧率；
4. BLE(NimBLE)+Wi-Fi+AFE 同跑时的内部 RAM 余量。

**实现层面的已知取舍**：

- 本工程**不启用** link 的 `wan` 信道（`wan_uri = NULL`）：语音面走独立的 voice WS；link 广域信道需要服务端 `/v1/link/ws` 支持，尚未落地；
- 局域网信道设备→手机方向采用「HTTP 响应携带」（`lan_link.c` 头注）：`contracts/local/lan-link.md` 只定义了请求方向，异步推送口径待 R-L1 会签后定稿；
- 未配对（无令牌）时局域网只放行 `prov.hello` / `link.ping` / `node.announce`（契约 §2 口径）；令牌协商（BLE 配对成功后下发）尚未实现。

## 8. 合规

- 会话音频与转写正文**不落库、不落盘**；本工程亦不写 SD（仅读凭据文件）；
- `ONEYE_LLM_ENABLE_WIFI_FILE` 与 `ONEYE_LLM_ENABLE_LAN_LINK` 是**台面/研发**便利开关，量产固件必须置 `n`（改走 claim + 一机一密、BLE 加密配网）；
- 生产部署不使用 AWS；语音小服务部署形态见总控 ADR-0015（提议）。
