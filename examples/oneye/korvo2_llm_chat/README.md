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
| `ONEYE_LLM_SELFTEST_TURN_MS` | 0 | 台面自检：非 0 时语音面就绪后自动收音该时长并提交一轮（无人值守验证整条链路；量产必须 0） |

## 7.1 真机取证（2026-09-17，逐轮累计）

### 7.1.1 ✅ 已闭环：开机 → 联网 → 会话 → 收音 → 提交 → 服务端返回 → 喇叭回放（第 8 轮）

台面自检档（`ONEYE_LLM_SELFTEST_TURN_MS=3000`，免按键走与长按完全相同的代码路径）串口实录：

```
prov_service: 已联网：ip=192.168.110.80 source=kconfig
panel: [diag] HTTP GET http://192.168.110.208:9091/healthz → ESP_OK（status=200）
llm_client: 语音面客户端：ws://192.168.110.208:9091/v1/voice/ws（子协议 oneye.voice.v1）
websocket_client: Started
llm_client: 状态 → SESSION_START：已连接，发 session.start
llm_client: 发送 session.start（device_id=korvo2-llm-0001）
llm_client: 状态 → READY：会话就绪                      ← 服务端 session.ready
panel: [selftest] 自动收音 3000 ms（台面自检，非按键路径）
MODEL_LOADER: Successfully load srmodels                ← model 分区生效
AFE: AFE Version: (1MIC_V250121)
AFE: Input PCM Config: total 2 channels(1 microphone, 1 playback), sample rate:16000
AFE: AFE Pipeline: [input] -> |AEC(VOIP_LOW_COST)| -> |NS(nsnet2)| -> [output]
AUDIO_PIPELINE: Pipeline started
voice_io: 采集开始（每帧 1920 B = 60 ms @16 kHz/16 bit/单声道）
llm_client: 状态 → LISTENING
voice_io: 采集已停止（本次 82560 B）                     ← 43 帧 × 1920 B
panel: [selftest] 自动提交本轮（上行 82560 B）
llm_client: 状态 → THINKING                             ← input.audio.commit
llm_client: 状态 → SPEAKING                             ← 首帧下行 TTS（rtt 369~412 ms）
AUDIO_HAL: Codec mode is 3, Ctrl:1
voice_io: 回放打开（16 kHz/16 bit → 立体声 → ES8311）
llm_client: 状态 → READY：本轮结束                       ← tts.end / turn.end
voice_io: 回放已关闭（本轮下行 88320 B）
panel: [panel] 本轮结束：turn_seq=1 rtt=412 ms
```

服务端（chatd，stub provider）同窗口日志：**1 条连接，`active=0` 升级后无冲突、无重连**，
会话持续到设备被人工复位才以 20 s 读超时收场：

```
voice: ws 已升级 peer=192.168.110.80:53151 active=0          （会话建立）
…（82 s 内持续收到设备保活帧，读超时判据一直被刷新）…
voice: 连接结束 err="…:53151: i/o timeout"                    （设备被 esptool 复位后静默 20 s）
```

⇒ **上行 82,560 B / 下行 88,320 B / 首帧时延 369–412 ms，全程无复位、无 `conflict`**。
服务端那条 20 s 超时是**人工烧写导致设备静默**造成的，同时也**反证了设备保活帧确实在刷新服务端读超时**
（否则会话早在 20 s 就断）。

### 7.1.2 ✅ 人手长按 REC 键的真实按键取证（第 8 轮）

按住 REC 键 4.9 s 后松开，串口实录（`turn_seq=2`，即同一启动内的第二轮）：

```
key_talk: REC 按下
key_talk: 长按达标（579 ms ≥ 600 ms）→ 开始收音        ← 见下方口径说明
AUDIO_HAL: Codec mode is 1, Ctrl:1
MODEL_LOADER: Successfully load srmodels
AFE: AFE Pipeline: [input] -> |AEC(VOIP_LOW_COST)| -> |NS(nsnet2)| -> [output]
voice_io: 采集开始（每帧 1920 B = 60 ms @16 kHz/16 bit/单声道）
llm_client: 状态 → LISTENING
key_talk: REC 松开（按住 4948 ms）→ 提交本轮
voice_io: 采集已停止（本次 126720 B）                   ← 66 帧 × 1920 B
llm_client: 状态 → THINKING
llm_client: 状态 → SPEAKING                             ← rtt 531 ms
voice_io: 回放打开（16 kHz/16 bit → 立体声 → ES8311）
llm_client: 状态 → READY：本轮结束
voice_io: 回放已关闭（本轮下行 179200 B）
panel: [panel] 本轮结束：turn_seq=2 rtt=531 ms
```

