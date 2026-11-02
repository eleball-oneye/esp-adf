# bedrail/sensor —— 床护栏子设备（ESP32-C6-ZERO，COM4）

> **完整复制自 `examples/thermometer-t31`**（不是裁剪骨架）：温度模拟、电量帧、
> GNSS 定位帧、`gnssState` 全部保留；只追加 **DRV5032FB 霍尔**（`0x57 hallState`）。
> 默认角色 = **SUBDEV**（BLE 外设 `T31-xxxx`，⛔ 不连云）。
> 编译器按 `CONFIG_ONEYE_T31_ROLE_SUBDEV` 分派；网关角色源码仍在，但本工程不构建。

## 1. 子设备上报的全部传感数据

| 帧 | cmd | 内容 | 云端影子属性 |
|---|---|---|---|
| 温度 | `0x01` | i16 LE 温度 0.01℃ + u16 LE 序号 | `temperature` |
| 电量 | `0xB1` | u8 电量% + u16 LE mV | `battery` / `batteryMv` |
| GNSS 定位 | `0x02` | i32 LE 纬度/经度、i16 LE 海拔、u16 LE 地速、u8 卫星数、u8 fix | `latitude` / `longitude` / `altitude` / `speed` / `satellites` / `gnssFix` |
| 通用 TLV | `0x03` | `0x56 gnssState` + **`0x57 hallState`** | `gnssState` / `hallState` |

霍尔语义：
- `hallState = 1`：OUT 低（有磁）→ 护栏闭合/拉起
- `hallState = 0`：OUT 高（无磁）→ 护栏打开/放下

温度仍是**模拟数据**（60 s 正弦 36.0–37.5 ℃），与 thermometer-t31 完全一致；接真实温度传感器时只改 `t31_sim_temp_centi()`，帧格式不变。

## 2. 接线（DRV5032FBLPGM，TO-92 顶视图）

| DRV5032 引脚 | 接到 C6-ZERO | 说明 |
|---|---|---|
| 1 VCC | **3V3** | 1.65–5.5 V |
| 2 GND | **GND** | 共地 |
| 3 OUT | **GPIO1** | 推挽输出，**无需上拉** |
| — | 3V3–GND 0.1 µF | 建议靠近传感器 |

C6-ZERO 同时保留：
- GNSS：UART1 `GPIO20`(TX→模组 RXD) / `GPIO21`(RX←模组 TXD) / `GPIO22`(PPS，可选)；
- 霍尔：`GPIO1`；
- 避开 strapping GPIO4/5/8/9/15、USB GPIO12/13、JTAG GPIO6/7、UART0 console GPIO16/17。

**与 GNSS 共存的接线建议**：
- 霍尔只新增 **1 根信号线**（OUT→GPIO1）；VCC/GND 与 GNSS 共用板上的 3V3/GND，不必再占引脚。
- ⛔ 不要为了省线把霍尔接到 GPIO20/21/22；即使 PPS 不接，GPIO22 也建议保留给 GNSS。
- 霍尔线走 C6-ZERO **左侧排针**（GPIO0–GPIO5 一侧），GNSS 走右侧 GPIO20/21/22，两边分开走线，
  避免插错/干扰；霍尔线若 >20 cm，OUT 串 100 Ω、GPIO1 对地加 100 nF。
- 若后续要加电池电压检测，再占 `GPIO2`（ADC）；霍尔仍固定 GPIO1。

**GND 不能省、也不建议用 GPIO 代替**：
- DRV5032 的 GND 是内部电路与推挽输出的参考地；悬空 ⇒ 输出无定义，且 OUT 可能经
  C6 的 ESD 二极管倒灌，读数随机/损坏风险。
- GND 与 GNSS 共用板上的 GND 即可；GND 排针不够时用一分二杜邦线/焊一个地排，
  ⛔ 不要为了省一根线去占 GPIO。
- 如果确实要 GPIO 控制霍尔断电，用 **N-MOSFET 低边开关**（BSS138/2N7002）：
  `GPIOx → 100 Ω → Gate`、`Gate → 100 kΩ → GND`、`Source → GND`、`Drain → DRV5032 GND`；
  ⛔ 不要直接把 GPIO 当 GND（复位/启动期 GPIO 是高阻，地会浮空）。
- 用 CR2032 只供霍尔时，霍尔 GND 必须与 C6 的 GND 共地；C6 断电后 GPIO 不是地。

