# bedrail —— 床护栏「网关 + 子设备（温度/GNSS/霍尔）」例程

> 2026-11-02 新增。目标：用 **ESP32-C6-GEEK（网关，COM3）** + **ESP32-C6-ZERO（子设备，COM4）**
> 把子设备的**全部传感数据**（温度/电量/GNSS/霍尔）经 BLE → 网关 → MQTT/TLS → 云端影子，
> 最后在 App 设备详情的「最近上报」里看到 `temperature` / `battery` / `latitude` / `gnssState` /
> `hallState` 等字段。

## 1. 目录与角色

```
embedded/esp-adf/examples/oneye/bedrail/
├── gateway/    网关（C6-GEEK，COM3）：甲型 OY-GAW-01，BLE 中心 + App 配网 + 上云 + sub.* 代传
├── sensor/     子设备（C6-ZERO，COM4）：**完整复制 thermometer-t31（sub-c6）** + DRV5032FB 霍尔，⛔ 不连云
└── README.md   本文件（拓扑 / 接线 / 联调顺序）
```

拓扑：

```
DRV5032FB ──GPIO1──► C6-ZERO sensor（完整 thermometer-t31 sub-c6：温度/电量/GNSS/霍尔）
                         │  A6 03 | 56 01 <gnssState> 57 01 <hallState> | sum | 6A （10 B）
                         ▼  BLE notify
                    C6-GEEK gateway（BLE 中心，甲型 creds）
                         │  event/up  sub.telemetry {node,model,rssi,cmd:"0x03",tlv}
                         ▼  MQTT/TLS mqtt.oneye.me:18886
               cloud transport SubDeviceIngest.decodeT31
                         │  0x57 → hallState（0/1）→ TSL 校验 → 子设备影子 + Kafka
                         ▼
              Redis oneye:dev:{tenant}:shadow:{childDeviceId} → App 详情「最近上报」
```

- 子设备**不连云**、没有 MQTT 身份；它复用既有子设备 `KORVO2-THE01`
  （productKey `OY-THE-01`）与既有云端映射 `oneye:dev:child:C6-GAW01:KORVO2-THE01`。
- 网关仍用 `OY-GAW-01`（甲型，一机一密 creds 分区）。
- ⚠️ 空口模型名仍为 **T31**（`T31-xxxx` 广播），这是**有意复用**：网关 link-sdk 档案按
  `name_prefix="T31"` 匹配，云端子设备映射也复用 `OY-THE-01`；不要改成 `BED-xxxx`，
  否则网关扫不到、云端也认不出（除非同批改档案 + 新机型）。

## 2. 接线（DRV5032FBLPGM → C6-ZERO）

| DRV5032FBLPGM（TO-92，顶视图） | 方向 | ESP32-C6-ZERO | 说明 |
| --- | --- | --- | --- |
| 1 VCC | ← | **3V3** | 1.65–5.5 V；本板 3.3 V |
| 2 GND | — | **GND** | 必须共地 |
| 3 OUT | → | **GPIO1** | 推挽输出；磁场 |B| > BOP 时输出低 |
| — | — | 3V3–GND 之间 **0.1 µF** | 建议靠近传感器电源脚 |

- **不需要外部上拉**：`DRV5032FB` 是**推挽输出**（数据手册 Table 4-1：FB = Omnipolar /
  Push-pull / 5 Hz），不是开漏。
- `hallState` 语义（固件与云端唯一口径）：
  - **1 = 有磁**（OUT 低）—— 磁铁靠近，机械上定义为**护栏闭合/拉起**；
  - **0 = 无磁**（OUT 高）—— 磁铁离开，机械上定义为**护栏打开/放下**。
- 机械安装相反时，只改 sensor 的 `CONFIG_ONEYE_BEDRAIL_HALL_ACTIVE_LOW`，
  ⛔ 不要改云端物模型语义。
- 引脚选择依据：C6-ZERO 排针 GPIO0–GPIO5 / GPIO13–GPIO23；避开了 strapping
  GPIO4/5/8/9/15、USB GPIO12/13、JTAG GPIO6/7、UART0 console GPIO16/17；
  GNSS 仍保留：UART1 GPIO20/21/22。
