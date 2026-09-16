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
5. ~~**摄像头模组是否装配 + 真机取帧**~~ → **已确认（2026-09-16，第十八轮）**：模组已装配且真机取帧可用
   —— `Camera PID=0x3660 / Detected OV3660 camera / Detected camera at address=0x3c`，抓帧 320×240 落 SD 并经
   `/media` 在网页显示（实现与实测见 §5.8）。**注意**：该确认只覆盖"采集（取帧）"，
   `video.live` 还要求编码 + 上行数据面，本工程未实现（见 §7）。
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
I2S0(CODEC_ADC_I2S_PORT) 16 kHz / 32 bit / ONLY_LEFT        ← 单麦口径：input_format "RM"（麦 + AEC 回采）
  → algorithm_stream（esp-sr AFE，**TYPE1 + AFE_TYPE_VC**：AEC + 降噪；不含唤醒词/命令词模型）
  → wav_encoder（16 kHz / 16 bit / 单声道）
  → fatfs_stream 落盘
```

> **⚠️ 通道配方（第十四轮真机修正，勿再混用）**：本板 `board_def.h:116` 定义 `RECORD_HARDWARE_AEC (true)`，
> ADF 例程 `advanced_examples/algorithm` 因此走**单麦分支**：`input_format "RM"`（2 通道）+ `I2S_CHANNEL_FMT_ONLY_LEFT`
> + `ALGORITHM_STREAM_CFG_DEFAULT()`（TYPE1 + `AFE_TYPE_VC`）。其**双麦分支**才是 `"RMNM"`（4 通道）
> + `I2S_CHANNEL_FMT_RIGHT_LEFT` + `AFE_TYPE_SR`。把双麦的 I2S 口径喂给单麦 AFE，AFE 只取首通道并告警
> （`For single microphone channel, SE is deactivated.`），录出来的**幅度近乎静音**（实测 RMS 137/32768）。
> 修后串口可见 AFE 自述 `Input PCM Config: total 2 channels(1 microphone, 1 playback)`。
> 另：录音前须 `audio_hal_ctrl_codec(s_board->adc_hal /* ES7210，不是 audio_hal */, ENCODE, START)`
> 并按例程顺序（先 START 后设增益）重设 MIC 增益；`es7210_adc_ctrl_state()` 的返回值是寄存器读值、**非错误码**。

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
- **真机状态（第十四轮）**：① PSRAM 已确认 Octal 8 MB @80 MHz；② 录音链路已闭环（请求 5 s → 163,884 B = 5.12 s，
  原始 I2S 幅度 RMS 123–390 / 峰值 600–1600，安静房间量级）；③ SD 落盘与 SPIFFS 兜底两条路径均已实测；
  ④ 「对麦说话」的灵敏度取证与扬声器可听性待补（见 §8 末行「待续」）。

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
| 与录音互斥 | 本板 ES7210(ADC)/ES8311(DAC) **共用 I2S0**：录音中 `play` 返回 `ESP_ERR_INVALID_STATE`（`busy: recording (shared I2S0)`），回放中 `aec_start` 同样被拒（**串行独占端口**，实测重叠会把两条管线都卡死） |
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
| `POST /api/simulate/key?key=<volup\|voldown\|set\|play\|mode\|rec>[&action=click\|click_release\|press\|press_release]` | **按键事件注入**（第十五轮新增）：按与物理按键**完全相同**的上报路径发一次 `event/up`（同一函数、同一 id 生成、同一契约负载、同一 SDK 投递）。⚠️ 触发源是 **HTTP 而非 ADC 按键** ⇒ 用于远程/自动化验证与演示三态链路，串口日志带 `(注入/local-verification-only)` 标记；**不能**替代"物理按键可用"的验收证据 |
| `POST /api/lcd/draw?pattern=<bars\|grid\|checker\|test\|status>` | **板载 LCD 图案下发**（第十七轮新增）：设备真重绘（`panel.lcd.draws` 递增），人眼/拍照对账（§5.7） |
| `POST /api/camera/capture` | **板载摄像头抓帧**（第十八轮新增）：抓一帧 → `/sdcard/cam/cap-%05u.jpg`，返回 `path/bytes/w/h/ms/url/err`；`url` 可直接经 `/media` 显示（§5.8） |
| `POST /api/camera/reinit?fmt=&fb=&fbc=&grab=&xclk=&psram=&q=` | **摄像头取帧配置旋钮**（第十八轮新增，仅台面排障）：在同一块板子上对比定位取帧失败，回显生效配置（§5.8） |
| `POST /api/camera/snapshot` | **抓一帧不落盘**（第十九轮新增）：直接回 `image/jpeg` 字节 —— 面板「预览」的**回落通道**（~1 帧/s 轮询即得近实时画面，且不给 SD 卡写放大） |
| `GET http://<设备>:81/stream` | **MJPEG 预览流**（第十九轮新增，**独立 httpd 实例**）：`multipart/x-mixed-replace`，浏览器 `<img>` 直接显示；主验证面（:80）轮询不受影响（§5.8） |

