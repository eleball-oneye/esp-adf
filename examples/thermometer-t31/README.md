# thermometer-t31 —— 网关 + 温感子设备双角色例程（oneye-link-sdk × oneye-dev-sdk）

> **本批（TH-20261023）把例程从「只跑 link-sdk 的最小 demo」升级为可双板实机验证的两角色固件**：
> 一块板做 **BLE 网关**（甲型一机一密 + App BLE 配网 + 上云上报影子属性），
> 一块板做 **T31 温感子设备模拟器**（BLE 外设，周期推温度帧）。
> 最终硬件方案会换成 **微雪 ESP32-C6-GEEK（网关）+ 健奥 T31（真温感贴）**；本批先在
> 两块 ESP32-S3（8 MB flash + 8 MB PSRAM）上验证，换板只改 `set-target` 与 profile。

## 1. 角色与拓扑

```
COM13  温感子设备（本批=B 板模拟 T31）          COM12  网关（本批=A 板）
  BLE 外设：广播 T31-xxxx / 服务 FFF0            BLE 中心（oneye-link-sdk）
  FFF1 notify 帧 A6 .. 6A（温度/电量）  ──BLE──▶  收帧解码 → shadow/up ──Wi-Fi/TLS──▶ mqtt.oneye.me:18886
  不连云、无云身份                                 甲型 creds 分区（0x510000）一机一密 + App BLE 配网
```

- 网关上云属性（`shadow/up` 的 `state`，与云端机型 `OY-GAW-01` 的 TSL 逐字一致）：
  `firmwareVersion` / `model` / `power` / `credSource`（**只报网关自身**；温度等子设备数据⛔ 不再塞进网关属性）
- 子设备是**独立实体**（独立 node_id/deviceId、在用户设备列表里独立成卡片）：网关代传
  `event/up` 的 `sub.online` / `sub.offline` / `sub.telemetry` / `sub.error`（`data.node` = 子设备 node_id），
  云端按**子设备机型**（`OY-THE-01`）的 TSL 校验后写子设备自己的影子/时序。
- 子设备自身**不**连云。

## 2. 构建（WSL + ESP-IDF 5.5.5）

```bash
source ~/esp/esp-idf-5.5.5/export.sh
export ADF_PATH=/mnt/e/workspace/rainmaker-oneye/embedded/esp-adf
cd $ADF_PATH/examples/thermometer-t31

# 两个角色一次构建（脚本内显式给 -DSDKCONFIG 与三份 defaults，IDF 5.5.5 不会自动叠加 .esp32s3）
bash tools/build-roles.sh both
```

等价的单角色命令：

```bash
# 网关（默认角色）
idf.py -B build-gw  -DSDKCONFIG=build-gw/sdkconfig  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32s3" set-target esp32s3
idf.py -B build-gw build

# 子设备模拟器
idf.py -B build-sub -DSDKCONFIG=build-sub/sdkconfig -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32s3;sdkconfig.defaults.subdev" set-target esp32s3
idf.py -B build-sub build
```

产物（实测）：网关 `0x149580` B（4 MB app 分区余 68%），子设备 `0x7f450` B（余 88%）。

> ⚠️ 两个角色**必须**用各自的 `-B` 与 `-DSDKCONFIG`，否则第二个角色会继承第一个的 sdkconfig（角色 Kconfig 不生效 ⇒ 编出另一个角色）。

## 3. 烧录

```powershell
# 应用（COM12=网关 / COM13=子设备）
python -m esptool --chip esp32s3 -p COM12 -b 460800 write_flash `
  0x0 build-gw\bootloader\bootloader.bin 0x8000 build-gw\partition_table\partition-table.bin `
  0x10000 build-gw\thermometer_t31.bin

python -m esptool --chip esp32s3 -p COM13 -b 460800 write_flash `
  0x0 build-sub\bootloader\bootloader.bin 0x8000 build-sub\partition_table\partition-table.bin `
  0x10000 build-sub\thermometer_t31.bin
```

