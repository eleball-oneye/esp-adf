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

## 2. 烧录（本轮**未执行**：真机烧录与取证按计划拆为后续卡）

```bash
idf.py -p <COMx> flash monitor        # Windows；Linux/macOS 为 /dev/ttyUSB0 或 /dev/ttyACM0
```

- Boot 键 + Reset 键进入下载模式；或由串口 DTR/RTS 自动下载（rst「自动下载」）。
- 扬声器接 **扬声器输出端口**；USB 供电建议 ≥5 V/2 A（rst「供电说明」）。

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

## 6. 配置（`idf.py menuconfig` → `korvo2_oneye 板级固件配置`）

| 配置 | 缺省 | 说明 |
| --- | --- | --- |
| `ONEYE_FW_DEVICE_ID` | `korvo2-0001` | 兼作 MQTT username/client_id（EMQX ACL `%u` 依赖） |
| `ONEYE_FW_CLOUD_HOST` / `_PORT` / `_WS_PATH` | 192.168.1.100 / 0 / 空 | 端点显式配置（端口 0 = 按承载取契约缺省 1883/8883/8083/8084） |
| `ONEYE_FW_TRANSPORT` | TCP | 承载选择（契约四承载） |
| `ONEYE_FW_CLOUD_TOKEN` | 空 | 设备令牌（空 = 匿名 dev 形态） |
| `ONEYE_FW_TLS_INSECURE` | **n** | 仅 dev/产测可开；生产须投放自研 CA（`tls_ca_pem`） |
| `ONEYE_FW_WIFI_SSID` / `_PASSWORD` | 空 | 空 = 只跑板级自检与按键，不上云 |
| `ONEYE_FW_ENABLE_KEYS` | y | 6 键 → `event/up` |
| `ONEYE_FW_ENABLE_SDCARD` / `_LCD` | n | 板级外设（未确认项不默认启用） |
| `ONEYE_FW_ENABLE_MPP` | n | 只初始化 mpp 域，不发起会话 |
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
| 板卡选择核对 | `board-config.txt` = `CONFIG_IDF_TARGET_ESP32S3=y` + `CONFIG_ESP32_S3_KORVO2_V3_BOARD=y`（由 `--firmware` 轨自动核对） |
| SDK 链接形态 | 构建日志：`链接预编译库（4 分域库，toolchain=xtensa-esp32s3-elf-gcc-14.2.0）` |
| 编译期断言 | `board_expect.h` 40+ 条 `_Static_assert` 全部通过（**失败即构建失败**，本轮已用它拦住 4 处真实问题，见总控集成说明 §10.3） |
| 运行期自检 | **未跑真机**（本轮只出构建产物）——烧录后应看到 `[board-check] … PASS` 逐行输出与 `结果：N 项，失败 0 项` |
| 门禁 | 宿主轨（4×`.a`+4×`.so`+demo+单测 17 组/215 用例/2890 断言）与 backend `contract_check` / `md_link_check`、总控 `md_link_check` 全绿（契约面零改动） |

**后续（真机）**：`idf.py -p <COM> flash monitor` → 核对自检逐行 PASS → 配 Wi-Fi/云端端点（menuconfig）→ 观察 `caps/up` / `status/up`（retained + LWT）/ `shadow/up` / `log/up` 与按键 `event/up`；摄像头取帧另行取证（决定 `video.live` 是否可声明）。

## 9. 边界

- 本工程**不实现**：媒体数据面（webrtc/http_upload/mqtt_frame）、PTZ/命令执行面、AI 端侧初筛、LED 显示服务、
  触摸、电池采集、camera 取帧（依赖外部 `esp32-camera`）；
- 上游资产纪律：`esp-adf` 上游分支（`master` / `release/*`）与 `esp-repo/` 镜像**只读**，我方改动只进集成分支；
- 真机烧录/取证（录音、取帧、LCD、按键、SD 插拔）登记为后续卡，不在本工程内声称通过。