**按键"历史响应"三态**：本地检测（`pending`）→ 交 SDK 上报（`sent`，失败 `failed`）→ 收到云端 `event/down` ack
（`acked`）。

> ⚠️ **关联口径（第十五轮修正）**：契约 §4.3 的 ack `data.ref` 指向**上行信封 id**，而信封 id 由 SDK 生成
> （uuid，`oneye_envelope_build(…, id=NULL, …)`），**不等于**固件为条目分配的 `key-%05u` ⇒ 早前按"在 ack
> 负载里找本地 id 子串"的关联**永远匹配不上**（真机症状：`event/down` ack 确实到了、`rx_frames` 增长、
> 串口 `EVENT_ACK` 有打印，但面板"云端确认"恒为 `sent`）。现按**上报 FIFO** 关联：`oneye_dev_event_report()`
> 内部强制成帧（SDK `oneye_dev_event.c:1099`）⇒ 一次上报 = 一帧 = 一 ack，弹出最旧一条即精确对应；
> 台面手动 ack（`POST /api/mqtt/ack`，ref 恰为本地 id）仍走子串匹配兜底。

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

## 5.6 时间同步（云端授时）

契约（[传输规范 §7](../../../../backend/contracts/api/mqtt/传输规范.md)）规定：所有 `ts` 为 **UTC 毫秒**，**首选授时来源 = `caps/down.cloud_ts`**（回退 HTTPS `Date`，SNTP 为部署可选且设备默认关闭）。固件实现：

1. **索取**：链路建立（`CLOUD_LINK_UP`，含重连）后由独立任务调用 `oneye_dev_base_sync_time()`（`caps/up{data.req:"time_sync"}`）。
   ⚠️ **不得**在 SDK 回调内调用（它阻塞等待应答，会在网络线程上自锁）；失败重试 6 次、每次间隔 5 s，之后等服务端在 `status/up` 时的周期性再同步（每设备 ≥5 min）。
2. **应用**：收到 `TIME_SYNCED` 即建立「采样点 + 单调时钟」偏移；此后**按键历史、ack 时间戳与所有上行 `ts` 统一走同一时钟**（`fw_wall_ms()`），未授时期间为设备运行时刻并带告警位（契约 §7）。
3. **可观测**：`GET /api/status` → `panel.time{synced, cloud_ts_ms, offset_ms, source, now_ms}`；面板「设备与链路」卡片显示**授时**状态，历史「设备时刻」列在已授时后显示真实本地时间（未授时显示 `运行 +N s`）。

`now_ms` 可直接与宿主时钟对照（例如宿主 `Date.now()`），差值即设备时钟相对宿主的偏差。

> ⚠️ **台面构建陷阱（实测踩到）**：`./build-all.sh --firmware` 会**按 `sdkconfig.defaults` 重新生成 `sdkconfig`**，从而丢掉台面值
> （`ONEYE_FW_CLOUD_HOST`/`_PORT`、`SPIRAM_MALLOC_RESERVE_INTERNAL`）——症状是设备去连占位地址 `192.168.1.100`。
> 台面配置备忘见 `output/.build/bench-sdkconfig.txt`：临时服务器 `175.178.190.187:1883`、`SPIRAM_MALLOC_RESERVE_INTERNAL=131072`。
> **另**：`src/internal/*.c` 等 SDK 内部源码**不参与固件编译**（固件链接 SDK 的**预编译库**）⇒ 改 SDK 内部实现后必须重跑
> `./build-all.sh --toolchains esp32s3@5.5.5`（否则"改了没生效"）。

## 5.7 板载 LCD（ILI9341 320×240 状态屏）

板载 LCD 走 ADF 板级 API（`audio_board_lcd_init()` → `esp_lcd_panel_draw_bitmap()`，与 `examples/display/lcd_jpeg|lcd_camera` 同源），我方只加**帧缓冲与图案**（`main/lcd_ui.c`）：

1. **帧缓冲在 PSRAM**：320×240 RGB565 = **153,600 B**，分 **40 行/带** 刷新（`esp_lcd_panel_draw_bitmap` 逐带写），避免一次性大传输挤占内部 DMA；
2. **图案**：`bars` / `grid` / `checker` / `test` / `status`（`status` = 实时状态屏：链路、Wi-Fi、授时、最近按键）；
3. **可观测**：`GET /api/status` → `panel.lcd{ready,w,h,pattern,draws,last_ms,fb_bytes,fb_mem}`；`POST /api/lcd/draw?pattern=…` 下发图案，面板「板载 LCD」卡片可切换（**人眼/拍照即可对账**）。