⇒ **长按触发 → 收音 → 松手提交 → 服务端返回 → 喇叭回放** 这条人手路径与自检轮完全一致地闭环。

> **口径说明（579 ms vs 阈值 600 ms）**：`key_talk` 打印的 held 是**自本模块收到"按下"事件起算**的时间，
> 而"长按达标"由 ADF `input_key_service` 按**其自身计时**判定；我们的起点晚于真实按下时刻
> （差一个 ADC 扫描周期 + 事件投递延迟），所以该打印值可能略小于阈值 —— 属正常误差，不代表阈值失效。
> 已把这条口径写进 `key_talk.c` 的日志与注释（日志文案改动，未重新烧写）。

### 7.1.3 ✅ BLE 配网在真机上初始化成功（第 9 轮：GATT 注册 + 广播 + POP）

第 2 轮只是"让预编译库带上 NimBLE 符号"，设备侧仍停在 `oneye_dev_ble_init=-4`。第 9 轮定位到真正根因并修复：

**根因（两层，缺一不可）**

1. **`access_cb = NULL` 违反 NimBLE 的 GATT 定义校验**（真机日志 `E NimBLE: ble_gatts_count_resources rc=3`）：
   `rc=3` 是 **`BLE_HS_EINVAL`**，不是 ENOMEM —— NimBLE 用类 errno 编号
   （`EAGAIN=1 / EALREADY=2 / **EINVAL=3** / EMSGSIZE=4 / ENOENT=5 / ENOMEM=6`，见 `host/ble_hs.h`），
   而 `ble_gatts_chr_is_sane()` 明确要求 `chr->access_cb != NULL`。自研 GATT 里 Dev→App 的
   **notify 特征**（只发不收）留了 `access_cb = NULL` ⇒ 计数阶段就失败。
   处置：给该特征一个显式回调 `gatt_tx_access()`（回 `BLE_ATT_ERR_UNLIKELY`；因为 flags 只有 NOTIFY，
   ATT 层不会把读/写路由到这里，回调实际不可达）。
   ⚠️ 早期把 `rc=3` 当 ENOMEM 排查（资源上限/内存不足）方向全错。
2. **例程链接的是预编译库**（`components/oneye-dev-sdk/CMakeLists.txt`：`if(EXISTS lib/<toolchain>/*.a)`），
   所以改源码后必须**重新生成库**才生效：
   `IDF_ROOT=$HOME/esp ./build-all.sh --toolchains esp32s3@5.5.5`（`lib/**` 是 gitignore，不入库）。

**随之修掉的两个 BLE 侧问题**

| 现象 | 根因 | 处置 |
| --- | --- | --- |
| `W oneye_ble: adv fields rc=4` | 广播载荷 33 B > 传统广播 31 B 上限（flags 3 + 128 bit UUID 18 + 设备名 12） | 广播数据只放 flags + 128 bit 服务 UUID；**设备名改放扫描响应**（`ble_gap_adv_rsp_set_fields`） |
| `E websocket_client: Error create websocket task` → `WebSocket 启动失败` | BLE 打开后内部 RAM 变紧，WS 任务栈 8192 创建失败 | WS 任务栈 **3584** / 缓冲 **2048**（都 < `SPIRAM_MALLOC_ALWAYSINTERNAL=4096`，落内部 RAM） |

**BLE + 语音闭环共存的内存预算（本轮关键取舍）**

打开 BLE 后音频管线被挤掉（`E voice_io: I2S 读元素创建失败` → `采集任务创建失败` → 自检轮无法收音）。
按"采集前内存取证"逐项腾内部 RAM：

