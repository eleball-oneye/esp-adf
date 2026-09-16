# korvo2_oneye —— ESP32-S3-Korvo-2 板级固件（ESP-ADF）

> **定位**：ESP32-S3-Korvo-2 V3.1 的**板级固件工程**：把「硬件文档（rst）→ 板级实现 → 固件自证」这条链
> 打成**可构建、可自证**的产物；同时以 `oneye-dev-sdk` 接入设备面契约（EMQX）。
>
> **事实源（两条，缺一不可）**
> 1. 硬件：`docs/zh_CN/design-guide/dev-boards/user-guide-esp32-s3-korvo-2.rst`（V3.1，板级唯一文档事实源）
> 2. 板级实现：`components/audio_board/esp32_s3_korvo2_v3/{board.c,board_def.h,board_pins_config.c}`
> 3. 协议：`backend/contracts/api/mqtt/{asyncapi.yaml,传输规范.md,Korvo-2设备能力与协议映射.md}`（四端契约冻结版）
>
> **工具链口径**：ESP-ADF v2.8 官方仅支持 **IDF v5.1–v5.5**；IDF 6.0.x 因移除内置 `json` 组件导致 `esp-sr`
> 依赖断裂 ⇒ **本固件单轨 IDF v5.5.5**（SDK 库本身仍可按 5.5.5 / 6.0.3 双档构建）。

## 1. 构建

> **前置：给 IDF v5.5.5 打 ADF 的 FreeRTOS 补丁**（真机实测必需，否则按键不可用）。
> ADF 的 `audio_thread` 用 `xTaskCreateRestrictedPinnedToCore()` 给按键等任务分配**静态栈**；
> IDF v5.5.5 未含该函数 ⇒ ADC 按键任务创建失败（串口：`Not found right xTaskCreateRestrictedPinnedToCore`
> → `ADC_BTN: Create button_task failed!`）。上游 `idf_patches/idf_v5.5_freertos.patch` 面向 v5.5.0，
> 在 v5.5.5 上**上下文已变（`git apply --check` 失败）**，故本 fork 增补适配版：
>
> ```bash
> cd ~/esp/esp-idf-5.5.5
> git apply <repo>/embedded/esp-adf/idf_patches/idf_v5.5.5_freertos.patch   # 已在 WSL IDF 上实打并实测通过
> ```

```bash
# 一次 shell 只导出**一个** IDF 版本（多版本 export 会互相污染 PATH）
source ~/esp/esp-idf-5.5.5/export.sh
export ADF_PATH=<repo>/embedded/esp-adf        # 本工程经 ADF 的 CMakeLists 取 ADF 组件
export CCACHE_ENABLE=0                         # 并行构建时 ccache 竞争会 ICE（见集成说明 §4.3）

cd <repo>/embedded/esp-adf/examples/oneye/korvo2_oneye
rm -rf build                                   # 残留 build 会让 set-target **静默回退** esp32
idf.py set-target esp32s3
idf.py build
```

构建后请核对 `sdkconfig`：`CONFIG_IDF_TARGET="esp32s3"`、`CONFIG_ESP32_S3_KORVO2_V3_BOARD=y`
（两者缺一即视为"没按本板构建"）。

## 2. 烧录（2026-09-15 **已实测**：Windows 侧 esptool → COM12，见 §8）

```bash
idf.py -p <COMx> flash monitor        # Windows；Linux/macOS 为 /dev/ttyUSB0 或 /dev/ttyACM0
```

- Boot 键 + Reset 键进入下载模式；或由串口 DTR/RTS 自动下载（rst「自动下载」）。
- 扬声器接 **扬声器输出端口**；USB 供电建议 ≥5 V/2 A（rst「供电说明」）。
- **首次上电前**（本仓库为 Windows 侧烧录准备，见 `output/.build/flash-korvo2.ps1`）：
  1. 把 [`sd-root/oneye-wifi.txt`](sd-root/oneye-wifi.txt) 拷到 microSD 卡**根目录**并填好 `ssid=` / `password=`，插卡；
  2. 烧写四个镜像（**必须先 `-Erase`**：分区表已改为 `factory 2M + model 2M + storage 1M`）：
     `0x0 bootloader.bin` / `0x8000 partition-table.bin` / `0x10000 korvo2_oneye.bin` / `0x210000 srmodels/srmodels.bin`;
  3. 复位后看串口：`从凭据文件读取 Wi-Fi` → `Wi-Fi 已获取 IP：…` → `已联网 → 启动上云` → `本地验证面板：http://<IP>/api/status`。
     若没看到，按 §5.4 的排查口径逐条核对（卡未挂载 / 文件不在根目录 / 键名拼写 / 密码错误）。

## 3. 板级参数核对表（rst ↔ ADF ↔ 固件自证）

「自证位置」列给出**固件把该期望值变成可执行断言**的位置：
`board_expect.h` = 编译期断言（宏一致性，构建即失败）；`board-selftest` = 运行期调板级查询 API 比对并打印 PASS/FAIL。

