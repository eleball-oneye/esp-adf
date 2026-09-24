# 把一批签发凭证做到真机上线（2026-09-24）

## 1. 这次要解决什么

用户给了一个真实交付物 `claim-batch-B-20260923-192813`（1 台 `KORVO2-0000`），要求：
① 把这份证书应用到串口开发板上并**验证上线云平台**；② 说清楚经过了哪些流程；
③ **以后在 claim-batch 产物目录里带一份能给产线人员看的操作指南**。

第 ③ 条不是"再写一篇文档"——用户要的是**交付物自带指南**（产线拿到的是 ZIP，
常常已经被解压转存，操作它的人未必有权限翻文档仓）。

## 2. 结论（一句话）

**证书确实把设备带上了平台**：门卫 `Client(KORVO2-0000, username=KORVO2-0000, connected=true)`、
平台应答它（`caps/down` 授时 + `log/down` ack）、设备侧 `esp.cred_source=partition` 且自证串指纹与清单一致。
**但控制台的节点页/身份视图看不到它** —— 卡在设备侧根本没发 `shadow/up`（§4 第 3 条）。
另外查出并修掉了两个会让"整批产品连不上云"的真问题（预设端口、shadowd 的 broker）。

## 3. 实际走过的 8 步（详见作业指导 §13.1）

1. 取交付物 → 2. `mkcreds` 出镜像（16384 B，CRC32 `cac3238f`）→ 3. **先验证端点选对**
（拿本批证书对每个 mTLS 监听器跑 `openssl s_client`）→ 4. `provision-device.ps1` 烧固件+凭证
（退出码 0，逐段 `Hash of data verified`，回读逐字节一致）→ 5. 串口看它自认身份
（`creds 分区 … node=KORVO2-0000`、`ONEYE-PROV1 … cert_fp=56c4e7aa…`、`esp.cred_source=partition`）
→ 6. 看上线（`link up: mqtt.oneye.me:18886 transport=mqtt-tls node=KORVO2-0000` + 门卫侧 client）
→ 7. 看平台应答（`time_sync → …/caps/down`、`ack → …/log/down code=ok`）→ 8. 核对 `device_certs` 台账行。

## 4. 这轮暴露的四个真问题

| # | 问题 | 处置 |
| --- | --- | --- |
| 1 | **量产预设钉错端口**：`sdkconfig.defaults.production` 钉 `18885`，那是 `oneye-crl`（老 CA）；平台 CA 签发的证书只有 `oneye-platca`（**18886**）认 | ✅ **已修**（18885→18886 + 注释写清"端口必须与信任签发 CA 的监听器一致"）；门禁 `--require` **11/11 生效** |
| 2 | **shadowd 与设备不在同一台 broker**：shadowd 订阅 `127.0.0.1:1883`（`oneye-emqx`），设备连平台 CA 那台 ⇒ 平台永远看不到设备 | ✅ **部署侧已修**（`EMQX_URL` 指到平台 CA 那台）；生产形态应"一台 broker 两个监听器" |
| 3 | **设备根本没发 `shadow/up`**：固件调了 `oneye_dev_base_report_state()` 并打印"已上报"，但 broker 上一帧都没有（原始报文十六进制查 `shadow` = 0 次）⇒ 无影子文档 ⇒ 身份视图/节点页看不到它 | ⚠️ **未修，已登记**（SDK 返回值被 `(void)` 吞掉，"失败"与"成功"在串口上长得一样） |
| 4 | **`caps/up` 的 `model_version` 类型漂移**：设备发字符串、shadowd 要 int ⇒ 整帧被判无效丢弃（授时不受影响） | ⚠️ **未修，已登记** |

## 5. 交付物自带指南（用户第 ③ 条）

- 新包 `backend/src/claim/batchguide`：模板 + 本批参数填充（批号/台数/逐台 SN·serial/CA 名/端点/偏移）。
  **CA 名是从本批证书的 Issuer 解出来的**，不是查表；端点走 `BATCH_GUIDE_CLOUD_ENDPOINT`（缺省 `mqtt.oneye.me:18886`），
  因为它是**部署事实**而不是签发事实。
- 申领服务组装 ZIP 时写入 `README.md`（`claim_batch.go` 的 `buildBatchZip`）。
- `backend/src/tools/batchguide`：给**已下发过**的批次补一份（`-zip` / `-dir`），与前者同一套模板。
- 契约 #21 §4 与作业指导 §1.2 已登记；验收脚本 `claim-console-acceptance.sh` 断言
  ZIP 里**有** `README.md` 且含本批 SN、端点、偏移、`provision-device.ps1` 步骤。
- 实测：`--start` ⇒ **PASS 14 · FAIL 0**；本批交付目录已补上 `README.md`（5823 B）。

## 6. 附带修掉的第五个问题（同类：写了不生效的配置）

`examples/oneye/korvo2_oneye/sdkconfig.defaults` 里的 `CONFIG_LOG_MAXIMUM_LEVEL_INFO=y` **什么都不做**：
IDF 的 `LOG_MAXIMUM_LEVEL_INFO` 带 `depends on LOG_DEFAULT_LEVEL < 3`，而上一行已把缺省设成 INFO(=3)
⇒ 成员不可选、被静默丢弃，choice 落回 `LOG_MAXIMUM_EQUALS_DEFAULT`（效果恰好一样，所以一直没人发现）。
已改成显式钉 `CONFIG_LOG_MAXIMUM_EQUALS_DEFAULT=y` 并写明原因。**门禁从"1 项 FAIL"变成全绿**。

## 7. 证据入口

- 交付物：`E:\tmp\prd-test\claim-batch-B-20260923-192813\{README.md,manifest.csv,batch.json,devices/KORVO2-0000/}`
- 镜像：`E:\tmp\prd-test\creds-B-20260923-192813\`（含 `creds-manifest.csv`）
- 留档：`E:\tmp\prd-test\provision-log.csv`（PASS 一行）
- 固件：`output/.build/korvo2_oneye-platca18886/`（台面验证用：量产预设 + 端口 18886 + Wi-Fi 文件开）
  与 `/tmp/oneye-prodcheck/`（**纯量产预设**，门禁跑在这份上）
- 串口：`E:\workspace\board-online.log`
- 门禁：`check-sdkconfig-defaults.py --require sdkconfig.defaults.production` ⇒ **PASS**（11/11 + 32 + 14）

## 8. 遗留（下一轮可做）

1. **设备侧 `shadow/up` 没发出来** —— 这是"控制台看得见设备"的唯一卡点（问题 3），要先在 SDK 里
   把 `report_state` 的返回值暴露出来（别 `(void)`），再定位是 guard/队列/payload 哪一步失败。
2. `caps/up` 的 `model_version` 类型漂移（问题 4）：设备发 string、服务端要 int，二选一改齐 + 加契约断言。
3. shadowd 生产形态：一台 broker（平台 CA 的 mTLS 监听 + 内部明文监听），别再用 docker 网桥 IP 指 broker。
4. 台面验证固件与量产固件的差异（Wi-Fi 文件开关）应当由 `build-all.sh` 的一个显式档位产出，而不是手写 defaults。