依据：TI [DRV5032 数据手册](https://www.ti.com/lit/gpn/DRV5032) Table 4-1（FB = 全极/推挽/5 Hz）、
Figure 5-5（LPG 引脚）；微雪 [ESP32-C6-Zero 引脚](https://docs.waveshare.net/ESP32-C6-Zero/)。

### 2.2 霍尔阈值/灵敏度（选型参考）

`DRV5032FB` 的 BOP/BRP 是**出厂固定**的，没有 trim/编程脚，**改固件不能降低阈值**：

| 型号 | BOP max | 极性/输出/采样 | TO-92 可用 | 说明 |
|---|---|---|---|---|
| **DRV5032FB（当前）** | 4.8 mT | 全极 / 推挽 / 5 Hz | ✅ | 阈值固定 |
| DRV5032DU | **3.9 mT** | 单极 / 推挽 / 20 Hz | ✅（pin3 = OUT2，南极点） | 同族最低阈值，但只响应单极，磁铁方向要固定 |
| DRV5032DG | **3.4 mT** | 单极 / 推挽 / 80 Hz | ❌（仅 X2SON） | 阈值更低但封装不兼容 |
| DRV5032FA/FC/FD | 4.8 mT | 推挽/开漏/单极 | 部分 | 阈值不比 FB 低 |

> 结论：**同封装可选的更低阈值只有 DRV5032DU（3.9 mT max）**，改善约 1 mT；
> 想明显增加感应距离，优先用**更强的钕磁铁、减小气隙、对准感应面**，
> 而不是只换 1 mT 的阈值。需要“可调阈值”时应改用**线性霍尔 + ADC + 软件阈值**
> （如 DRV5055/DRV5053 一类），或可编程霍尔开关；DRV5032 做不到。

机械/磁路建议：
- 磁铁垂直对准封装感应面，尽量正对霍尔元件中心，不要从边缘滑过；
- 气隙每减小 1 mm，磁场通常比“降 1 mT 阈值”带来的距离提升更明显；
- 可用软铁/坡莫合金极靴把磁通聚到感应面；
- 降低阈值会同时降低 BRP/迟滞，更容易被邻近磁铁、玩具、门吸误触发；床护栏场景建议保留足够迟滞。

### 2.1 CR2032 纽扣电池预留（可选，仅霍尔供电）

> **结论：CR2032 可以给 DRV5032 霍尔单独供电；⛔ 不能给整块 C6-ZERO 供电。**
> C6-ZERO 的 `3V3` 是 ME6217 LDO 输出（不是 VBAT 输入），且 BLE 发射峰值远大于
> CR2032 的 1–3 mA 连续 / 十几 mA 脉冲能力（内阻 10–40 Ω 会直接掉压复位）。
> 需要整板电池供电时改用 LiPo/Li-ion + LDO/充电管理，或 2×AA/CR123A；⛔ 不要用 CR2032。

推荐预留一个 **2-pin 电池座/排针 `J_BAT`**，只给霍尔供电，C6 仍由 USB/3V3 供电：

| 预留引脚/器件 | 连接 | 说明 |
|---|---|---|
| `J_BAT` pin1 `VBAT+` | CR2032 + | 3 V |
| `J_BAT` pin2 `GND` | CR2032 − / GND | 共地 |
| `D1` BAT54 | `VBAT+` → `HALL_VCC` | 电池 OR-ing；3V3 在时反偏不漏电 |
| `D2` BAT54 | `3V3` → `HALL_VCC` | 有 3V3 时优先由板供电 |
| `C1` 0.1 µF | `HALL_VCC` – GND | 靠近 DRV5032 |
| `R1` 10 kΩ | DRV5032 OUT → GPIO1 | 限流，防 C6 断电时经 GPIO 倒灌 |
| （可选）`GPIO2` | `VBAT+` 经 1 MΩ/1 MΩ 分压 | 电池电压检测；不用则保持空闲 |

接线：

```text
CR2032 + ── J_BAT ── D1(BAT54) ── HALL_VCC ── DRV5032 pin1 VCC
3V3 ───────────────── D2(BAT54) ──┘        │
GND ────────────────────────────────────────┴── DRV5032 pin2 GND
DRV5032 pin3 OUT ── R1 10k ── GPIO1
HALL_VCC ── C1 0.1µF ── GND
```

- 3V3 存在时：D2 导通，霍尔由板供电，CR2032 不放电（D1 反偏）。
- 3V3 不存在时：CR2032 经 D1 给霍尔供电（约 2.7 V，DRV5032 最低 1.65 V，正常）。
- DRV5032 平均 0.69 µA（3 V）/ 峰值 2.7 mA，CR2032 容量 ~220 mAh，
  理论寿命由电池自放电/漏电决定（远大于设备寿命）。
- ⛔ 不要把 `J_BAT` 直接并到 `3V3`：会倒灌 LDO/整板，且 BLE 峰值会拉垮纽扣电池。
- 如果不做电池，`J_BAT`/D1/D2/C1/R1 可全部不装；GPIO1 仍直连霍尔 OUT 也能工作（R1 可 0 Ω）。

## 3. 构建 / 烧录

```powershell
cd embedded\esp-adf\examples\oneye\bedrail\sensor
idf.py -B build-sensor -DSDKCONFIG=build-sensor/sdkconfig `
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32c6;sdkconfig.defaults.esp32c6.subdev;sdkconfig.defaults.subdev" `
  set-target esp32c6
idf.py -B build-sensor build
```

或 `bash tools/build-sensor.sh`（WSL/Linux）。

Windows 实测（2026-11-02）：`bedrail_sensor.bin` = **0xA5D60 B**（4 MB app 分区余 84%）。

```powershell
python -m esptool --chip esp32c6 -p COM4 -b 460800 write_flash `
  0x0 build-sensor\bootloader\bootloader.bin `
  0x8000 build-sensor\partition_table\partition-table.bin `
  0x10000 build-sensor\bedrail_sensor.bin
```

## 4. 空口帧

```
温度： A6 01 | i16le temp | u16le seq | sum | 6A          （8 B）
电量： A6 B1 | u8 pct | u16le mV | sum | 6A               （7 B）
GNSS： A6 02 | i32le lat lon / i16le alt / u16le spd / u8 sats fix | sum | 6A  （18 B）
通用： A6 03 | 56 01 <gnssState> 57 01 <hallState> | sum | 6A                （10 B）
```

`0x57` 是本仓扩展标签；网关 `gw_child.c` 对 `cmd=0x03` **只做 tag/len 边界校验并原样转发**，
云端 `SubDeviceIngest.decodeT31` 解成 `hallState`，写子设备影子。

## 5. 预期日志

```
I (xxx) hall: DRV5032FB 初始化完成：OUT=GPIO1（推挽/全极/5 Hz，低电平=有磁）
I (xxx) t31sub: ==== bedrail 床护栏子设备（ESP32-C6/ZERO；温度+GNSS+霍尔）启动 ====
I (xxx) t31sub: 广播中 T31-xxxx（FFF0/FFF1）
I (xxx) t31sub: → notify 温度帧 #N：36.5x ℃（模拟）
I (xxx) t31sub: → notify TLV 帧 #N：0x57 hallState=0（打开/无磁）／0x56 gnssState=1（10 B）
I (xxx) t31sub: 霍尔状态变化 ⇒ hallState=1（闭合/有磁）
```

## 6. 配置（menuconfig → `bedrail/sensor 例程配置`）

| 项 | 默认 | 说明 |
| --- | --- | --- |
| 本板角色 | **SUBDEV** | 本工程默认/只构建子设备 |
| 温度推送周期 | 2000 ms | 温度/电量/GNSS/TLV 都按此节奏 |
| GNSS | 开（C6） | UART1 GPIO20/21/22 |
| 霍尔 GPIO | **1** | DRV5032 OUT |
| 低电平=有磁 | y | DRV5032FB 推挽/全极 |
| 机型 / 版本 | `OY-THE-01` / `0.1.0-bedrail-sensor` | 子设备复用体温贴机型 |

## 7. 已知限制

1. 温度仍是模拟值（与 thermometer-t31 相同），不是真实测量。
2. 广播名与 GATT 仍按 T31 复用；云端 `/shadow` 的 `model` 也会显示 `T31`。
3. `OY-THE-01` 的 `hallState` 是机型级属性：纯温感贴卡片会多一个可能长期为空的字段。
4. DRV5032FB 5 Hz 采样，200 ms 内的快速抖动可能被器件滤掉。
