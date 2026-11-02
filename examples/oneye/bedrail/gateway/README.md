# bedrail/gateway —— 床护栏网关（ESP32-C6-GEEK，COM3）

> 复制自 `examples/thermometer-t31` 的 **GATEWAY** 角色。差异：
> ① 默认目标 C6-GEEK（`sdkconfig.defaults.esp32c6[.mem]`）；
> ② `main/gw_child.c` 允许子设备 `cmd=0x03` 通用 TLV **单独**成为一条 `sub.telemetry`
>    （旧逻辑只缓存等温度帧；完整复制的子设备仍会发温度，但这样霍尔/gnssState 不必等
>    温度帧，且未来"只发霍尔"的机型也能用）；
> ③ 身份/版本默认值改为 `BEDRAIL-GAW01` / `0.1.0-bedrail-gw`；
> ④ `ONEYE_T31_GW_SUB_TELE_MIN_MS` 默认 5000 → **0（不节流）**：完整复制的子设备每周期
>    发 `0x03`（gnssState+hallState）和 `0x01`（温度）两帧；1000 ms 窗口会把后到的温度帧
>    恒定丢弃。0 = 两帧都上报（约 1 条/s/子设备）。

## 1. 角色

- 甲型 `OY-GAW-01`：creds 分区一机一密 + App BLE 配网 + Wi-Fi 上云（影子/status）；
- 释放 NimBLE 后由 `oneye-link-sdk` 重建为 BLE **中心**，扫描/连接 `T31-xxxx` 子设备；
- 子设备事件 → `event/up` 的 `sub.online/offline/telemetry/error`（`data.node` = 子设备 node_id）；
- 网关自身影子只含 `firmwareVersion/model/power(+credSource)`，⛔ 不塞子设备数据。

## 2. 构建

```bash
source ~/esp/esp-idf-5.5.5/export.sh
export ADF_PATH=/mnt/e/workspace/rainmaker-oneye/embedded/esp-adf
cd $ADF_PATH/examples/oneye/bedrail/gateway
bash tools/build-gateway.sh
```

等价的单步命令：

```bash
idf.py -B build-gw-c6 -DSDKCONFIG=build-gw-c6/sdkconfig \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32c6;sdkconfig.defaults.esp32c6.mem" \
  set-target esp32c6
idf.py -B build-gw-c6 build
```

产物（Windows 实测 2026-11-02）：`build-gw-c6/bedrail_gateway.bin` = **0x1A25F0 B**
（4 MB app 分区余 59%）；bootloader `0x56A0 B`。

## 3. 烧录

```powershell
python -m esptool --chip esp32c6 -p COM3 -b 460800 write_flash `
  0x0 build-gw-c6\bootloader\bootloader.bin `
  0x8000 build-gw-c6\partition_table\partition-table.bin `
  0x10000 build-gw-c6\bedrail_gateway.bin
```

### 3.1 甲型 creds（每台一份，偏移固定）

`partitions.csv` 的 `creds` 固定在 **0x510000 / 16 KiB**，⛔ 不可挪。产线用
`mkcreds` 展开批量 ZIP 后，用 `provision-device.ps1` 写入并回读校验
（与 thermometer-t31/korvo2_oneye 同一套工具）。网关**没有 creds / creds 损坏 ⇒ fail-closed 不联网**。

## 4. 配置（menuconfig → `bedrail/gateway 例程配置`）

| 项 | 默认 | 说明 |
| --- | --- | --- |
| 本板角色 | 网关 | 本工程只构建 GATEWAY |
| 子设备遥测最小上报间隔 | **0（不节流）** | 每子设备每周期 0x03+0x01 两帧 ≈ 1 条/s |
| 云端主机 / 端口 | `mqtt.oneye.me` / `18886` | 设备面 TLS |
| 机型 / 版本 | `OY-GAW-01` / `0.1.0-bedrail-gw` | node_id 运行时以 creds 为准 |

## 5. 预期日志

```
I (xxx) t31gw: creds 分区读取成功 node=C6-GAW01
I (xxx) oneye_link: oneye-link-sdk 初始化完成 node=C6-GAW01 max_links=4
I (xxx) t31gw: [子设备] ... model=T31 rssi=-52
I (xxx) oneye_link.up: event/up 已提交（sub.telemetry cmd=0x03 tlv=…570101）
```

`tlv` 里会同时带链路层 `RAW_FRAME(0x80)` 与通用 TLV `57 01 01`：
`0x57` = 霍尔标签、`0x01` = 长度、`0x01` = 有磁/护栏闭合（云端只解 0x57，0x80 按契约忽略）。

## 6. 已知限制

- 仍然最多 5 条 BLE 链路；床护栏单子设备场景足够。
- 网关 LCD 卡片 C：设备名固定显示 **SUB**，原电量百分数位置改为霍尔状态
  **On**（有磁/闭合）/ **Off**（无磁/打开）/ **--**（未知）；温度/Wi-Fi 仍照常显示。
  温度是子设备上报的模拟值，不是网关自身测量，不影响上云与子设备代传。
- `sub.telemetry` 的 `cmd` 可能是 `0x01`（温度，TLV 已并 GPS + 霍尔 + 电量）或 `0x03`（gnssState+hallState 直出）；云端不依赖 `cmd`，只解 TLV。
- 2026-11-02 起网关做**同周期聚合**：`0x02` GNSS 只缓存并入温度帧；`0x03` 在最近 5 s 有温度帧时不再单独发；`0xB1` 电量缓存后并入下一帧温度。正常连接时子设备约 **1 条 event/up : 2 s**（温度帧），电量/霍尔/GNSS 都在同一 TLV 里；只有“无温度帧”的机型才让 `0x03`/`0xB1` 独立上报。