| 阶段 | 内部余量 | 最大连续块 | 结果 |
| --- | --- | --- | --- |
| 仅改 WS 栈/广播拆分 | 42,655 B | 25,600 B | ❌ 采集任务创建失败 |
| ＋NimBLE 主机侧分配改 PSRAM | 56,051 B（PSRAM 8.4 MB） | 31,744 B | ✅ 采集/播放/AFE 全通 |

配套默认值（已进 `sdkconfig.defaults`）：`BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL=y`、
`MSYS1/MSYS_1/MSYS_2_BLOCK_COUNT=12`（原 24）、`ESP_WIFI_STATIC_RX_BUFFER_NUM=8`（原 16）、
`ESP_WIFI_DYNAMIC_RX_BUFFER_NUM=16`（原 32）。

**真机实录（BLE 配网与语音闭环同时开启）**

```
I (1355) oneye_ble: GATT 服务已注册（RX 句柄=0 TX 句柄=0，MTU 期望 247）
I (1376) NimBLE: GAP procedure initiated: advertise;
[oneye][ble] 配网配对码 POP=187078（300 s 内有效）
I (1389) prov_service: BLE 配网已就绪（未配网时会以 ONEYE-<id 后 4 位> 广播；POP 在 start 时打印）
I (3568) llm_client: 状态 → SESSION_START：已连接，发 session.start
I (3748) llm_client: 状态 → READY：会话就绪
I (6750) panel: [selftest] 采集前内存：内部余 56051 B / 总余 8403271 B / 最大块 31744 B
I (6977) voice_io: 采集开始（每帧 1920 B = 60 ms @16 kHz/16 bit/单声道）
I (10325) voice_io: 采集任务退出（累计 78720 B）
I (10807) llm_client: 状态 → SPEAKING
I (13382) panel: [panel] 本轮结束：turn_seq=1 rtt=438 ms
```

**仍未做**：手机侧（`mobile/Android`）真机连这个 GATT 服务走完 POP 配对 + 下发 Wi-Fi 凭据的端到端流程
（本机缺 JDK/Gradle/Android SDK，见 `mobile/README.md` 的"待验收（缺工具链）"清单）。

### 7.1.4 根因：`link_report_task` 栈溢出踩坏自旋锁 → CPU1 中断看门狗（第 8 轮定位并修复）

第 7 轮及以前把周期性复位判成"平台级关中断停顿、与例程逻辑无关"，**是错的**。第 8 轮用
`xtensa-esp32s3-elf-addr2line` 解码 panic 的两核 dump 后定位到明确的应用侧根因：

```
Guru Meditation Error: Core  1 panic'ed (Interrupt wdt timeout on CPU1).
Backtrace: 0x4037ae8c 0x4037f3f9 0x4037eddf 0x4200f831 0x4037f1ad
  → esp_cpu_compare_and_set            (cpu.c:200)
  → spinlock_acquire / xPortEnterCriticalTimeout   (port.c:489)
  → xQueueReceive                      (queue.c:1549)
  → link_report_task                   (prov_service.c:135)
  → vPortTaskWrapper
```

- **机理**：`link_report_task` 的栈在 `xQueueCreate(s_report_q)` **之后**创建，两者在堆上**紧邻**。
  该任务栈取 4096 B 时，`oneye_dev_link_prov_report_status()` → `oneye_dev_link_broadcast()`
  组帧路径的峰值（实测 ≈5.2 KB）**向下溢出**，第一个被踩坏的对象就是紧邻其下的**队列本体**
  （内含 `xQueueLock` 自旋锁的 owner/count）。锁被写成垃圾值后，任务下一轮
  `xQueueReceive(portMAX_DELAY)` 会**在关中断状态下永久自旋**在这把锁上：
  既不让出 CPU，也永远不会触发 FreeRTOS 的栈金丝雀检查（它只在上下文切换时检查），
  于是只能由 3 s 中断看门狗收场 —— 表现为"每 ~6 s 复位一次"，且复位点看起来总在"联网/建 WS 之后"。
- **为什么以前查不出来**：dump 出的两个核都显示"正常阻塞态"（Core 0 = idle），
  因为**持有锁的任务根本不存在**（锁的值是垃圾），于是被误读成"关中断期过长"的平台问题。