| 分组 | 项 | rst 出处 | ADF 实现 | 自证位置 | 结论 |
| --- | --- | --- | --- | --- | --- |
| I²C | SDA=IO17 / SCL=IO18（编解码器、摄像头 SCCB、LCD 触摸、IO 扩展器共用） | 管脚分配列表；I2C 测试点 J18 | `board_pins_config.c:35-48` | board-selftest | 一致 |
| I²S0 | MCLK=IO16 / SCLK=IO9 / LRCK=IO45 / DSDIN=IO8 / ASDOUT=IO10 | 编解码器测试点 J15；ADC 测试点 J16 | `board_pins_config.c:50-72` | board-selftest | 一致（I2S1 明确不支持） |
| 回放 | ES8311 编解码器 + NS4150 功放；PA_CTRL=IO48；板级增益 6 dB | 组件介绍；管脚分配列表 IO48 | `board.c:60-66`；`board_def.h:117,119,120` | board_expect.h + board-selftest | 一致 |
| 采集 | ES7210 4 通道 ADC；MIC1\|MIC2\|MIC3；32 bit；48 kHz；硬件 AEC；输入格式 `"RMNM"` | 组件介绍；AEC 电路 | `board.c:50-58`；`board_def.h:114-116,121,126` | board_expect.h + board-selftest | 一致（口径说明见 §4） |
| 按键 | 6 键 REC/MUTE/PLAY/SET/VOL-/VOL+，共用 IO5 一路 ADC 梯形 | 组件介绍；管脚分配列表 IO5 | `board.c:155-171`（ADC1_CH4 + 7 档阈值）；`board_def.h:146-153` | board_expect.h（键数/ID）+ board-selftest（key_init） | 一致（阈值/通道属 `board.c` 私有常量，应用侧不可见 → 只能以按键事件为证） |
| microSD | DATA0=IO4、CMD=IO7、CLK=IO15；**仅一线模式、无卡检测** | 管脚分配列表；组件介绍（一线模式） | `board.c:173-201`（非 `SD_MODE_1_LINE` 直接失败）；`board_def.h:59-75` | board_expect.h + board-selftest（可选） | 一致；无热插拔（见 §4） |
| LCD | SPI：CLK=IO1、SDA/MOSI=IO0、DC=IO2；CS/RST/BL 经 TCA9554 P3/P2/P1；320×240、RGB565、SPI 60 MHz | LCD 连接器；IO 扩展器 GPIO 分配；组件介绍 | `board.c:89-153`；`board_def.h:39-53` | board_expect.h + board-selftest（可选开 LCD） | 一致（分辨率口径见 §4） |
| IO 扩展器 | TCA9554：P0 未分配、P1 LCD_CTRL、P2 LCD_RST、P3 LCD_CS、P4 TP_INT、P5 PERI_PWR_ON、P6 LED1、P7 LED2 | IO 扩展器 GPIO 分配 | `board.c:91-108`（仅 P1/P2/P3 被驱动）；`board_def.h:31-42,104-107` | board_expect.h（P1/P2/P3/P6/P7） | **部分**：P4/P5 在 ADF 无驱动（见 §4） |
| 摄像头 | DVP 8 位并口：XCLK=IO40、SIOD=IO17、SIOC=IO18、VSYNC=IO21、HREF=IO38、PCLK=IO11 + 数据脚 | 摄像头连接器；管脚分配列表 | `board_def.h:81-98`（`FUNC_CAMERA_EN=1`） | board_expect.h | 一致（**命名偏移**见 §4；驱动为外部 `esp32-camera`） |
| 指示灯 | 蓝=IO 扩展器 P7、红=P6，无绿灯 | 组件介绍；IO 扩展器 GPIO 分配 | `board_def.h:31-33`；`board_pins_config.c:154-162`；**`board.c:84-87` 的 `audio_board_led_init()` 直接 `return NULL`** | board_expect.h + board-selftest（引脚查询） | **引脚一致 / 无实现**：ADF v2.8 端口未提供 LED 显示服务 |
| 耳机检测 | 不支持 | 管脚分配列表（无该信号） | `board_def.h:118`（`-1`） | board_expect.h + board-selftest | 一致 |
| 其它引脚 | IO6 BAT_MEAS_ADC；IO19/IO20 ESP_USB_DM/DP（预留）；IO46/IO35/IO36/IO37 NC；EN/UART0 | 管脚分配列表 | **无对应宏** | —（不可断言） | 见 §4 |

## 4. 不可断言项 / 未确认项（**不据推测实现**）

以 [Korvo-2 设备能力与协议映射页](../../../../../backend/contracts/api/mqtt/Korvo-2设备能力与协议映射.md) §6 为准，本工程只登记：

1. **物理麦克风数量**：rst「组件介绍」列的是**左/右两枚板载麦克风**，rst「AEC 电路」说明回声参考信号经
   **ADC_MIC3P/ADC_MIC3N** 采回 ⇒ 软件侧的 `MIC1|MIC2|MIC3` 中 **MIC3 是 AEC 回采通路而非第三枚实体麦克风**。
   映射页 §1-#2 的"3 路麦克风"应读作"3 路 ADC 通道（2 麦 + 1 回采）"；物理数量仍需 BOM/实测（§6-6）。
2. **`PERI_PWR_ON`（TCA9554 P5）**：rst 列为外设电源使能，但 ADF v2.8 全仓**无任何代码驱动该位**（grep 零命中）。
   是否影响相机模组/LCD 扩展板供电**未确认**——需原理图核对或真机实测；本工程不新增推测性驱动。
3. **触摸面板**（TP_I2C + TP_INT=P4）：`FUNC_LCD_TOUCH_EN=1` 只是参数宏，`board.c` 无触摸控制器初始化（§6-2）。
4. **microSD 热插拔**：`SDCARD_INTR_GPIO = -1` + `ESP_SD_PIN_CD = -1` ⇒ 无卡检测线路；固件**只能上报挂载成败**，
   不得伪造插入/拔出事件（§6-3、R3-5）。
5. **摄像头模组是否装配 + 真机取帧**：代码侧只证明"声明为支持 + 引脚齐备 + 树内存在以本板为默认板的取帧例程"
   （`examples/display/lcd_camera` 等），物理装配需硬件核对、取帧需真机（§6-1）。
