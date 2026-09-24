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

## 7. 第二批：③④与 broker（用户要求"③④都要修，broker 要适应稳定生产形态"）

### 7.1 ④ 已修并钉住（caps/up 的 model_version 类型漂移）

契约（域页《物模型与能力集》§2 / asyncapi `CapsUpData`）定的是 **int**，服务端按 int 反序列化；
而 SDK 公开 API 收字符串（`const char *model_version`）且**原样当字符串发** ⇒ 线上是带引号的 `"1"`
⇒ shadowd 整帧丢（`cannot unmarshal string into Go struct field CapsUpData.model_version of type int`）。
修法：**出线这一处**解析成十进制整数发数字（公开 API/ABI 不变，改结构体字段类型是破坏性变更）；
非十进制/溢出 ⇒ `ERR_INVALID` 且不发帧（宁可不上报，也不发类型不对的字段）。
单测：`caps_up_frame` 改为断言数字（原断言带引号 —— 等于把 bug 写进测试），
新增 `caps_model_version_must_be_numeric`（`v2` ⇒ INVALID 且不入队；`0007` ⇒ 发 `7`）。
host 单测 **21 组 / 250 用例 / 断言 3568 次 / 失败 0**。

### 7.2 ③ 的主因已修，但还有第二层（未闭环）

**主因（已修）**：固件拼影子的 JSON 时只去掉了 SDK 片段**开头**的 `{`，结尾的 `}` 还留着 ⇒
`{"esp.fw_version":"0.1.0","esp.power":true,"esp.cred_source":"partition"}}`（**多一个花括号**）
⇒ 不是合法 JSON ⇒ `report_state` 返回 INVALID ⇒ **平台侧从来没有影子文档**。
它藏这么久，是因为调用处是 `(void)…` + 一句无条件的"凭据来源已上报"：
**失败与成功在串口上长得一模一样**。现已：看返回值 + 失败重试一次 + 打印错误码与**实际发出的那一串**。

**第二层（未闭环，已定位到 SDK）**：修完 JSON 后串口说"影子上报成功"，但线上仍看不到 `shadow/up`。
用 EMQX topic trace + 固件侧 `[cloud-stats]` 拿到的事实：
- `[cloud-stats] stats_rc=0 link_rc=0 tx_frames=0 rx_frames=0 dropped=0 retries=0 cloud_link_up=1`
  ⇒ SDK 认为**一帧都没发出去**，也没有丢帧 ⇒ 落在"保留该帧、下一轮重试"那条路径
  （`oneye_internal.c` 的 `bint_drain_tx`：`oneye_mqtt_publish` 返回 -7 时 `held_net` 不释放）。
- 100 s 窗口内线上只有 `caps/up`×3、`log/up`×1、`status/up`×1，**没有 `shadow/up`**；
  设备发过 PINGREQ（写路径活着）、20 个 PUBLISH 出去、只收回 10 个 PUBACK。
- 怀疑点（下一步按这个查）：`oneye_mqtt.c` 的 **slot/inflight 窗口** —— `mqtt_slot_reclaim` 是
  **严格 FIFO**，`mqtt_flush_tx` 在 `inflight >= max_inflight(=8)` 时对 QoS1 **停止写出**；
  PUBACK 若个别没匹配上（或没被 poll 消费），未确认槽会一直占着窗口，后面的 QoS1 帧
  （正是 shadow/up）永远写不出去，而**控制报文不受影响**（所以订阅照常、看起来"连接正常"）。
- ⚠️ 排查期的假线索：短窗口（40 s）看不到 PUBLISH，一度以为"设备完全不发"；拉到 90–100 s 才看到
  周期性上行。**判"发没发"要看足够长的窗口或 topic trace。**

### 7.3 ⑤ broker 已收成稳定生产形态

`backend/deployment/broker/`：一台 `oneye-broker`、两个监听器、两个固定宿主端口 ——
`127.0.0.1:1883`（明文，**只绑回环**：shadowd/申领服务/运维）与 `0.0.0.0:18886`（mTLS，平台 CA，
`peer_cert_as_username=cn`）。于是 `EMQX_URL=tcp://127.0.0.1:1883` 重新成为**稳定**值，
不再依赖 docker 网桥 IP（上一批就是用它顶的，而它会随容器重建变化）。历史四台实验容器只 `stop` 不 `rm`。
`deploy-broker.sh` 自检 3/3：平台 CA 证书被接受、无证书被拒、1883 只绑回环。

ACL 从历史的 `{allow, all}` 收成"未认证一律拒绝"，但 ⚠️ **按设备隔离 topic 没做成**（实测）：
EMQX 5.8.6 的**文件型 ACL 里 `%u` 与 `${username}` 占位符都不展开**（三种写法都试了；
只有把 node 写死才有效，不可扩展）。当前强度 = "必须持 CA 签发的证书"，**不等于**"只能访问自己的面"。