- **修复**：`link_report_task` 栈 4096 → **12288**（`LINK_REPORT_TASK_STACK`），
  并在每次上报后打印 `uxTaskGetStackHighWaterMark` 取证。
- **取证**：修复后串口首条上报即打印 `配网上报完成 state=5，栈余量 7028 B`
  ⇒ 该任务实际峰值 ≈5.2 KB，**旧的 4 KB 栈确实不够**；此后连续 3 次 50~85 s 真机捕获
  **0 次复位、0 次 panic**。
- **同时清掉的隐患**：`llm_client.c` 里 `LLM_PING_MS` 被**重复定义**（顶部 20000 / 中部 10000），
  后者静默覆盖前者（编译只报 warning），导致"注释写 20 s、实际 10 s"的取证口径不一致；
  现已合并为顶部单一定义（10 s），并加 `ESP_LOGD` 保活取证（需 `CONFIG_LOG_MAXIMUM_LEVEL_DEBUG` 才可见）。


**第 7 轮已修的两个真机问题**（都在应用侧）：

1. `xTaskCreate(prov_boot, 16 KB)` 直接失败 → 设备停在 BOOT。BLE(NimBLE) 真起来后内部 RAM 紧张，
   12/16 KB 任务栈创建失败；且 SDK 组帧已改堆分配（不再需要大栈）⇒ 配网任务降到 6 KB、
   httpd 6 KB。⚠️ **当时把上报任务一起压到 4 KB 是错误的**（见 §7.1.4 根因），现已回到 12 KB。
2. `esp_wifi_start()` 与 `set_config/disconnect/connect` 挤在同一任务里连调 → 触发
   `Interrupt wdt timeout`（串口先报 `E wifi:sta is connecting, return error`）；
   改为**初始化阶段就 start**，配网只做 set_config + connect；另把
   `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP` 置 n（Wi-Fi/lwIP 缓冲留内部 RAM）。

### 7.1.5 第 7 轮已修：保活/读超时导致的"重复连接"噪声

`esp_websocket_client` 的 `network_timeout_ms` 被当作 **socket 读超时**：会话就绪后若一段时间没有下行数据，
组件判"读失败"并重连 ⇒ 服务端按契约（每设备并发 1）把第二条连接拒为 `conflict` 并关闭 ⇒ 设备再重连，
形成噪声循环（现象：`session.ready` 后 ~4 ms 收到 `conflict`）。处置：读超时 8 s → **15 s**、
打开**协议层 ping**（`ping_interval_sec=5`，服务端 gorilla 自动回 pong）、
应用层契约帧 `ping` 周期 **10 s**（`LLM_PING_MS`，服务端 12 s 陈旧窗口据此判定）；
服务端同步收敛（`StaleSessionWindow` 25→12 s、WS ping 5 s、读超时 20 s、**收到 pong 也算活跃**）。
修复后该轮已能稳定走到 `LISTENING`。

### 7.1.6 第 7 轮的其他结论（部分已被 §7.1.4 取代，保留作排查记录）

1. **联调环境侧真相：Windows 防火墙按"程序完整路径"放行**。本机入站规则只对**历史出现过的
   chatd.exe 路径**放行（如 `%LOCALAPPDATA%\go-build\<hash>\chatd.exe`）；用 `go run`（每次新临时路径）
   起的 chatd **不被放行**，表现为设备侧 `esp_transport_connect … CONNECTION_TIMEOUT`、
   HTTP 探针 `ESP_ERR_HTTP_CONNECT`。把 chatd 构建到已放行路径后，设备**立刻**连通：
   `[diag] HTTP GET …/healthz → ESP_OK（status=200）` + WS `session.ready`。
   ⇒ 台面联调请固定用**同一个二进制路径**跑 chatd（见 backend `scripts/e2e/`）。
2. ~~`Interrupt wdt timeout` 是周期性平台停顿、与例程逻辑无关~~ —— **此结论已被 §7.1.4 推翻**：
   是 `link_report_task` 栈溢出踩坏队列自旋锁。注意台面验证档**不要**再用 `CONFIG_ESP_INT_WDT=n`
   掩盖问题（那只会把 panic 变成静默 `rst:0x7 (TG0WDT_SYS_RST)`，丢失唯一的现场）。