真机实测：`lcd.ready=true`、`w=320 h=240`、`fb_bytes=153600`、`fb_mem=psram`、图案切换后 `draws` 递增、按键注入即触发状态屏重绘。

## 5.8 板载摄像头（OV3660 抓帧 → SD → 网页显示）

链路：`esp_camera`（外部组件 `esp32-camera`，见 `main/idf_component.yml`，**按 commit pin**）→ 抓帧 → 落 `/sdcard/cam/cap-%05u.jpg` → 经既有 `/media/<alias>/<path>` 只读面在网页显示（SPIFFS 兜底）。面板「板载摄像头」卡片可抓拍并显示最近一帧。

**可用配置（真机实测，2026-09-16）**——同一块板子上逐项对比得出：

| 配置 | 结果 |
| --- | --- |
| `fmt=rgb565` + `fb=psram` + `fbc=2` + `grab=when_empty` + **`psram_dma=1`** | ✅ 抓帧成功（320×240 JPEG 约 2.3–7 KB，110–130 ms/帧）；上云链路同时可用 |
| `fmt=jpeg` + `fb=psram`（`fbc=2`、`when_empty`） | ❌ `cam_hal: NO-SOI - JPEG start marker missing` → `Failed to get frame: timeout` ⇒ `fb=NULL`（**该模组本驱动下 JPEG 直出不可用**） |
| `fmt=rgb565` + `fb=dram`（上游 `lcd_camera` 例程的原始口径） | ❌ `esp_camera_init` 直接失败：单机跑例程时内部 DRAM 够，**本固件还跑 Wi-Fi/AEC/云链路**，2×153,600 B 内部帧缓冲拿不到 ⇒ `ESP_FAIL` |

**关键坑（内部 DMA 内存）**：`cam_hal` 在 **`psram_mode=false`** 时会额外申请 `dma_buffer_size=30720 B` 的**内部 DMA** 缓冲（`cam_hal.c:522`，仅非 psram 分支）。本板默认 `CONFIG_CAMERA_PSRAM_DMA` 未开 ⇒ 串口 `PSRAM DMA mode disabled` ⇒ 内部 RAM 只剩 ~18 KB、最大连续块 7.6 KB，**SDK 上云直接 `base start 失败：out of memory`**（表现为"摄像头一开，云端确认就没了"）。修法：`esp_camera_set_psram_mode(true)`（帧缓冲直接作 DMA 目标）⇒ 内部 `internal_free` 18 KB → ~26 KB、云端与摄像头**同时可用**。该结论已固化为默认值与 `/api/camera/reinit` 旋钮。

**排障旋钮（本地验证面）**：`POST /api/camera/reinit?fmt=jpeg|rgb565&fb=dram|psram&fbc=1|2|3&grab=when_empty|latest&xclk=10|20|40&psram=0|1&q=0..63` —— 在**同一块板子**上对比定位，不靠反复烧写猜；生效配置回显在 `/api/status.panel.camera{format,fb_loc,fb_count,grab,xclk_mhz,psram_dma,quality,stream_port,stream_frames,stream_clients}`。

**预览（第十九轮新增）**：两条通道，面板自动选优、失败自动回落——

| 通道 | 设备侧 | 实测 | 特点 |
| --- | --- | --- | --- |
| **MJPEG 流**（首选） | `GET http://<设备>:81/stream`（`camera_api_stream_start()` 起**第二个 httpd 实例**） | 12 s 收到 **93 帧 ≈ 7.8 帧/s**、429 KB（320×240，编码 ~110 ms/帧） | 长连接；因为跑在独立实例上，**主验证面 `:80` 的轮询全程 49–89 ms 正常、云链路不掉**（真机实测） |
| **单帧轮询**（回落） | `POST /api/camera/snapshot`（**不落盘**，内存 JPEG 直回） | 单帧 ~3.5–5.7 KB、~120 ms | 流端口连不上/浏览器不支持 MJPEG 时由面板自动切换（~1 帧/s），**不写 SD** |

> ⚠️ **边界**：预览是**局域网本地验证面**（:81 长连接 / 内存 JPEG），**不是**云端设备面契约的媒体数据面（`mpp`/`http_upload`）。因此**不得**据此声明 `video.live`——该能力位要求「编码 + 上行数据面」在契约承载上落地（见 §7/§9）。
> ⚠️ **内存代价**：第二个 httpd 实例占用内部 RAM（流任务栈 5 KB）⇒ 内部 `internal_free` 由 ~26 KB 降到 ~18 KB（真机读数）。若哪天起不来，串口会打 `预览流服务启动失败：…`，面板回落单帧轮询，**其余功能不受影响**。

**画面质量排查（第十九/二十轮真机）**：面板侧反馈"花屏严重"后做了**逐帧取证**并逐档定位，结论如下（全部为真机读数）：