6. **摄像头数据脚命名偏移**：rst「摄像头连接器」用 `D2…D9` 命名，ADF `cam_pin_d0…d7` 为**同一物理网络、编号整体前移两位**
   （ADF `D0`=IO13=rst `D2`、ADF `D7`=IO39=rst `D9`）。这是**命名口径差异**、非引脚冲突；本工程按 ADF 命名断言。
7. **LCD 分辨率口径**：rst 写"240×320（面板）"，ADF 用 `LCD_H_RES=320 / LCD_V_RES=240 + LCD_SWAP_XY=false`
   （横屏 320×240 呈现）。二者描述同一面板的不同书写口径，**不是**配置错误。
8. **电池电压采集**（IO6 BAT_MEAS_ADC）：ADF 无板级配置（`battery_service` 无 korvo2 板级实例），
   且契约无承载通道（R3-5 邻近项）——本工程不实现。
9. **板级 I²S 位宽口径**：`AUDIO_CODEC_DEFAULT_CONFIG()` 的 `bits = 16`（回放/编解码器 HAL 侧），
   而 ADC 侧 `CODEC_ADC_BITS_PER_SAMPLE = 32`（16 bit 麦 + 16 bit AEC 回采，采集轨 `algorithm_stream`/AEC 例程消费）。
   两者分别作用于回放与采集轨，**不是同一配置项的矛盾**；采集运行期取证（真机录音）仍需单独做。
10. **`"RMNM"` 的消费方**：该字符串由 `components/audio_stream/include/algorithm_stream.h:153,173` 与
    recorder/AEC 例程消费（喂 esp-sr AFE），**板级初始化本身不解析它**——所以"格式正确"只能靠断言 + AFE 例程构建/实跑证明。

## 5. 自检输出解读

启动日志分两段：

- 构建期（`board_expect.h`）：任何 `_Static_assert` 失败 → **编译失败**，无需人工比对；
- 运行期（`board-selftest`，TAG=`korvo2_oneye`）：

```
I (xxx) korvo2_oneye: ==== Korvo-2 板级参数自检（rst V3.1 ↔ board_pins_config.c）====
I (xxx) korvo2_oneye: [board-check] i2c.sda (rst J18)                expect=17     actual=17     PASS
...
I (xxx) korvo2_oneye: [board-check] adc_input_ch_format              expect=RMNM   actual=RMNM   PASS
I (xxx) korvo2_oneye: [board-check] 结果：15 项，失败 0 项
```

`CONFIG_ONEYE_FW_SELFTEST_STRICT=y`（缺省）时，任一项 FAIL 即**中止上云**并保持自检结论可见——不静默继续。

## 5.2 AEC 采集（录音 → web 播放）

`CONFIG_ONEYE_FW_ENABLE_AEC_CAPTURE=y`（缺省开）时固件内建 AEC 采集链：

```
I2S0(CODEC_ADC_I2S_PORT) 16 kHz / 32 bit / RIGHT_LEFT     ← ES7210 四通道按 "RMNM" 排布（2 麦 + AEC 回采）
  → algorithm_stream（esp-sr AFE，**AFE_TYPE_VC**：AEC + 降噪；不含唤醒词/命令词模型）
  → wav_encoder（16 kHz / 16 bit / 单声道）
  → fatfs_stream 落盘
```

- **落盘位置**：SD 卡可用 → `/sdcard/rec/aec-N.wav`；否则 SPIFFS 兜底 → `/spiffs/rec/aec-N.wav`。
- **触发方式**：`POST /api/action {"op":"aec_start","duration_s":5}`（到时自动停止）或 `{"op":"aec_stop"}`；
  验证面板上有「录 5/10/30 s」「停止录音」按钮，并在「音频」卡片里直接播放/下载 WAV（`Range` 已支持拖动）。
- **分区口径（自定义 `partitions.csv`）**：`factory 2M` + **`model 2M`** + `storage(spiffs) 1M`。

> **⚠️ 关键坑位（已修复，勿踩）——esp-sr 的模型分区**：`components/esp-sr/CMakeLists.txt:78-100` 只在
> **`CONFIG_PARTITION_TABLE_CUSTOM=y` 且分区表中存在名为 `model` 的分区**时，才生成 `srmodels/srmodels.bin`
> 并把模型烧到该分区；否则那句提示只是赋给一个未被使用的 CMake 变量（**上游不打印**）⇒ **构建成功、模型从未投放**，
> 真机 AFE 会因找不到模型而失败。**上游 `examples/advanced_examples/{aec,algorithm}` 的 `partitions.csv` 即缺 `model` 分区**。
> 本工程已修正：`partitions.csv` 含 `model`（2 M），构建产出 `srmodels.bin` **337,952 B**（仅 NSNET2 + WebRTC VAD；
> 不含 WakeNet/MultiNet ⇒ 与 `AFE_TYPE_VC` 口径一致，且省下约 3.8 MB flash）。
- **前置条件**：AFE 需要 PSRAM（`CONFIG_SPIRAM*`，本板 ESP32-S3-WROOM-1 带 Octal PSRAM）。
- **⚠️ 待真机核对**：① 模组 PSRAM 为 Octal 还是 Quad（若 Quad，把 `CONFIG_SPIRAM_MODE_OCT` 改为 `MODE_QUAD`）；
  ② 采集链路真实出声与降噪效果（模型已投放是**构建级证据**，运行期仍需实测）；③ SD 卡（一线模式）与 SPIFFS 兜底两条落盘路径。

## 5.3 板上回放（选 SD 卡文件 → 开发板扬声器出声）