### 3.1 甲型凭据（creds 分区，**每台一份、出厂不可变**）

`partitions.csv` 里 `creds` 固定 **`0x510000` / 16 KiB**（与 `korvo2_oneye` 逐行一致，⛔ 挪偏移=丢身份）。
产线写法（先由签发服务签出批量 ZIP，再用 `mkcreds` 展开为 `creds.bin`）：

```powershell
# 1) 批量签发 ZIP → 每台 creds.bin + creds-manifest.csv
mkcreds -zip <batch.zip> -out E:\prod\creds

# 2) 写入并回读校验（脚本会逐项校验 magic/版本/长度/CRC，写完回读逐字节比对）
powershell -ExecutionPolicy Bypass -File ..\oneye\korvo2_oneye\tools\provision-device.ps1 `
  -Port COM12 -Sn KORVO2-GAW01 -CredsDir E:\prod\creds -Offset 0x510000
```

- 网关**没有 creds 分区 → 不联网**（甲型 fail-closed，日志 `甲型无 creds 分区 → 不联网`）；
- 子设备**不需要** creds（本轮不连云）。

## 4. 配置（menuconfig → `thermometer-t31 例程配置`）

| 项 | 默认 | 说明 |
| --- | --- | --- |
| 本板角色 | 网关 | `网关（COM12）` / `温感子设备模拟器（COM13）` |
| 子设备温度推送周期 | 2000 ms | 子设备 notify 间隔 |
| 网关最小影子上报间隔 | 5000 ms | 收帧节流（同值不重报） |
| 云端主机 / 端口 | `mqtt.oneye.me` / `18886` | 设备面 TLS 监听器（⛔ 不是 8883） |
| 承载 | TLS | `mqtts` |
| 机型/身份 | `OY-GAW-01` / 占位 | **运行时以 creds 分区里的 node_id 为准**（占位仅日志用） |

## 5. 预期日志（网关）

```
I (xxx) t31: creds 分区读取成功 node=KORVO2-GAW01
I (xxx) oneye_link: oneye-link-sdk 初始化完成 node=KORVO2-GAW01 max_links=4
I (xxx) oneye_link.ble: NimBLE 同步完成，addr_type=0
I (xxx) t31: [上线] AA:BB:CC:DD:EE:FF model=T31 rssi=-58
I (xxx) t31: 收到温度 36.52 ℃（cmd=0x01）
I (xxx) oneye_link.up: shadow/up 已提交（temperature=36.52 battery=88 rssi=-58）
```

子设备（COM13）：

```
I (xxx) t31sub: 广播中 T31-3F24（FFF0/FFF1）
I (xxx) t31sub: 已连接，开始推送温度（2000 ms）
I (xxx) t31sub: notify 帧 0x01 len=6 temp=3652(0.01℃)
```

## 6. 重新配网

BOOT 长按 ≥3 s → 清 Wi-Fi 凭据 + `esp_restart()`（link-sdk 目前无 stop/deinit，
central 起来后本进程内回不到 peripheral；⛔ 不为此新增 SDK API）。

## 7. 已知限制（如实登记）

1. **真 T31 帧体编码未解出**：网关侧注册了「同 model 覆盖档案」按模拟器帧长解析
   （`0x01`=4B 温度+i16 序号、`0xB1`=3B 电量+mV，校验=从 cmd 起求和低 8 位）；
   接真机时删掉该覆盖档案、先看原始帧透传（`cmd=0x01` 的字段映射解出后只改 `profiles/t31.json`，**不改固件**）。
2. 子设备温度是**正弦模拟**（36.0–37.5 ℃），不是真实测温。
3. 初始影子 `battery=255`（未知哨兵）；若云端 TSL 写 0–100 并做范围校验，首帧可能被拒 → 必要时改 0。
4. 子设备镜像也链接了 oneye-dev-sdk（共用 `main/CMakeLists.txt`），体积 509 KB 而非最小。
5. 目标板仍是 ESP32-S3；C6-GEEK 适配（target/分区/PSRAM/NimBLE 配置）留待换板批。
