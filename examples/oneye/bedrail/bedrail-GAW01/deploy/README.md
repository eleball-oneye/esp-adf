# bedrail-GAW01 · 这块 S3-Zero 的签发/录入清单

## 0. 这块板子的实测身份（esptool，2026-11-18）

```text
端口     COM9
芯片     ESP32-S3 (QFN56) rev v0.2
MAC      a0:f2:62:ea:6a:ec
Flash    4 MB
```

> ⚠️ `COM4` 上那块不是 C3，esptool 读出 `ESP32-C6FH8 / 8 MB`；C3 产物不能烧 COM4。
> 本工程同时支持 `esp32s3` / `esp32c3` 两种 target。

## 1. 两条量产通道（别混）

签发服务的运维页「**CSV → 签证 → 导入云（provision 覆盖导入）**」走的是**量产路径**，
但它与「申领路径」是两条不同通道：

| 通道 | 私钥在哪 | 入口 | 适用 |
| --- | --- | --- | --- |
| **本页（服务端现生成密钥）** | 只在服务**内存**（30 分钟），仅出现在勾选二次确认后下载的 `keys.zip` | UI 上传 CSV → `handleImportCloud` → `opsprovision.Provision` | 产线灌注（本板走这条） |
| 申领路径（设备持私钥） | **设备自己生成**，云上不留 | `POST /v1/sign-pub {node_id, public_key_pem}` | 设备先有密钥对的场景 |
| 按序批量（prefix/count） | 服务生成 | `POST /v1/batch {"prefix":…,"count":…}` | 只想要一堆 node_id + SN 的场景 |

## 2. 本页对 CSV 的硬规则（逐条取自页面与实现）

| 列 | 规则 |
| --- | --- |
| 品类 `productKey` | **必填** |
| 固件版本 `firmwareVersion` | **必填** |
| `node` / 设备激活码（= `node_id`） | **必填**，且**必须与设备码不同** |
| `设备码`（deviceSn） | **可留空** ⇒ 服务**现铸 v3 SN**（24 字符：`SN-3-<4位编号>-<11位Crockford>-<2位校验>`，即蓝牙广播 `sn_field` 的 24 字节），并把铸出的串**回写**结果 CSV 的 `deviceSn` 列与交付包目录 `devices/<SN>/`；填了就按 `strict_sn` 判 |
| `证书序列号` | ⛔ **不能指定** —— 本服务签的序列号由 CA 现铸；填了该行失败（要用外部证书请走 `/v1/import-cloud`） |
| `设备公钥` | 留空 ⇒ 服务**现生成密钥对**（私钥只进 `keys.zip`）；填了 ⇒ 走「签我给的公钥」，服务**不产生私钥** |
| `厂商码` | 可选；留空 ⇒ 请求里固定填 `ONEYE`（回执 `vendor_code_source=default`）；显式填则原样透传并标 `explicit`，≤32 字节 |
| `厂商sn` | 可选；非空才透传给云侧 `vendorSn`（≤64 字节）；⛔ 不回显 SN 值 |
| 设备编码 / MAC / 外部* | 原样忽略（忽略的列名会列在结果上方） |
| 批次号 | 会随 provision 带上 |

页面上的两个勾选：`干跑(dry_run)`（只签发 + 组包，不发云）、`覆盖已有设备`（按设备码 upsert）、
`strict_sn`（填了设备码时必须是合法 v1/v2/v3 SN，否则该行失败）。逐行失败**不阻断**后面的行。

## 3. 本板的 CSV（`s3-zero-a0f262ea6aec.csv`）

```csv
租户id,品类,设备名称,设备码,设备编码,设备激活码,mac地址,批次号,固件版本,外部平台代码,外部设备标识,外部产品,node,厂商sn,厂商码,证书序列号,设备公钥
,OY-GAW-01,床护栏网关-S3Zero-01,,,GAW01-6AEC,a0:f2:62:ea:6a:ec,S3ZERO-20261118,0.1.0-bedrail-gaw01,,,,GAW01-6AEC,,ONEYE,,
```

- `设备激活码` / `node` = **GAW01-6AEC**（两列同值，`node_id` 口径）
- `设备码`、`证书序列号`、`设备公钥` **全部留空**：
  - 设备码留空 ⇒ 由服务现铸 v3 SN 并回写；
  - 证书序列号必须空（填了该行失败）；
  - 设备公钥留空 ⇒ 服务生成密钥对，私钥只在 `keys.zip`（**下载后立即转存受控介质**）。

## 4. 操作步骤

```text
① 页面上传本 CSV，先勾 dry_run 跑一遍（只签发+组包，不动云）→ 看逐行结果与铸出的 deviceSn
② 确认无误后取消 dry_run，勾「覆盖已有设备」，strict_sn 不勾（设备码空 ⇒ 由服务铸）
③ 下载 keys.zip（需二次确认）→ 转存受控介质；把回写后的 CSV 存档
④ 把 devices/<SN>/client.crt + ca.crt + key.pem 按 creds 分区格式烧到板子 creds@0x300000
⑤ idf.py -p COM9 -b 460800 flash monitor 验证上云
```

⚠️ 产测/凭据烧写脚本若按偏移直写，**必须用本工程的 0x300000**（不是 korvo2/C6 的 0x510000）。

## 5. SN 与蓝牙广播的关系（结论：**无关，不需要广播 SN**）

签发页写「v3 SN 的 24 字符**恰好等于**蓝牙广播 `sn_field` 的 24 字节」，那是**长度兼容性说明**，
不是对本仓设备的协议要求。依据三条：

1. 设备认证/标识走的是 **creds 分区的一机一密（证书 + 私钥 + CA）+ `node_id`**，与 BLE 广播无关；
   云端认的是 `device_sn` / 证书，不认广播内容（`signimport.go:612`、`:638` 也只是在说"不需要截断"）；
2. 本仓协议**已经把 `sn_field` 那 24 字节征用掉了**：`BLE-ADVERTISING-PROTOCOL.md:16`
   「厂商 AD 已经占满 31 B，因此不新增 scan response，直接定义 v4 layout，把原来的 `sn_field` 24 B
   征用为 telemetry」；`:66` 明确 `off 1  24 B  telemetry（原 sn_field）`；
3. v5 现行广播里只有 `DEVICE_STATE / SEQ / MODEL_ID / FW_VERSION / BATTERY_PCT / TEMP / HALL`，
   身份仍是 **BLE MAC**（TLV-REGISTRY：「`MODEL_ID` 只做一致性校验，不替代 `MAC/node_id` 的设备身份」）。

所以：**本页铸出的 SN 只用于云端台账 + 网关凭据，不进二进制广播**；传感器侧不需要也不应该加 SN 字段。
（只有将来要做"无云本地配对、用广播带 SN"这种新场景，才需要重新定义广播字段 —— 那时也优先考虑
scan response 或扩展广播，而不是挤掉 telemetry。）