3. "一次启动内开两条 WS 连接"（同 device_id 第二条在 14–18 s 后到达被 `conflict` 拒绝）已随之消失：
   起因是组件读超时判死重连，已由 §7.1.5 的保活参数 + 服务端更快回收共同修掉；
   另加了两处防重（`on_net_ready` 幂等、`llm_client_start()` 防并发重入）与 Wi-Fi 连接次序修正
   （已连上时才 disconnect，避免两次 `GOT_IP`）。

新增台面诊断开关 `ONEYE_LLM_DIAG_HTTP_PROBE`（缺省 n）：建 WebSocket 之前先用普通
HTTP `GET /healthz` 探同一 host:port，用来判定"是 socket/lwIP 层面"还是"WebSocket 组件"；
该开关同时把 `llm_client` 的运行期日志级别提到 DEBUG（**要看 `ESP_LOGD` 还需
`CONFIG_LOG_MAXIMUM_LEVEL_DEBUG=y`**，默认 INFO 档下 DEBUG 语句被编译掉）。

### 7.1.7 构建与镜像

**构建（已通过）**：ESP-IDF v5.5.5 + ESP-ADF v2.8，`idf.py build` 成功，
应用镜像 ≈1.60 MB（`0x186350`），落在 `factory` 3 MB 分区内（余量 49%）；
`srmodels.bin` 随 `model` 分区投放（`0x310000`）。

**真机（ESP32-S3-Korvo-2，COM12，已烧写取证）**：

| 环节 | 结果 | 证据 |
| --- | --- | --- |
| 启动 / 分区 / PSRAM | ✅ | 分区表与 `partitions.csv` 一致；octal PSRAM 8 MB 探测通过 |
| 板级音频初始化 | ✅ | `ES8311 in Slave mode` / `ES7210 Enable MIC1..3` + `Enable TDM mode` |
| REC 键长按服务 | ✅ | `key_talk: REC 键就绪：长按 ≥ 600 ms 开始收音`（`press_judge_time` 覆写生效）；ADC 校准成功 |
| 凭据文件配网（本板 SD 卡 `/sdcard/oneye-wifi.txt`） | ✅ | `prov_service: 凭据文件命中 … ssid=wanya` → `已联网：ssid=wanya ip=192.168.110.83 source=file` |
| 局域网链路 | ✅ 启动 | `lan_link: UDP 发现已就绪（57321）` + `POST http://<ip>:80/api/link/frame` 已注册 |
| BLE 配网 | ✅ 已通（第 9 轮） | `GATT 服务已注册` + `POP=xxxxxx` + `BLE 配网已就绪`；与语音闭环同跑（见 §7.1.3） |
| 语音面一轮闭环 | ✅ 已闭环 | 见 §7.1.1：上行 82,560 B / 下行 88,320 B / 首帧 369–412 ms，无复位、无 `conflict` |

**开机即复位的缺陷 → 第 2 轮已修（关键根因在 SDK 侧）**：

- 现象（第 1 轮）：连上 Wi-Fi 后创建 WebSocket 客户端时复位（`Guru Meditation … LoadProhibited` 或
  `Interrupt wdt timeout on CPU0`，两者都落在 heap 侧）；
- 根因（`xtensa-esp32s3-elf-addr2line` 解码回溯源 + 代码复核）：`oneye_dev_link_send()` 的组帧路径
  **在栈上开 8 KB**（`oneye_dev_link.c:558 char frame[ONEYE_DEV_LINK_FRAME_MAX_BYTES]`），
  叠加 `oneye_link_frame_build()` 里那份**本不需要**的 `payload[2049]` 拷贝 ⇒ 公共 API 单次调用栈峰值 ≈11 KB；
  而它会被系统事件任务（3 KB）、httpd 任务（4 KB）等小栈任务调用 ⇒ 踩穿栈 → 破坏堆 → 之后任意 malloc 崩。
- **已修（SDK 侧，第 2 轮）**：组帧缓冲改**堆分配**（每次调用申请/释放，失败返回 `ERR_NO_MEM`）；
  `oneye_link_frame_build()` 去掉 2 KB 栈拷贝（改为直接校验 `frame->p[0]`）。
  回归：宿主单测 **20 组 / 235 用例 / 3261 断言、0 失败**；ESP 六库重发。