- 数据手册：[DRV5032 datasheet (SLVSDC7H)](https://www.ti.com/lit/gpn/DRV5032)，
  LPG/TO-92 引脚见 Figure 5-5；微雪板引脚见
  [ESP32-C6-Zero 文档](https://docs.waveshare.net/ESP32-C6-Zero/)。

### 2.1 CR2032 预留（可选，仅霍尔）

- **可做**：CR2032 只给 DRV5032 供电（0.69 µA 平均 / 2.7 mA 峰值），预留 2-pin `J_BAT`
  + BAT54 双二极管 OR-ing + 0.1 µF + OUT 串 10 kΩ。
- **不可做**：CR2032 给整块 C6-ZERO 供电。C6-ZERO 的 `3V3` 是 LDO 输出、没有 VBAT；
  BLE 峰值电流远超 CR2032 的 1–3 mA 连续能力，会掉压复位。
- 整板电池方案请改 LiPo/Li-ion + LDO/充电管理（或 2×AA/CR123A），不要用纽扣电池。
- 详细接线/BOM 见 [sensor/README.md §2.1](sensor/README.md)。

## 3. 构建与烧录

环境：ESP-IDF 5.5.5（本仓既有口径），`ADF_PATH=<repo>/embedded/esp-adf`。

```powershell
# 子设备（COM4；完整 thermometer-t31 复制，默认 SUBDEV）
cd embedded\esp-adf\examples\oneye\bedrail\sensor
idf.py -B build-sensor -DSDKCONFIG=build-sensor/sdkconfig `
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32c6;sdkconfig.defaults.esp32c6.subdev;sdkconfig.defaults.subdev" `
  set-target esp32c6
idf.py -B build-sensor build

# 网关（COM3）
cd ..\gateway
bash tools/build-gateway.sh          # 或按 README 里的 idf.py 命令逐条跑
```

烧录（应用分区都在 `0x10000`；以 `build-*/flash_args` 为准）：

```powershell
# 子设备
python -m esptool --chip esp32c6 -p COM4 -b 460800 write_flash `
  0x0 build-sensor\bootloader\bootloader.bin `
  0x8000 build-sensor\partition_table\partition-table.bin `
  0x10000 build-sensor\bedrail_sensor.bin

# 网关（creds 分区另行写入，见 gateway/README.md）
python -m esptool --chip esp32c6 -p COM3 -b 460800 write_flash `
  0x0 build-gw-c6\bootloader\bootloader.bin `
  0x8000 build-gw-c6\partition_table\partition-table.bin `
  0x10000 build-gw-c6\bedrail_gateway.bin
```

## 4. 云端（必须**先**于固件烧录生效）

> ⚠️ 迁移件已写盘，但**本会话未在云库执行**（云端 SSH 公钥不可用）；以下 1–3 由有权限的
> 运维/开发执行，4 是代码部署。

1. 执行迁移 `cloud/docs/data/migration/M-20261102-01-dev-product-oy-the-01-hall-state.sql`
   （`OY-THE-01` 物模型 1.0.2/13 props → **1.0.3/14 props**，追加 `hallState` enum 0/1）。
2. TDengine 加列：
   `ALTER STABLE oneye_ts.\`dev_property_oy_the_01\` ADD COLUMN \`hallState\` TINYINT;`
3. 刷新 Redis 下发面 `oneye:dev:tsl:OY-THE-01`（payload 的 `tsl` 与 DB 同源；
   接入层 60 s 内生效）——否则 `TslRegistry` 会因 `hallState` 不在 TSL 里**整帧拒绝**。
4. 部署带 `decodeT31` 0x57 解码的接入层 jar（见 `SubDeviceIngest.java` / 单测 21 条）。

固件与云端的顺序：**先 TSL + Redis + 接入层，再烧子设备/网关**。

## 5. 实机验收（只看运行时读数）

子设备（COM4）：

```
I (xxx) hall: DRV5032FB 初始化完成：OUT=GPIO1（推挽/全极/5 Hz，低电平=有磁）
I (xxx) t31sub: ==== bedrail 床护栏子设备（ESP32-C6/ZERO；温度+GNSS+霍尔）启动 ====
I (xxx) t31sub: 广播中：name=T31-xxxx / 服务 0000FFF0… / FFF1 notify
I (xxx) t31sub: → notify 温度帧 #N：36.5x ℃（模拟）
I (xxx) t31sub: → notify TLV 帧 #N：0x57 hallState=0（打开/无磁）／0x56 gnssState=1（10 B）
I (xxx) t31sub: 霍尔状态变化 ⇒ hallState=1（闭合/有磁）
```

网关（COM3）：

```
I (xxx) t31gw: [子设备] ... ⇒ sub.telemetry
I (xxx) oneye_link.up: event/up 已提交（sub.telemetry）
```

云端/App：

- Redis：`HGET oneye:dev:{tenant}:shadow:{childDeviceId} hallState` → `0` / `1`；
- TDengine：`SELECT hallState FROM oneye_ts.d_{childDeviceId}` 有值；
- App：设备详情「最近上报」出现 `hallState  1`（`ShadowBlock` 遍历 shadow map，
  **不需要改 APK**）。

## 6. 已知限制

1. 温度仍是**模拟值**（完整复制自 thermometer-t31，60 s 正弦 36.0–37.5 ℃），
   不是真实测量；接真实温度传感器时只改 `t31_sim_temp_centi()`，帧格式不变。
2. 复用 `OY-THE-01` 是联调口径；量产建议拆独立机型（届时同批改
   `sdkconfig` 的 productKey/网关档案/云端子设备映射）。
3. DRV5032FB 是 5 Hz 采样（200 ms 更新），护栏快速抖动可能被 5 Hz 采样+去抖滤掉；
   这是器件特性，不是固件丢帧。