| 检查 / 配置 | 结果 |
| --- | --- |
| 帧完整性（把设备发出的 MJPEG 全量落盘后按 multipart 切帧） | SOI/EOI/后随边界异常均为 0 ⇒ 帧结构完整（结构完整 ≠ 画面正确，见下） |
| **决定性对照** | **面板停止轮询时抓帧干净；面板在跑（每 600 ms 一次 `/api/status`）时同一配置必现水平彩带** ⇒ 与"并发网络负载 + DVP 数据率"强相关 |
| 逐档实测（均在并发 HTTP 下，320×240/160×120 × 10/20/40 MHz） | `QVGA/40` 花 · `QVGA/20` 花 · `QQVGA/40` 花 · `QQVGA/20` 干净 · **`QVGA/10` 干净**（分辨率不降也能稳） |
| 机制判读 | 彩带 = 整行错位 + 假彩色 ⇒ DVP→PSRAM 路径上**丢字节**（不是 FIFO 溢出：串口无 `FB-OVF`）；改 XCLK/分辨率即改"每秒字节数" → 命中阈值就干净 |
| **默认配置改为 `XCLK=10 MHz`**（`CAM_XCLK_HZ`，非上游例程的 40 MHz） | 保留 320×240，牺牲帧率换稳定；旋钮 `/api/camera/reinit?xclk=10\|20\|40&size=qvga\|qqvga` 现场可切 |
| 辅助对策（都已落地） | ① 读帧前显式 `esp_cache_msync(M2C\|INVALIDATE)`；② 抓帧/快照**连采 3 帧**、按"彩噪评分"挑最干净一帧（`panel.camera.last_noise/last_grabs` 可观测）；③ 流路径连采 2 帧（约 2.7 帧/s） |
| 附带修掉 | 反复 reinit 后偶发整体偏色 ⇒ 初始化时**显式打开** AWB/AWB 增益/曝光/增益（与上游 CameraWebServer 一致） |

> ⚠️ **构建不再依赖 GitHub（第二十轮）**：组件管理器每次 reconfigure 都要 `git fetch` 上游 `esp32-camera`，本机到 GitHub 连接不稳（两次因 `GnuTLS recv error (-110)` / 连接超时 139 s 直接卡住构建）⇒ 已把该组件**固化为工程内组件** `components/esp32-camera/`（来源提交 `3fb41a99…`，Apache-2.0，LICENSE 随源码保留），`main/idf_component.yml` 里保留了回到 git 方式的注释。
<br>注：`XCLK=10 MHz` 下预览约 2.7 帧/s、抓帧约 120–160 ms/帧 —— 验证面板够用，若要更高帧率需在**低并发**下把 XCLK 调回 20/40。