`main/player.{c,h}`：`/sdcard/<file>` → `fatfs_stream`(reader) → 解码器（按扩展名）→ `i2s_stream`(writer) → ES8311 → NS4150 → 扬声器。

| 口径 | 值 |
| --- | --- |
| 来源 | **仅 SD 卡（FATFS）**：`/sdcard/...`。SPIFFS 里的 AEC 录音**不上板播放**（ADF 的 `fatfs_stream` 读不了 SPIFFS），只在网页播放/下载 |
| 格式 | **WAV / MP3**（其他扩展名返回 `unsupported codec`，不静默失败） |
| 采样率/声道 | 以解码器上报的 `MUSIC_INFO` 为准，**动态重设 I2S 时钟**（否则 16 kHz 的 AEC 录音会按 48 kHz 播快） |
| 控制 | `POST /api/action {"op":"play","path":"/sdcard/music/x.wav"}`、`{"op":"stop"}`、`{"op":"set_volume","volume":0-100}` |
| 状态 | `GET /api/status` 的 `player{playing,path,codec,rate_hz,channels,elapsed_ms,volume,msg}`；播完自动回收管线 |
| 并发 | 管线与事件接口**由播放任务独占销毁**：外部 `stop` 只置标志并等待任务回收（≤3 s），避免 use-after-free |
| 边界 | **板上视频回放（AVI/MJPEG → LCD）本轮不做**（需 LCD + esp_muxer + 视频解码链路）；SD 上的图片/视频可在**网页**预览 |

验证面板的「音频」卡片里，SD 卡音频文件会多出一个 **「板上播放」** 按钮（`/media/list` 已带 `playable_on_board` 与 `device_path`），
另有「停止播放」与音量滑块；状态行显示正在播放的文件、编码、采样率与已播时长。

## 5.4 Wi-Fi 配网（SD 卡凭据文件 → 开机自动配网）

**用户操作三步**：把凭据文件放 SD 卡 → 插卡 → 复位。固件启动时按 `SD 凭据文件 → Kconfig` 顺序取凭据，
命中即连接；连接成功才启动上云与本地验证面板（面板需设备 IP 才可达）。

| 项 | 说明 |
| --- | --- |
| 文件路径 | `/sdcard/oneye-wifi.txt`（SD 卡**根目录**；模板见 [`sd-root/oneye-wifi.txt`](sd-root/oneye-wifi.txt)）；SD 未挂载时自动试 `/spiffs/oneye-wifi.txt` |
| 文件格式 | `key=value` 逐行（也接受 `key: value`）；键 `ssid`（或 `wifi_ssid`）、`password`（或 `pass`/`psk`/`wifi_password`）；`#`/`;` 开头为注释；两侧空白与行尾 CR 自动去除 |
| 查找顺序 | `/sdcard/oneye-wifi.txt` → `/spiffs/oneye-wifi.txt` → `CONFIG_ONEYE_FW_WIFI_SSID/_PASSWORD` → 都没有则**不联网**（板级自检/按键/录音/板上回放照常） |
| 串口取证 | `[wifi_prov] 从凭据文件读取 Wi-Fi：/sdcard/oneye-wifi.txt（ssid=…，密码已提供）` → `Wi-Fi 已获取 IP：192.168.x.x（ssid=…，来源=file:/sdcard/oneye-wifi.txt）` → `已联网 → 启动上云（oneye-dev-sdk）` |
| 面板可见 | `GET /api/status` → `wifi{connected,ip,ssid,source}`；面板「Wi-Fi 配网」卡片显示**凭据来源** |
| 运行期改配 | `POST /api/action {"op":"wifi_set","ssid":"…","password":"…"}`（面板表单同款）：只改**本次运行**、**不写文件**，用于换网/排障；持久化仍以 SD 文件（或 Kconfig）为准 |
| 开关 | `CONFIG_ONEYE_FW_ENABLE_WIFI_FILE`（缺省 y）同时控制凭据文件读取与 `wifi_set`；量产**必须置 n** |
| 安全口径 | 凭据文件在 SD 卡上是**明文** Wi-Fi 密码 ⇒ **仅台面/研发配网**。量产不得依赖该路径：走 claim + 一机一密凭据（ADR-0007） |

连接超时为 20 s（`wifi_prov_connect(…, 0)` 取缺省）；失败只降级、不重启、不阻塞板级自检，
串口给出 `Wi-Fi 连接失败（来源 …，ssid=…）→ 跳过上云` 与改配建议。

## 5.5 本地验证面（HTTP JSON API）与验证面板

固件内置一个**本地验证面**（`main/panel_api.c`），供宿主**验证面板**（[`tools/panel/`](../../../tools/panel/README.md)）
与其他联调工具读取设备状态。**它不是云端设备面契约**（不进 `backend/contracts`、不新增 topic/影子键），
量产应置 `CONFIG_ONEYE_FW_ENABLE_PANEL_API=n`。

| 路由 | 内容 |
| --- | --- |
| `GET /api/ping` | 存活探针 |
| `GET /api/status` | 固件/板卡、运行时长、Wi-Fi IP、云端链路与收发帧、自检汇总、按键计数 |
| `GET /api/selftest` | 板级核对**逐行** `item/expect/actual/pass`（与串口 `[board-check]` 同源） |
| `GET /api/keys?limit=N` | 6 键标签、`current`（最近一次）、`counters`、`history[]`（新→旧，含上行状态与 ack 时间） |
| `GET /media/list` | 可播放文件清单（SD 与 SPIFFS 兜底存储；按扩展名判定 `audio`/`image`/`video`），并带 `aec_enabled`/`aec_recording`/`rec_root` |
| `GET /media/<alias>/<path>` | 文件下载/流式播放，**支持 `Range`**（206 + `Content-Range`，浏览器可拖动进度）；`alias` = `sdcard` \| `spiffs`；含目录穿越防护 |
| `POST /api/action` | `{"op":"aec_start","duration_s":N}` / `{"op":"aec_stop"}` / `{"op":"play","path":"/sdcard/..."}` / `{"op":"stop"}` / `{"op":"set_volume","volume":0-100}`（**写操作，仅台面验证**） |

