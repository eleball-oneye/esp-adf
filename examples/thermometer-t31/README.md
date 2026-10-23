# thermometer-t31 —— 微雪 ESP32-C6-GEEK 接入健奥 T31 体温贴

基于 [`oneye-link-sdk`](../../components/oneye-link-sdk) 的最小可跑例程：
网关做 BLE Central 连接 T31，按档案解析（电量）或原始帧透传（温度待解），
经 Wi-Fi/MQTT 上报 Oneye 云（`rmng/dev/{node}/event/up`）。

## 1. 硬件

- 微雪 **ESP32-C6-GEEK**（USB-A 供电，5V/1A 适配器或电脑 USB）
- 健奥 **T31** 无线体温贴 1~4 片（CR2025 电池）
- 可选：一个 MQTT Broker（本机 EMQX 即可）

> 测试前**关闭手机上的健奥云 / 测温宝 App**：T31 大概率只接受单连接，
> 手机占着时网关连不上，会被误判为"协议问题"。

## 2. 构建

```bash
# 1) 导出 IDF（按本机路径）
source ~/esp/esp-idf/export.sh          # Windows PowerShell: . $HOME\esp\esp-idf\export.ps1

# 2) 组件路径（若未导出 ADF_PATH，CMake 会按相对路径找）
export ADF_PATH=<repo>/embedded/esp-adf

# 3) 构建烧录
cd <repo>/embedded/esp-adf/examples/thermometer-t31
idf.py set-target esp32c6
idf.py build flash monitor
```

## 3. 配置（menuconfig）

```bash
idf.py menuconfig
# thermometer-t31 例程配置
#   ├─ 网关节点 ID              (默认 OE-GW-C6-0001)
#   ├─ 并发连接的 T31 数量      (默认 4)
#   ├─ Wi-Fi SSID / 密码        (留空 = 仅本地打印，不联网)
#   ├─ MQTT Broker URI          (默认 mqtt://192.168.1.10:1883)
#   └─ 未命中档案也接入         (默认 y)
```

## 4. 预期日志

```
I (1234) oneye_link: oneye-link-sdk 0.1.0 初始化完成 node=OE-GW-C6-0001 max_links=4
I (1250) oneye_link.prof: 档案已注册: T31（notify=fff1 write=fff2）
I (2010) oneye_link.ble: NimBLE 同步完成，addr_type=0
I (5200) t31: [上线] AA:BB:CC:DD:EE:FF model=T31 rssi=-58
I (5230) t31: [数据] AA:BB:CC:DD:EE:FF cmd=0x01 raw[11]
I (5260) t31: [数据] AA:BB:CC:DD:EE:FF cmd=0xB1 battery=88% batt_mv=2900mV raw[11]
I (30230) t31: 统计: 在线=1 已知=1 收帧=312 解析=52 透传=260 …
```

- `cmd=0x01` 目前只透传（温度编码未解）；`cmd=0xB1` 已能解析电量/电压。
- 未配 Wi-Fi 时只有本地日志；配了 MQTT 会同时上云。

## 5. 云端报文样例

```json
{"v":1,"id":"…","seq":42,"ts":1761177600000,"type":"sub.telemetry",
 "data":{"sub":"AA:BB:CC:DD:EE:FF","model":"T31","rssi":-58,"cmd":"01",
         "tlv":"800BE4A601390A7923D4EF9FA86A"}}
```

`tlv` 里的 `0x80` 段就是 T31 原始帧（`A6 01 … 6A`），云端可先用它解析温度。

## 6. 解出温度后怎么改

只改档案，**不改固件**：

```json
"commands": [
  { "cmd": "01", "telemetry": true,
    "fields": [ {"name":"temp","type":"i16le","offset":0,"scale":0.01,"tlv":16} ] }
]
```

- 编译期内置：改 `components/oneye-link-sdk/profiles/t31.json` 重新编译；
- 运行期下发：云端发 `event/down`，`type=sub.profile`（见 SDK `docs/PROTOCOL.md` §5）。

## 7. 排障

| 现象 | 排查 |
| --- | --- |
| 一直扫不到 T31 | 贴片是否在测温/广播？手机 App 是否占着连接？距离 <10 m？ |
| 连上但收不到数据 | 是否订阅到 FFF1？看 `找到 notify 特征` 日志；CCCD 是否写入成功 |
| 数据全是 `raw[...]` | 正常（温度未解）；电量帧应能解析出 battery |
| MQTT 连不上 | 先只配 Wi-Fi 验证网络；broker URI、账号密码；`cloud_connected` 统计 |
| 构建报 NimBLE 字段错误 | 按 SDK `src/oneye_link_ble.c` 的 `chr_uuid_of()` 注释调整一处 |