**边界（不得越过）**：抓帧只用于**验证面**（本地 HTTP + `/media` 只读面），**不声明 `video.live`** —— 采集声明的前提是"模组装配 + 真机取帧"（已满足，见映射页 §6-1），但 `video.live` 还要求**编码 + 上行数据面**（MJPEG/`http_upload`）落地，本工程未实现（见 §7/§9）。

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
| `ONEYE_FW_ENABLE_SDCARD` / `_LCD` | **y** / **y** | SD 缺省开（凭据文件、录音、板上回放都依赖它；未插卡只告警不阻塞）；LCD 已于 **2026-09-16 真机验证**（`panel.lcd.ready=true`、320×240、PSRAM 帧缓冲），故缺省开 |
| `ONEYE_FW_ENABLE_MPP` | n | 只初始化 mpp 域，不发起会话 |
| `ONEYE_FW_ENABLE_PANEL_API` | y | 设备侧本地验证 HTTP API（**量产置 n**）；`_PANEL_PORT` 缺省 80、`_KEY_HISTORY_MAX` 缺省 64 |
| `ONEYE_FW_DECLARE_VIDEO_LIVE` | **n** | 声明前提 = ① 模组装配 + ② 真机取帧（**均已满足**，映射页 §6-1 已转"确认"）**+ ③ 编码与上行数据面落地**（MJPEG/`http_upload`，本工程未实现）⇒ 仍保持 **n**；`audio.intercom` 无开关（WebRTC 数据面未落地前一律不得声明） |
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
| **按键三态（真机，第十五轮**真后端**闭环）** | 台面服务器 `175.178.190.187` 跑真后端（backend `spec-0084`：`rmng/dev/+/event/up` 消费 + `event/down` ack 生产）。真机取证：注入/按键 → 上行 `event/up`（信封 id 为 uuid、条目 id 为 `key-00001`）→ 服务端回 `{"type":"ack","data":{"ref":"<信封 id>","code":"ok"}}` → 设备侧历史 `uplink=acked ack_ms=21728`，面板"云端确认"列亮起；串口同步可见 `[cloud-ack] 云端确认批次（ref=…）→ 本地事件 key-00001 ⇒ acked` |
| ⚠️ **云端 ack 关联口径缺陷（真机，第十五轮已修）** | 症状：ack 帧**确实到达设备**（`rx_frames` 0→1、串口 `[sdk-event] EVENT_ACK len=123`）但面板"云端确认"恒为 `sent`。根因：契约 §4.3 的 `data.ref` = **上行信封 id**（SDK 生成 uuid，`oneye_envelope_build(…,id=NULL,…)`），而固件按"ack 负载里含本地 `key-%05u` 子串"关联 ⇒ 永不命中。修法：固件维护"已上报 item id"的 FIFO（`oneye_dev_event_report()` 强制成帧 ⇒ 一帧一条，FIFO 弹出精确对应；SDK `oneye_dev_event.c:1099`），命中即经 `panel_api_key_ack_item()` 置 `acked`；台面手动 ack 仍按子串兜底 |
| **上云链路（真机，第十三轮已闭环）** | 端点指向临时服务器 `175.178.190.187:1883` 后：`link up: 175.178.190.187:1883 transport=mqtt-tcp node=korvo2-0001`；真 EMQX 抓包收到 `status/up`（含 LWT 遗言 `{"online":false,"reason":"lwt"}`）、`caps/up`（`data{node_id,model_version,fw_version,sdk_version,caps,attrs,events,cmds}`）、`shadow/up`（`esp.fw_version`/`esp.power`）、`log/up`（`items[0]={level,tag:"oneye_base",msg:"板级自检：19 项 / 失败 0 项"}`）、`event/up`（按键事件，`data.items[].id=key-0000N`） |
| **按键三态（真机，第十三轮：云端桩）** | 本地检测（串口 `[key] <名>/<动作>` + `panel_api: key event #N … id=key-0000N`）→ 上行（真 EMQX `event/up`）→ 云端 ack（面板云端桩 `event/down` `{"type":"ack","data":{"ref":"key-0000N","code":"ok"}}`，`ref` 与上行 id 对齐）——**注**：桩的 `ref` 恰为本地 id，掩盖了上表的关联口径缺陷；第十五轮改走**真后端**并修正关联（见同表两行） |
| ⚠️ **真机崩溃链：ack 必崩（已修）** | 设备收到 `event/down` 即重启：`***ERROR*** A stack overflow in task oneye_net has been detected` + `Backtrace ... CORRUPTED`。根因 = **SDK 把 IDF `xTaskCreatePinnedToCore` 的栈单位当"字"**（IDF 是**字节**）⇒ 网络线程只有 4 KB 栈，回调一跑就溢出。修法：按字节传入 + 缺省 8 KB（SDK `3029bba`） |
| ⚠️ **真机录音 0/44 字节（已修）** | 串口：`sdmmc_cmd: allocate_dma_buf: not enough mem` + `AUDIO_THREAD: Error creating RestrictedPinnedToCore algo_fetch` + `afe_feed: handle or input data is NULL!` ⇒ 内部 RAM 被 Wi-Fi+SDK+AFE 吃满。修法：`SPIRAM_MALLOC_ALWAYSINTERNAL=4096` + 内部保留量；实测 `internal_free 39 KB→88 KB`、`largest 15 KB→40 KB`，录音恢复正常 |
| **录音时长精度（已修）** | ① 原「起管线即计时」把 AFE 预热算入（请求 5 s 得 ~4 s）；② 改「按文件大小探首帧」受 FATFS 写缓冲滞后（请求 5 s 得 ~8.4 s）；③ 现按 **fatfs_stream 的 `byte_pos`（已写字节数）** 精确计时：**请求 5 s → 163,884 B = 5.12 s** |
| **落盘健壮性（新增）** | 起管线前 512 B **写盘预检**，SD 不可写即回落 SPIFFS；采集 0 字节时显式报错（不再静默产出空文件） |
| **面板可观测性（新增）** | `/api/status.panel.heap{internal_free,internal_largest,dma_free,dma_largest,psram_free}`（本轮三类问题均靠它定位）；第十四轮再增 `panel.reset_reason`（`poweron/sw/panic/task_wdt/…`）——面板看到「离线/在线来回跳」时据此区分**设备重启**与网络抖动 |
| **录音通道配方修正（真机，第十四轮已闭环）** | 原实现把**双麦**口径（`AUDIO_ADC_INPUT_CH_FORMAT` = `"RMNM"` 4 通道 + `I2S_CHANNEL_FMT_RIGHT_LEFT`）喂给**单麦** AFE（`AFE_TYPE_VC`），AFE 只取首通道 ⇒ 幅度近乎静音（RMS 137/32768 ≈ 0.42%）。按 ADF 例程 `advanced_examples/algorithm` 在本板 **`RECORD_HARDWARE_AEC == true`（`board_def.h:116`）** 下实际走的**单麦分支**逐项照抄：`input_format "RM"`（2 通道：麦 + AEC 回采）、`I2S_CHANNEL_FMT_ONLY_LEFT`、16 kHz/32 bit、`ALGORITHM_STREAM_CFG_DEFAULT()`（TYPE1 + `AFE_TYPE_VC` + AEC\|NS）。修后串口 AFE 自述由 4 通道告警变为 **`Input PCM Config: total 2 channels(1 microphone, 1 playback)`**，原始 I2S 幅度回到环境噪声量级（RMS 123–390、峰值 600–1600，安静房间；此前异常样本 137 是 AFE **输出**幅度） |
| **录音前 ADC 重新 arm（新增）** | 例程同款：`audio_hal_ctrl_codec(adc_hal, AUDIO_HAL_CODEC_MODE_ENCODE, AUDIO_HAL_CTRL_START)` + 逐麦重设增益（MIC3=24 dB、MIC1\|MIC2=33 dB）。注意两点真机坑：① `s_board->audio_hal` 是 **ES8311（DAC）**，ADC 句柄是 **`s_board->adc_hal`（ES7210）**；② `es7210_adc_ctrl_state()` 的返回值是 **CLOCK_OFF 寄存器读值**（实测 0x20）而非错误码，**非 0 属正常**（曾误报 `ADC START 返回 ERROR`） |
| ⚠️ **录音到时必崩（真机，第十四轮已修）** | 录音到时自动停止后设备静默重启（`uptime_ms` 回退、面板显示离线/在线反复跳）：`assert failed: spinlock_acquire spinlock.h:142 (lock->count == 0)` @ `AFE: exit` 之后，`rst:0xc (RTC_SW_CPU_RST)`。根因与第七轮播放 `stop` **同一类**：`aec_capture_stop()` 在 `audio_pipeline_deinit()` 之后又对 `s_writer/s_wav/s_algo` 逐个 `audio_element_deinit()`（**双释放**）。修法：管线销毁后只置空句柄；I2S 读元素不属管线、另行销毁 |
| ⚠️ **回放永不出声且不结束（真机，第十四轮已修）** | 复现：播放 5.12 s 的录音，`playing=true` 持续 36 s 不结束、扬声器无声。逐级打点（新增诊断 `player_diag_dump()`：`file 已读 / decoder 已出 / i2s 已写` 字节数）定位为 **`i2s 已写 44 B` 即卡死** ⇒ 写元素不再被调度。根因：`player.c` 从 algorithm 例程抄了 **`i2s_cfg.task_stack = -1`**（那是「写元素由上游 `write_cb` 驱动」的用法），而本工程走的是官方播放例程 `pipeline_play_sdcard_music` 的 `decoder → i2s` 常规链路：**无任务元素经 `i2s_stream_set_clk()`（内部 `pause`→`resume`，日志可见 `[i2s] RESUME timeout`）后不再被调度**。修法：去掉 `task_stack = -1`（用默认任务栈），并按官方例程补上 `audio_element_setinfo(i2s, &music_info)` → `i2s_stream_set_clk(...)` 的顺序。修后串口 `IN-[i2s] AEL_IO_DONE` → **`player: 播放结束`**，`/api/status.player.playing=false` |
| **录音/回放互斥（新增）** | 本板 ES7210(ADC) 与 ES8311(DAC) **共用 I2S0**：「播放中启动录音」会把两条管线都卡死（AFE 持续 `Ringbuffer of AFE is empty`，随后重启）⇒ `aec_capture_start()` 拒绝在回放中录音、`player_play()` 拒绝在录音中回放（`ESP_ERR_INVALID_STATE`，面板返回 `busy: already recording or playing (shared I2S0)`）。配套：录音的 I2S 读元素改为**按需创建、结束时销毁**（不再常驻占用端口） |
| **稳定性复测（真机，第十四轮）** | `play → rec 5 s → play → rec 5 s → 守卫拒绝 → stop` 全流程：**重启 0 次**（`uptime_ms` 单调 8,246→76,220 ms）、两次录音均 163,884 B = 5.12 s、两次回放均 `播放结束`、`in-app 守卫` 正确拒绝重叠操作 |
| **云端授时闭环（真机，第十六轮）** | 服务端（backend spec-0085）回 `caps/down{cloud_ts}` → 串口 `[oneye][base][I] time synced: cloud_ts=1789542529385 offset=…` → `/api/status.panel.time = {synced:true, cloud_ts_ms:1789542811275, offset_ms:1789542806775, source:"caps/down.cloud_ts", now_ms:1789542821824}` ⇒ **按键上行帧 `ts` 由运行时刻（≈8e5）变为 UTC 毫秒（≈1.79e12）**，端到端时延 66–85 ms（设备侧时钟内相减） |
| ⚠️ **授时即重启（真机，第十六轮已修）** | 收到 `caps/down` 后必现 `***ERROR*** A stack overflow in task oneye_net has been detected.` + `rst:0xc (RTC_SW_CPU_RST)`（表现为"一授时就重启"）。根因：SDK 网络线程栈上放 `oneye_envelope_t`（内含 `data[4096]`）+ `local[1024]`，叠加 `TIME_SYNCED` 应用回调链后突破 8 KB 缺省栈。修法：网络线程栈 **8 KB → 16 KB**（`oneye_internal.c` 的 `oneye_bint_start`），且 `caps/down` 处理的两个缓冲改**静态**（该面仅网络线程串行处理）。**注**：SDK 以预编译库链接，改内部源码须重跑 `build-all.sh`（见 §5.6 陷阱） |
| ⚠️ **`build-all.sh --firmware` 会重置 sdkconfig（真机，第十六轮踩到）** | 台面云地址与内存保留值被 `sdkconfig.defaults` 覆盖 ⇒ 设备改连占位 `192.168.1.100:1883`、AFE 录音内存不足。处置：台面值备忘 `output/.build/bench-sdkconfig.txt` + §5.6 的构建注意 | 面板按键条与 `/api/keys` 的按键清单原按**下标 0..5** 猜标签（`volup/voldown/set/play/mode/rec`），而本板 `board_def.h` 的 `INPUT_KEY_DEFAULT_INFO()` 实为 **REC=1 / MUTE=7 / SET=2 / PLAY=3 / VOLUP=6 / VOLDOWN=5**（**且本板无 MODE 键**，id 也不从 0 起）⇒ 真机表现：历史里出现 `MUTE`，而按键条显示 `MODE`、VOL± 错位。修法：清单按 **ADF user_id** 定义（`panel_api.c` 的 `k_panel_keys`），`/api/keys` 与注入校验（`simulate/key`）同源取值 |
| ⚠️ **面板 MQTT 观测通道静默死链（第十五轮已修，面板侧）** | 症状：12 次真机按键，面板只观测到 1 次 event/up，其余"云端确认"恒为**未见**，而 `mqtt.connected` 一直显示 true。根因：`MiniMqtt` 只在**收到数据**时才发 PINGREQ ⇒ 空闲超过 broker 的 `keepalive×1.5` 被判失联；对端关闭后 `recv` 返回 EOF、`fileno()` 仍有效 ⇒ 旧代码把它当超时 `continue`，**既不重连也不报错**。修法：空闲也按 `keepalive/2` 发 PINGREQ、EOF 即判对端关闭、`keepalive×2` 无入站数据判死链重连；"云端确认"列在 MQTT 未连通时显示**未连通**（而非误导性的"未见"）。复测：注入 6 次 + **空闲 80 s** + 再注入 2 次 ⇒ 8/8 全部观测到、连接未断 |
| **板载 LCD（真机，第十七轮已闭环）** | `panel.lcd={ready:"true",w:320,h:240,pattern:"status",draws:N,fb_bytes:153600,fb_mem:"psram"}`；`POST /api/lcd/draw?pattern=bars\|grid\|checker\|test\|status` 逐个生效（`draws` 递增、`last_ms` 更新），按键注入即触发状态屏重绘；面板「板载 LCD」卡片 + `/api/action/lcd` 代理已验证（人眼/拍照对账） |
| **板载摄像头（真机，第十八轮已闭环）** | `panel.camera={inited:"true",sensor:"OV3660",pid:13920(0x3660),format:"rgb565",fb_loc:"psram",fb_count:2,grab:"when_empty",xclk_mhz:40,psram_dma:1,root:"/sdcard/cam"}`；串口 `Camera PID=0x3660 / Detected OV3660 camera / Detected camera at address=0x3c / sccb-ng: pin_sda 17 pin_scl 18`；`POST /api/camera/capture` → `/sdcard/cam/cap-0000N.jpg`（320×240，2.3–7.0 KB，110–130 ms），`/media/sdcard/cam/…` 下载后经宿主解码核对 = **真实图像**（均值亮度 54–148、17 档亮度分布，非纯色/花屏）；面板「板载摄像头」卡片抓拍 + 显示最近一帧已跑通 |
| ⚠️ **摄像头 JPEG 直出不可用（真机，第十八轮查明）** | `fmt=jpeg` 时串口持续 `cam_hal: NO-SOI - JPEG start marker missing` → `Failed to get frame: timeout` ⇒ `esp_camera_fb_get()` 恒 NULL。改用 `rgb565` + 软件 `frame2jpg()` 落盘 JPEG 后正常（配置矩阵与判据见 §5.8） |
| ⚠️ **摄像头吃内部 DMA ⇒ 上云 OOM（真机，第十八轮已修）** | `PSRAM DMA mode disabled` 时驱动额外申请 **30,720 B 内部 DMA**（`cam_hal.c:522` 非 psram 分支）⇒ 内部 `internal_free` 54 KB→18 KB、最大连续块 30 KB→7.6 KB ⇒ SDK `base start 失败：out of memory`（**云链路与摄像头互斥**）。修法 `esp_camera_set_psram_mode(true)`（`psram_dma=1`）：内部余量回到 ~26 KB，**云链路（`link_up=true`、`tx_frames=5`、授时 `synced=true`）与抓帧同时可用** |
| ⚠️ **抓拍后网页无图（真机，第十九轮已修）** | 现象：面板提示「已抓拍」但图不显示。根因：`/media/list` 只扫 `rec/` 与根目录，**从不扫 `cam/`** ⇒ 面板按 `cam/*` 过滤媒体列表永远为空、`<img>` 拿不到 src（而文件本身 `GET /media/sdcard/cam/…` 一直是 200 + `image/jpeg`）。修法：① 设备侧 `/media/list` 增加 `cam` 目录扫描；② 面板抓拍后**直接用抓拍响应里的 `url` 贴图**，不再依赖媒体列表轮询 |
| **预览流（真机，第十九轮新增）** | `panel.camera.stream_port=81`；`curl` 实测 12 s → **93 帧 / 429,540 B ≈ 7.8 帧/s**，首段为 `--oneyeframe / Content-Type: image/jpeg / Content-Length: …`；**同时** `GET /api/status` 49–89 ms 正常、`cloud.link_up=true`（独立 httpd 实例的价值）；面板「开始预览/停止预览」按钮 + `GET /api/camera/snapshot.jpg` 回落通道均已验证 |
| **摄像头探测的初始化顺序（真机，第十八轮查明）** | 把 `camera_api_init()` 放到 `board_init_peripherals()` **之前**（照抄上游 `lcd_camera` 的 "camera init in advance" 注释）⇒ `camera probe … no sensor FAIL`（ADF I2C 总线尚未建立）；放在**板级初始化之后**⇒ 探测成功。另：该失败曾因计入硬自检而触发 `SELFTEST_STRICT` 中止上云 ⇒ 设备连 IP 都拿不到（**看不到失败原因**），故摄像头改用提示级 `chk_warn`（记红行、不中止） |
| **待续（未闭环，明确记录）** | ① **可听性**：回放链路已把 PCM 完整时钟输出（`AEL_IO_DONE` + 时长吻合），但「扬声器是否真的出声」需人耳确认（PA `GPIO48` 已在 `es8311_codec_init` 打开、音量 80）；② **麦克风灵敏度**：原始幅度随环境变化（123→390），对着板子说话的幅度取证待补；③ **`video.live` 上行数据面**（MJPEG 编码 + `http_upload`/`mqtt_frame`）未实现 ⇒ 该能力位仍不得声明 |
| ⚠️ **镜像余量告急（第十八轮）** | `korvo2_oneye.bin` **2,045,856 B**（`0x1f37a0`），`factory 2M` 分区**仅余 2%**（0xc860）。后续增长首选 `CONFIG_COMPILER_OPTIMIZATION_SIZE=y`（-Os），或扩 `factory` 分区（16 MB flash 尚有余量） |