**按键"历史响应"三态**：本地检测（`pending`）→ 交 SDK 上报（`sent`，失败 `failed`）→ 收到云端 `event/down` ack
（`acked`，按 `data.ref == 事件 id` 关联，固件生成的 id 形如 `key-00001`）。

宿主面板（`tools/panel/`，Python 标准库零依赖）把三条通道聚合到一页：设备 HTTP（主）、MQTT `rmng/dev/+/event/up`（云端独立验证）、
串口（无网兜底）：

```bash
# 设备侧（先烧录并配好 Wi-Fi；IP 见串口日志）
idf.py -p <COMx> flash monitor

# 宿主侧（本仓根目录）
python3 tools/panel/panel.py --device korvo2-0001=http://<设备IP>
#   → 浏览器打开 http://127.0.0.1:8787/ ：按键实时状态 + 历史响应（含云端确认与端到端时延）+ 板级核对表 + 链路 + 串口
# 无硬件时验证面板自身链路：
python3 tools/panel/panel.py --self-test
```

## 6. 配置（`idf.py menuconfig` → `korvo2_oneye 板级固件配置`）

| 配置 | 缺省 | 说明 |
| --- | --- | --- |
| `ONEYE_FW_DEVICE_ID` | `korvo2-0001` | 兼作 MQTT username/client_id（EMQX ACL `%u` 依赖） |
| `ONEYE_FW_CLOUD_HOST` / `_PORT` / `_WS_PATH` | 192.168.1.100 / 0 / 空 | 端点显式配置（端口 0 = 按承载取契约缺省 1883/8883/8083/8084） |
| `ONEYE_FW_TRANSPORT` | TCP | 承载选择（契约四承载） |
| `ONEYE_FW_CLOUD_TOKEN` | 空 | 设备令牌（空 = 匿名 dev 形态） |
| `ONEYE_FW_TLS_INSECURE` | **n** | 仅 dev/产测可开；生产须投放自研 CA（`tls_ca_pem`） |
| `ONEYE_FW_ENABLE_WIFI_FILE` | **y** | 凭据文件配网（SD/SPIFFS `oneye-wifi.txt`）+ `/api/action wifi_set`；**量产置 n** |
| `ONEYE_FW_WIFI_SSID` / `_PASSWORD` | 空 | 兜底凭据；空且无凭据文件 = 只跑板级自检与按键，不上云 |
| `ONEYE_FW_ENABLE_KEYS` | y | 6 键 → `event/up` |
| `ONEYE_FW_ENABLE_SDCARD` / `_LCD` | **y** / n | SD 缺省开（凭据文件、录音、板上回放都依赖它；未插卡只告警不阻塞）；LCD 仍未确认，保持 n |
| `ONEYE_FW_ENABLE_MPP` | n | 只初始化 mpp 域，不发起会话 |
| `ONEYE_FW_ENABLE_PANEL_API` | y | 设备侧本地验证 HTTP API（**量产置 n**）；`_PANEL_PORT` 缺省 80、`_KEY_HISTORY_MAX` 缺省 64 |
| `ONEYE_FW_DECLARE_VIDEO_LIVE` | **n** | 真机取帧取证通过后才可开；`audio.intercom` 无开关（WebRTC 数据面未落地前一律不得声明） |
| `ONEYE_FW_SELFTEST_STRICT` | y | 自检失败中止上云 |

## 7. 契约对接（设备面）

| 面 | 本固件行为 |
| --- | --- |
| `caps/up` | 上线声明能力集（缺省 `NONE`；`model_version="1"`、`fw_version`） |
| `status/up` | `enable_status_topic=true`（retained + LWT） |
| `shadow/up` | 只写**已登记键**：`esp.fw_version`、`esp.power`（不新增键） |
| `log/up` | 板级自检结论、启动信息（`tag=oneye_base`） |
| `event/up` | 按键事件（`type=device_event`，`data{key,action}`） |
| `command/down` | 收到即打印（命令执行面属 S16，未落地；不做假回执） |
| `mpp/*` | 缺省不启用（媒体数据面未真机验证） |

## 8. 本轮实测记录

**环境**：WSL2（Ubuntu-24.04）+ ESP-IDF **v5.5.5**（`source ~/esp/esp-idf-5.5.5/export.sh`）+ `CCACHE_ENABLE=0`；构建目录 `output/.build/`、产物 `output/`（均 gitignored）。