⚠️ 顺带查出一个**未修的生产级缺口**：**broker 重启后 shadowd 不重新订阅**
（`clients list` 显示 `subscriptions=0`，设备上行全部丢失且无任何告警，必须重启 shadowd 才恢复）。

### 7.4 还踩到一个"更贵的坑"：预编译库与源码脱节

现象：改了 SDK 源码、重新构建固件，**设备行为却完全不变**（串口没有我新加的诊断）。
机制：`components/oneye-dev-sdk/CMakeLists.txt` 在 ESP-IDF 路径下，只要
`lib/<toolchain-id>/liboneye_dev_base.a` 存在就**只链接预编译归档、根本不编译 `src/`**；
而原来的"版本哨兵"只比对 `toolchain.json` 的 `sdk_version` 与头文件版本字符串 ——
**改了源码没改版本号，哨兵照样通过**。代价：当天为此白烧三轮板子（真机与源码不符、主机单测全绿，
因为宿主轨是从源码构建的）。修法：新增 `cmake/sdk_src_hash.cmake` 对参与编译的源取**内容哈希**
（用文件名而非绝对路径，可跨目录/worktree），`write_manifest.cmake` 写入 `src_sha256`，
消费侧比对不一致即 FATAL 并给出重生成命令；旧格式清单只告警不误判。
**改 SDK 源码后的正确顺序**：`./build-all.sh --toolchains esp32s3@5.5.5`（重生成归档）→ 再构建固件。

### 7.5 一条工具经验（我自己的，写下来免得重犯）

**不要用 PowerShell 5.1 的 `Get-Content -Raw` / `Set-Content` 往返改含中文的 UTF-8 文件**：
它按系统本地代码页（GBK）读、再以 UTF-8 写回 ⇒ 中文全变乱码（本次把 digest 弄坏一次，
靠 `git checkout` 恢复）。改文件用编辑工具（按 UTF-8 处理），或用 git 自身的命令。

## 8. 证据入口

- 交付物：`E:\tmp\prd-test\claim-batch-B-20260923-192813\{README.md,manifest.csv,batch.json,devices/KORVO2-0000/}`
- 镜像：`E:\tmp\prd-test\creds-B-20260923-192813\`（含 `creds-manifest.csv`）
- 留档：`E:\tmp\prd-test\provision-log.csv`（PASS 一行）
- 固件：`output/.build/korvo2_oneye-platca18886/`（台面验证用：量产预设 + 端口 18886 + Wi-Fi 文件开）
  与 `/tmp/oneye-prodcheck/`（**纯量产预设**，门禁跑在这份上）
- 串口：`E:\workspace\board-online.log`
- 门禁：`check-sdkconfig-defaults.py --require sdkconfig.defaults.production` ⇒ **PASS**（11/11 + 32 + 14）

## 9. 遗留（下一轮可做）

1. **③ 的第二层 = 设备侧 shadow/up 上不了线**（§7.2）——这是"控制台看得见设备"的唯一卡点。
   已定位到 SDK 的 MQTT slot/inflight 窗口（`oneye_mqtt.c`：严格 FIFO 回收 +
   `inflight >= max_inflight` 时对 QoS1 停止写出），下一步按"PUBACK 是否个别没匹配/没被 poll 消费"
   查，并在 `bint_drain_tx` 的 `-7` 路径上加可观测性（现在它只保留帧、不留话）。
2. **broker 重启后 shadowd 不重新订阅**（§7.3）——生产形态下必须先修，否则任何 broker 重启都会
   静默丢掉全部设备上行。
3. **按设备隔离 topic 未实现**（§7.3）：EMQX 5.8.6 文件 ACL 不支持占位符；要真隔离得换
   内置库授权 / HTTP 授权源（或加一层网关）。
4. 台面验证固件与量产固件的差异（Wi-Fi 文件开关）应由 `build-all.sh` 的一个显式档位产出，
   而不是每次手写 defaults；顺带把"固件里到底装了哪些模块"变成可查询的事实。

1. **设备侧 `shadow/up` 没发出来** —— 这是"控制台看得见设备"的唯一卡点（问题 3），要先在 SDK 里
   把 `report_state` 的返回值暴露出来（别 `(void)`），再定位是 guard/队列/payload 哪一步失败。
2. `caps/up` 的 `model_version` 类型漂移（问题 4）：设备发 string、服务端要 int，二选一改齐 + 加契约断言。
3. shadowd 生产形态：一台 broker（平台 CA 的 mTLS 监听 + 内部明文监听），别再用 docker 网桥 IP 指 broker。
4. 台面验证固件与量产固件的差异（Wi-Fi 文件开关）应当由 `build-all.sh` 的一个显式档位产出，而不是手写 defaults。