**后续（真机）**：`idf.py -p <COM> flash monitor` → 核对自检逐行 PASS → **SD 卡放 `oneye-wifi.txt` 复位自动配网** → 观察
`caps/up` / `status/up`（retained + LWT）/ `shadow/up` / `log/up` 与按键 `event/up`；其间可用面板「Wi-Fi 配网」卡片核对**凭据来源**；
LCD 状态屏与摄像头抓帧见 §5.7/§5.8（**已真机闭环**），`video.live` 仍须等编码+上行数据面落地。

## 9. 边界

- 本工程**不实现**：媒体数据面（webrtc/http_upload/mqtt_frame，含 `video.live` 上行）、PTZ/命令执行面、
  AI 端侧初筛、LED 显示服务、触摸、电池采集；
- 板载 **LCD 状态屏**、**摄像头抓帧**与**局域网 MJPEG 预览**（:81）属**本地验证面**（`/api/*`、`:81/stream`、既有 `/media/*` 只读面），不是云端设备面契约的一部分：**不新增 topic / 影子键 / 能力位**；预览也**不构成** `video.live` 的声明依据（缺契约承载的编码+上行数据面）；
- 抓帧与云端链路共享内部 DMA 内存：改摄像头配置后必须同时核对 `panel.heap.internal_free` 与 `cloud.link_up`（见 §5.8 的关键坑）；
- 上游资产纪律：`esp-adf` 上游分支（`master` / `release/*`）与 `esp-repo/` 镜像**只读**，我方改动只进集成分支；`esp32-camera` 以 `idf_component.yml` **按 commit pin** 引入（不静默跟随上游默认分支）；
- 真机烧录/取证（录音可听性、麦克风灵敏度、SD 插拔）登记为后续卡，不在本工程内声称通过。