| 项 | 结果 |
| --- | --- |
| 固件轨（推荐入口） | `./build-all.sh --toolchains esp32s3@5.5.5 --firmware` → **rc=0**；产物 `output/firmware/xtensa-esp32s3-elf-gcc-14.2.0/korvo2_oneye/` |
| 固件产物 | `korvo2_oneye.bin` **301,152 B**（SHA256 `516738fb65fd8cd11409574940ba05ca1cd5baa7371c95b88697688e653dc38a`）；`korvo2_oneye.elf` 5,312,804 B；`bootloader.bin` 20,832 B；`partition-table.bin` 3,072 B |
| 固件产物（**含本地验证 API 后**，2026-09-15 第二轮） | `korvo2_oneye.bin` **302,496 B**（`0x49da0`；分区余量 71%）；新增 `main/panel_api.c`（`esp_http_server`）+ 按键历史/ack 关联 ⇒ 相对上一版 +1,344 B |
| 固件产物（**含 AEC 采集 + 媒体 API 后**，2026-09-15 第三轮） | 自定义分区表：`factory 2M` + `model 2M` + `storage(spiffs) 1M`；`korvo2_oneye.bin` **390,128 B**（`0x5f3f0`，分区余量 81%）；`srmodels/srmodels.bin` **337,952 B**（烧写偏移 `0x210000`）；`flash_args` 已含 `0x210000 srmodels/srmodels.bin` |
| 固件产物（**含板上回放后**，2026-09-15 第四轮） | `korvo2_oneye.bin` **390,656 B**（`0x5f600`，分区余量 81%）；新增 `main/player.c`（fatfs → wav/mp3 解码 → i2s，动态重设时钟）+ `/api/action` 的 play/stop/set_volume + `/api/status.player{}` |
| ⚠️ **上表体积口径更正（第十一轮）** | 上面四行（301,152 / 302,496 / 390,128 / 390,656 B）**都不含 Wi-Fi**：用 `xtensa-esp32s3-elf-nm` 核对当时产物，`esp_wifi_init`/`esp_netif_init`/`esp_event_loop_create_default` **均为 0 个已定义符号**（那几轮 `app_main` 未引用 Wi-Fi 代码路径，链接器没拉 `libesp_wifi`/`libwpa_supplicant`/`libnet80211`/`libesp_netif`）。**镜像体积一律以本行以下的完整固件为准**，旧数字仅作历史记录 |
| 固件产物（**含 SD 卡凭据文件配网后**，2026-09-15 第十一轮） | 删除工程内 `sdkconfig` 后经 `./build-all.sh --toolchains esp32s3@5.5.5 --firmware` 重建（证明 `sdkconfig.defaults*` 可独立决定构建结果）：`korvo2_oneye.bin` **1,930,864 B**（`0x1d7670`；`factory 2M` 余量 **8%**）、`.elf` 16,185,716 B、`bootloader.bin` 20,832 B、`partition-table.bin` 3,072 B；新增 `main/wifi_prov.{c,h}` + `sd-root/oneye-wifi.txt` 模板；**Wi-Fi 首次真正进镜像**（+`libwpa_supplicant`/`libnet80211`/`libesp_netif` 等，约 +1.5 MB @ `-Og`）；`flash_args` 仍含 `0x210000 srmodels/srmodels.bin`；余量偏紧 ⇒ 后续增长时首选打开 `CONFIG_COMPILER_OPTIMIZATION_SIZE=y`（-Os） |
| AEC 编译期硬伤拦截 | 本轮构建先后拦下 4 类问题并修复：`/api/*` 注释内嵌 `/*`（`-Werror=comment`）、`HTTPD_416_*` 在本 IDF 版本不存在（改手工置状态码）、`I2S_CHANNEL_FMT_*` 需显式 `#include "driver/i2s.h"`、`snprintf` 路径拼接可能截断（`-Werror=format-truncation`，改用 512 B 缓冲 + 显式长度校验）；**并查出 esp-sr 缺 `model` 分区导致模型从不投放的上游坑位**（见 §5.2） |
| 板卡选择核对 | `board-config.txt` = `CONFIG_IDF_TARGET_ESP32S3=y` + `CONFIG_ESP32_S3_KORVO2_V3_BOARD=y`（由 `--firmware` 轨自动核对） |
| SDK 链接形态 | 构建日志：`链接预编译库（4 分域库，toolchain=xtensa-esp32s3-elf-gcc-14.2.0）` |
| 编译期断言 | `board_expect.h` 40+ 条 `_Static_assert` 全部通过（**失败即构建失败**，本轮已用它拦住 4 处真实问题，见总控集成说明 §10.3） |
| 运行期自检 | **未跑真机**（本轮只出构建产物）——烧录后应看到 `[board-check] … PASS` 逐行输出与 `结果：N 项，失败 0 项` |
| 本地验证面（设备 API） | 编译通过（IDF 5.5.5 / esp32s3）：`CONFIG_ONEYE_FW_ENABLE_PANEL_API=y`、端口 80、历史 64 条；`/api/status.wifi{}` 增 `ssid`/`source`，`/api/action` 增 `wifi_set` |
| 宿主验证面板 | `python3 tools/panel/panel.py --self-test`（内置 mock 设备 + mock broker + 凭据文件配网模拟）**33 项全通过、rc=0**：设备/自检/按键历史/云端确认关联/端到端时延/AEC 采集/板上回放/**Wi-Fi 配网四态与来源核对**等；详见 [tools/panel/README.md](../../../tools/panel/README.md) §7 |
| 门禁 | 宿主轨（4×`.a`+4×`.so`+demo+单测 17 组/215 用例/2890 断言）与 backend `contract_check` / `md_link_check`、总控 `md_link_check` 全绿（契约面零改动） |
| **真机识别（2026-09-15 第十二轮）** | `esptool flasher_id`：`ESP32-S3 (QFN56) rev v0.2`，**Embedded PSRAM 8MB**、**flash 实测 16 MB** ⇒ 模组实为 **ESP32-S3-WROOM-1-N16R8**（`CONFIG_SPIRAM_MODE_OCT=y` 假设**成立**：启动日志 `octal_psram: vendor id 0x0d (AP) … Found 8MB PSRAM device / Speed 80MHz`）；MAC `b8:1f:3f:c4:07:24` |
| **真机烧录（COM12）** | 全片擦除 + 按偏移写 4 区域（`0x0/0x8000/0x10000/0x210000`）→ 全部 `Hash of data verified`；启动日志分区表 = `nvs / phy_init / factory 2M / model 2M / storage 1M`（与 `partitions.csv` 一致） |
| **板级自检（真机，STRICT 口径）** | **19 项，失败 0 项**：I²C 17/18、I2S0 五脚 16/9/45/8/10、PA 48、耳机/卡检测 -1、SD 参数、绿/蓝灯、ES8311 MCLK=0、`RMNM`、codec 初始化、6 键初始化、SD 挂载 —— 首轮曾 16 项失败 1（蓝灯见下），修正后全绿 |
| **蓝灯断言口径修正** | 首轮真机 `blue_led_gpio expect=128 actual=-128 FAIL`：ADF `get_blue_led_gpio()` 返回 **`int8_t`**，而 TCA9554 P7 = `BIT(7)` = 0x80 ⇒ 经 int8 回传为 **-128**（板子无问题，是取值类型口径）⇒ 期望值按同口径折算（`(int)(int8_t)(1<<7)`） |
| **6 键任务（真机）** | 打 `idf_v5.5.5_freertos.patch` 后：`AUDIO_THREAD: The button_task task allocate stack on external memory` → `ADC_BTN: Calibration scheme version is Curve Fitting / Calibration Success` → `按键服务已启动（6 键 → event/up）`（**补丁前**为 `Not found right xTaskCreateRestrictedPinnedToCore` → `Create button_task failed!`） |
| **SD 卡（真机）** | `sdcard mount (1-line) mounted PASS`（`CID name SD!`）；SPIFFS 兜底 `total=934 KB` |
| **AEC / 回放（真机启动）** | `AEC 采集已就绪（16000 Hz / 32 bit / RMNM → AFE → WAV 16000 Hz 16 bit 单声道）`、`板上回放就绪`、ES7210 `Enable ES7210_INPUT_MIC1/2/3` + `Enable TDM mode`、ES8311 `in Slave mode` |
| **凭据文件配网（真机首启）** | SD 已挂载但根目录无凭据文件 ⇒ `wifi_prov: 未找到 Wi-Fi 凭据文件（试过 /sdcard/oneye-wifi.txt 与 /spiffs/oneye-wifi.txt）` → 按设计**降级不中止**（自检/按键/录音/回放继续），并在串口给出投放指引 |
| **凭据文件配网（投放后，真机）** | SD 卡根目录放 `oneye-wifi.txt`（`ssid=…`/`password=…`）复位即生效：`从凭据文件读取 Wi-Fi：/sdcard/oneye-wifi.txt（ssid=***，密码已提供）` → `Wi-Fi 已获取 IP：192.168.110.79（来源=file:/sdcard/oneye-wifi.txt）` → `本地验证面已启动` → `已联网 → 启动上云`；`/api/status.wifi.source` 亦为 `file:/sdcard/oneye-wifi.txt`（**来源可核对**） |
| **局域网访问本地验证面（真机）** | `GET http://192.168.110.79/api/ping` → `{"ok":"true","api_version":"1","role":"local-verification-only"}`；`/api/status` → `selftest{total:19,failed:0}`、`media.sd_mounted=true`、`wifi.connected=true` |
| **AEC 录音（真机）** | `POST /api/action {"op":"aec_start","duration_s":5}` → `/sdcard/rec/aec-00001.wav` **127,020 B**（WAV 头 44 B + 126,976 B 音频 = 3.97 s @16 kHz/16 bit 单声道）；`/media/list` 列出且 `playable_on_board=true`。**口径**：请求 5 s 实得 ~4 s —— 停止定时器在 `aec_capture_start()` 返回时即开始计时，而 AFE 初始化/预热约 1 s 内不出数据；需要精确时长时应按「首次出帧后计时」改造（登记为后续项） |
| **网页播放（真机，Range）** | `GET /media/sdcard/rec/aec-00001.wav` + `Range: bytes=0-43` → **206 Partial Content**、`Content-Type: audio/wav`、`Accept-Ranges: bytes`、`Content-Range: bytes 0-43/127020`、前 4 字节 `RIFF`、数据长度字段 126,976 ⇒ 浏览器 `<audio>` 可拖动播放 |
| **板上回放与停止（真机）** | `POST /api/action {"op":"play","path":"/sdcard/rec/aec-00001.wav"}` → `playing=true`、`codec=wav rate_hz=16000 channels=1`、`elapsed_ms` 递增（扬声器出声）；`{"op":"stop"}` → **917 ms** 返回 `stopped`、`playing=false`（重复 stop 幂等 349 ms）；`set_volume 60` 生效 |
| ⚠️ **/media 通配路由从未命中（真机修复）** | 首轮真机 `GET /media/sdcard/rec/aec-00001.wav` 返回 `404 Nothing matches the given URI`（`/media/list` 正常）：IDF 的 `HTTPD_DEFAULT_CONFIG()` 中 **`uri_match_fn = NULL`**，而 `httpd_find_uri_handler()` 在 NULL 时退化为 `httpd_uri_match_simple`（精确串比较）⇒ 以星号结尾的通配路由永不命中。修法：`panel_api_start()` 显式 `cfg.uri_match_fn = httpd_uri_match_wildcard`。**主机 mock 用 Python 自写前缀匹配 ⇒ 该缺陷只能在真机暴露** |
| ⚠️ **停止播放必崩（真机修复）** | 首轮真机 `stop` 使设备**重启**（`uptime_ms` 回退）：`assert failed: spinlock_acquire (lock->count == 0)` @ `xEventGroupSetBits` ← `audio_element_stop` ← `audio_element_deinit` ← `teardown_no_task (player.c:85)`。根因：**双重释放** —— ADF 的 `audio_pipeline_deinit()` 已逐个 deinit 并注销注册元素（`audio_pipeline.c:263-271`），我方随后又对元素逐个 `audio_element_deinit` ⇒ use-after-free。修法：管线销毁后仅置空元素句柄 |
| **上云链路（真机，未闭环）** | SDK 已启动（`base initialized: node=korvo2-0001 transport=mqtt-tcp`、`module registered: log/event`），但按 Kconfig 占位端点 `192.168.1.100:1883` 反复 `connect ... failed` ⇒ **按键三态中的「云端 ack」需要真 broker 才能验证**（登记为后续项：把 `ONEYE_FW_CLOUD_HOST` 指向 dev-stack EMQX 后复测 `caps/up`/`status/up`/`shadow/up`/`log/up`/`event/up` 与 `event/down` ack 关联） |
| **上云链路（真机，第十三轮已闭环）** | 端点指向临时服务器 `175.178.190.187:1883` 后：`link up: 175.178.190.187:1883 transport=mqtt-tcp node=korvo2-0001`；真 EMQX 抓包收到 `status/up`（含 LWT 遗言 `{"online":false,"reason":"lwt"}`）、`caps/up`（`data{node_id,model_version,fw_version,sdk_version,caps,attrs,events,cmds}`）、`shadow/up`（`esp.fw_version`/`esp.power`）、`log/up`（`items[0]={level,tag:"oneye_base",msg:"板级自检：19 项 / 失败 0 项"}`）、`event/up`（按键事件，`data.items[].id=key-0000N`） |
| **按键三态（真机，第十三轮已闭环）** | 本地检测（串口 `[key] <名>/<动作>` + `panel_api: key event #N … id=key-0000N`）→ 上行（真 EMQX `event/up`）→ 云端 ack（面板云端桩 `event/down` `{"type":"ack","data":{"ref":"key-0000N","code":"ok"}}`，`ref` 与上行 id 对齐） |
| ⚠️ **真机崩溃链：ack 必崩（已修）** | 设备收到 `event/down` 即重启：`***ERROR*** A stack overflow in task oneye_net has been detected` + `Backtrace ... CORRUPTED`。根因 = **SDK 把 IDF `xTaskCreatePinnedToCore` 的栈单位当"字"**（IDF 是**字节**）⇒ 网络线程只有 4 KB 栈，回调一跑就溢出。修法：按字节传入 + 缺省 8 KB（SDK `3029bba`） |
| ⚠️ **真机录音 0/44 字节（已修）** | 串口：`sdmmc_cmd: allocate_dma_buf: not enough mem` + `AUDIO_THREAD: Error creating RestrictedPinnedToCore algo_fetch` + `afe_feed: handle or input data is NULL!` ⇒ 内部 RAM 被 Wi-Fi+SDK+AFE 吃满。修法：`SPIRAM_MALLOC_ALWAYSINTERNAL=4096` + 内部保留量；实测 `internal_free 39 KB→88 KB`、`largest 15 KB→40 KB`，录音恢复正常 |
| **录音时长精度（已修）** | ① 原「起管线即计时」把 AFE 预热算入（请求 5 s 得 ~4 s）；② 改「按文件大小探首帧」受 FATFS 写缓冲滞后（请求 5 s 得 ~8.4 s）；③ 现按 **fatfs_stream 的 `byte_pos`（已写字节数）** 精确计时：**请求 5 s → 163,884 B = 5.12 s** |
| **落盘健壮性（新增）** | 起管线前 512 B **写盘预检**，SD 不可写即回落 SPIFFS；采集 0 字节时显式报错（不再静默产出空文件） |
| **面板可观测性（新增）** | `/api/status.panel.heap{internal_free,internal_largest,dma_free,dma_largest,psram_free}`（本轮三类问题均靠它定位） |
| **待续（未闭环，明确记录）** | 录到的音频**幅度极低**（RMS 137/32768 ≈ 0.42%、峰值 1035 ≈ 3.2%）⇒ 麦克风通路近乎静音，故"板上回放没声音"；需单独一轮音频链路排查（建议：绕开 AFE 直接录 ES7210 原始 4 通道比对、核对 `"RMNM"` 通道序与 TDM 槽位、`PERI_PWR_ON`(TCA9554 P5) 供电口径）。LCD/摄像头取帧、时间同步同样未闭环 |

**后续（真机）**：`idf.py -p <COM> flash monitor` → 核对自检逐行 PASS → **SD 卡放 `oneye-wifi.txt` 复位自动配网** → 观察
`caps/up` / `status/up`（retained + LWT）/ `shadow/up` / `log/up` 与按键 `event/up`；其间可用面板「Wi-Fi 配网」卡片核对**凭据来源**；
摄像头取帧另行取证（决定 `video.live` 是否可声明）。

## 9. 边界

- 本工程**不实现**：媒体数据面（webrtc/http_upload/mqtt_frame）、PTZ/命令执行面、AI 端侧初筛、LED 显示服务、
  触摸、电池采集、camera 取帧（依赖外部 `esp32-camera`）；
- 上游资产纪律：`esp-adf` 上游分支（`master` / `release/*`）与 `esp-repo/` 镜像**只读**，我方改动只进集成分支；
- 真机烧录/取证（录音、取帧、LCD、按键、SD 插拔）登记为后续卡，不在本工程内声称通过。