- **应用侧加固**：所有 link 上报走独立 **12 KB** 任务（`link_report_task` + 队列；⚠️ 曾误压到 4 KB，
  反而引入 §7.1.4 的栈溢出根因）、配网启动走 6 KB 任务、httpd 栈 6 KB、
  `CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192`、`CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE=4096`。

**BLE 预编译库缺陷 → 第 2 轮已修（两处，缺一不可）**：

1. `build-all.sh` 的 IDF 探针 `IDF_PROBE_REQUIRES` 缺 `bt esp_wifi`（且探针 sdkconfig 未开 BLE）
   ⇒ 生成的编译参数里没有 `CONFIG_BT_ENABLED/CONFIG_BT_NIMBLE_ENABLED`；
   已补齐 REQUIRES、给探针加 `sdkconfig.defaults`（开 BT + NimBLE）、并加 `IDF_PROBE_REV` 使缓存失效；
2. `oneye_ble_plat_nimble.c` / `oneye_dev_ble.c` 用 `#if defined(CONFIG_*)` 判定能力，却**没包含 sdkconfig.h**
   ⇒ 条件恒假、实现体被整段裁掉。已显式 `#include "sdkconfig.h"`（ESP 平台）。
   **取证**：`liboneye_dev_ble.a` 41,192 B（只有宿主桩）→ **54,100 B 且含 `nimble_port_init`/`ble_gatts` 引用**；
   设备侧日志由 `oneye_dev_ble_init 失败：-5（UNSUPPORTED）` 变为真正跑 NimBLE 初始化。

**BLE 配网（第 9 轮已通，见 §7.1.3）**

设备侧已能：`GATT 服务已注册` → 广播（设备名走扫描响应）→ `POP` 打印 → `BLE 配网已就绪`，
并与语音闭环同跑。**仍未做**的是手机侧真机走完 POP 配对 + 下发 Wi-Fi 凭据的端到端流程
（`mobile/Android` 工程已就位，但本机缺 JDK/Gradle/Android SDK，构建与单测登记"待验收（缺工具链）"）。

**其余待真机取证项**：

1. **人手长按 REC 键** —— 已取证（见 §7.1.2，`turn_seq=2` 闭环）；短按 / 播放中长按的**打断时延**尚未实测；
2. 单声道→立体声回放（本工程自行复制声道，S3 上 ADF 的 `i2s_mono_fix()` 不参与编译）—— 已完成一轮，
   听感/音量待人工确认；
3. 打断时延（`input.cancel` → 停止回播）是否 ≤200 ms；`down_drops` 丢帧率；
4. BLE(NimBLE)+Wi-Fi+AFE 同跑时的内部 RAM 余量 —— 已有取证：采集前内部余 **56,051 B**
   （最大连续块 31,744 B，见 §7.1.3）；`link_report_task` 上报路径栈余 6,884~7,028 B / 12 KB。

**实现层面的已知取舍**：

- 本工程**不启用** link 的 `wan` 信道（`wan_uri = NULL`）：语音面走独立的 voice WS；link 广域信道需要服务端 `/v1/link/ws` 支持，尚未落地；
- 局域网信道设备→手机方向采用「HTTP 响应携带」（`lan_link.c` 头注）：`contracts/local/lan-link.md` 只定义了请求方向，异步推送口径待 R-L1 会签后定稿；
- 未配对（无令牌）时局域网只放行 `prov.hello` / `link.ping` / `node.announce`（契约 §2 口径）；令牌协商（BLE 配对成功后下发）尚未实现。

## 8. 合规

- 会话音频与转写正文**不落库、不落盘**；本工程亦不写 SD（仅读凭据文件）；
- `ONEYE_LLM_ENABLE_WIFI_FILE` 与 `ONEYE_LLM_ENABLE_LAN_LINK` 是**台面/研发**便利开关，量产固件必须置 `n`（改走 claim + 一机一密、BLE 加密配网）；
- 生产部署不使用 AWS；语音小服务部署形态见总控 ADR-0015（提议）。
