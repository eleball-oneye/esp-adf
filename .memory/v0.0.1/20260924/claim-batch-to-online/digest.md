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

### 7.2 ③ 已闭环（两层都修了）

**第一层（已修）：影子 JSON 多一个花括号。** 固件拼 JSON 时只去掉了 SDK 片段**开头**的 `{`，
结尾的 `}` 还留着 ⇒ `…"partition"}}` ⇒ 不是合法 JSON ⇒ `report_state` 返回 INVALID
⇒ **平台侧从来没有影子文档**。它藏这么久，是因为调用处是 `(void)…` + 一句无条件的
"凭据来源已上报"：**失败与成功在串口上长得一模一样**。

**第二层（已修）：影子帧在队列里过期被静默清掉。** 修完 JSON 后串口说成功、平台仍看不到。
本轮新增的三条诊断（`[cloud-stats]` / `tx progress` / `tx expired`）给出事实：
- `tx progress: app_tx=0 app_dropped=0 | mqtt_pub=0 … | txq=0/16384 slots=0/16 inflight=0/8`
  ⇒ 上层一帧都没发出去，而 **MQTT 层根本没被占满** ⇒ 先前"slot/inflight 窗口被占满"的怀疑
  **被数据否定**（这是本轮最有价值的一次纠错：差一点就去修错的地方）。
- `tx expired: face=1 expired=1 ttl=30000` ⇒ 影子帧 7.4 s 入队，第一次真正的 drain 到 ~80 s 才发生
  （链路建立后那段被订阅等串行动作占着），帧躺过 30 s TTL 被**静默清掉**。

**两侧修法**：
- SDK：TTL 只度量"链路可用期间的等待" —— 新增 `oneye_queue_refresh_enqueued()`，
  链路恢复时刷新已入队帧的入队时刻（断链不是设备的错，重连后仍应上报）。
- 固件：影子状态 **每 60 s 周期重述**（`esp.cred_source` 是**状态**不是事件，一次丢了不该永远丢）。

**闭环证据（真板 Korvo-2 / COM12）**：`GET /v1/devices` 含 `KORVO2-0000`；
`GET /v1/devices/KORVO2-0000/shadow` = **200** 且 `reported.esp.cred_source=partition`；
身份视图 `identity=partition`（`basis=reported`）；shadowd 日志
`device KORVO2-0000 credential identity: partition`；串口 `app_tx=6 mqtt_pub=6`。

**留档的诊断（下次同类问题靠它们，不用再烧板子猜）**：`oneye_mqtt_tx_snapshot()`（txq/slots/inflight）、
`tx progress`（app 与 MQTT 两层计数并排）、三条以前只涨计数不留话的路径（`-7` 保留重试 /
其它错误丢弃 / **TTL 过期静默清掉**）全部打日志且限频。

### 7.3 ⑤ broker 已收成稳定生产形态

`backend/deployment/broker/`：一台 `oneye-broker`、两个监听器、两个固定宿主端口 ——
`127.0.0.1:1883`（明文，**只绑回环**：shadowd/申领服务/运维）与 `0.0.0.0:18886`（mTLS，平台 CA，
`peer_cert_as_username=cn`）。于是 `EMQX_URL=tcp://127.0.0.1:1883` 重新成为**稳定**值，
不再依赖 docker 网桥 IP（上一批就是用它顶的，而它会随容器重建变化）。历史四台实验容器只 `stop` 不 `rm`。
`deploy-broker.sh` 自检 3/3：平台 CA 证书被接受、无证书被拒、1883 只绑回环。

ACL 从历史的 `{allow, all}` 收成"未认证一律拒绝"，但当时 ⚠️ **按设备隔离 topic 没做成**（实测）：
EMQX 5.8.6 的**文件型 ACL 里 `%u` 与 `${username}` 占位符都不展开**（三种写法都试了；
只有把 node 写死才有效，不可扩展）。当时强度 = "必须持 CA 签发的证书"，**不等于**"只能访问自己的面"。
→ **当晚已补做，见 §10.3**（换授权源而不是继续在文件里想办法）。

⚠️ 顺带查出一个**未修的生产级缺口**：**broker 重启后 shadowd 不重新订阅**
（`clients list` 显示 `subscriptions=0`，设备上行全部丢失且无任何告警，必须重启 shadowd 才恢复）。
→ **当晚已修，见 §10.2**。

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
- broker 落点（§10）：`/home/ubuntu/oneye-broker/{docker-compose.yml, acl.conf, sync-authz.sh, api-keys.txt}`
  （`api-keys.txt` = 管理 API 密钥，0640 root:1000，**不入库**）；隔离验收探针 `/tmp/mqttprobe`（linux/amd64）
  + 设备证书 `/tmp/probe-cr/{ca.crt,client.crt,client.key}`（取自本批交付物 `devices/KORVO2-0000/`）

## 9. 遗留（下一轮可做）

1. **链路建立后那一段为什么把 drain 挡住 ~70 s**（§7.2 的间接原因，未深挖）：订阅是串行等待的
   （`oneye_mqtt_subscribe(..., 2000)` × 每面一条），实测每条约 10 s 才轮到下一条，
   怀疑 SUBACK 在 wait 期间没被消费（`mqtt_wait_for` 里有 `mqtt_pump`，但现象是每条都耗满超时）。
   → **当晚定位并修掉，见 §10.1**（真因比"SUBACK 没消费"更朴素：每次 wait 都**超时**，不是没消费）。
2. **broker 重启后 shadowd 不重新订阅** → **已修（§10.2）**。
3. **按设备隔离 topic 未实现**：EMQX 5.8.6 文件 ACL 不支持占位符。 → **已落地（§10.3）**：
   换 `built_in_database` 授权源，那里的 `${username}` 会展开。
4. 台面/量产固件差异应由 `build-all.sh` 显式档位产出，并让"固件里到底装了哪些模块"可查
   → **已做（§10.4，`--firmware-preset`）**。
5. 仍留着的（**未做，已写进 `backend/docs/ops/量产上线前检查单.md`**）：控制台 TLS（配置已备好、未放证书）、
   CRL 落地（`enable_crl_check=false`，先要有可用 CRL 端点，否则 https 端点失败 = 全量拒连）、
   shadowd 单实例、eFuse 防克隆与凭证分区加密（已拍板不做，靠吊销兜底）。

## 10. 第二轮：把四个"量产口径"缺口逐条收口（2026-09-24 晚）

口径是用户定的：**"按照量产生产环境来要求"** —— 判据不是"开发跑通了"，而是"出货后错了要能查、能收、能停"。

### 10.1 ① 链路建立后 ~70 s 的 drain 停顿：真因是"订阅等待每次都超时"

现象：`link-up subscribe phase took 60709 ms`，这段里发送线程被占住 ⇒ 首帧影子躺过 TTL。
第一版怀疑"SUBACK 在等的时候没被消费"，于是把每条订阅的等待上限收到 500 ms —— **没用**（总量仍是 60 s 级），
说明**每个 wait 都耗满了自己的超时**，而不是慢。
真因：`bint_subscribe_mqtt_all()` 一条条**阻塞**等 SUBACK，而那时序上 SUBACK 根本没到（或没被 pump）⇒
每条都等满 ⇒ 串起来 60 s+。
修法：**订阅全部改成非阻塞**（`oneye_mqtt_subscribe(..., 0)`），并**去掉重复的那次 `oneye_mqtt_resubscribe`**；
`SUBACK` 返回 `0x80` 时打日志（被拒的订阅以前**完全无声**）。
另加：`bint_drain_tx()` 每 10 s 打一行 `tx progress`（app 计数 | MQTT 计数 | txq/slots/inflight），
`-7`（保留重试）/其它错误（丢弃）/TTL 过期三条路径**都留话且限频**。
**闭环证据**：真机日志不再出现 subscribe phase 警告；`app_tx=5` 在 10 s 内出现；无 `tx expired`。

### 10.2 ② shadowd 重连后不恢复订阅（生产级：broker 一重启就静默丢全部上行）

真因：paho 的自动重连**只重建 TCP/MQTT 会话，不重放订阅**（`clients list` 里 `subscriptions=0`，且无告警）。
修法：`subTracker` 包一层 paho 客户端，`Subscribe/Unsubscribe` 时记账，`SetOnConnectHandler` 里重放；
`SetConnectionLostHandler` 打 WARN（以前断了也没声）。
单测：`subtracker_test.go` 用假 token/假客户端覆盖"无订阅不重放 / 全量重放 / 去重 / 单条失败不阻断"。
**闭环证据**：`docker restart oneye-broker` ⇒ shadowd `WARN mqtt 连接断开：EOF` → `INFO mqtt 重连后已恢复 6 条订阅`，
门卫侧 `subscriptions=6`。

### 10.3 ③-b 按设备隔离 topic：换授权源，一条模板规则覆盖全部设备

**关键判断：不要继续在文件 ACL 里想办法。** 文件源不展开占位符（实测），但
**内置数据库授权源（`built_in_database`）的模板占位符会展开** —— 实测矩阵：
| 规则 topic | 订自己的 | 订别人的 |
| --- | --- | --- |
| `rmng/dev/KORVO2-0000/#`（写死） | ALLOWED | DENIED |
| `rmng/dev/${username}/#` | **ALLOWED** | **DENIED** |
| `rmng/dev/%u/#` | DENIED | DENIED（`%u` 不是这个源的语法） |
| `rmng/dev/${clientid}/#` | DENIED（探针 clientid 是随机的） | DENIED |
于是：一条 `rmng/dev/${username}/#`（allow all）覆盖全部设备，**新增设备不需要动 broker 配置**
（生成式 ACL 的不可扩展问题一并消失）。落地件：

- `deployment/broker/acl.conf`：只留控制面 shadowd；**删掉**给设备的整体放行，**也删掉 `{deny, all}`**
  （⚠️ 文件源里留一条"什么都拒"会让排在后面的授权源**永远轮不到被查**，隔离直接失效；兜底靠 `no_match=deny`）。
- `deployment/broker/sync-authz.sh`：幂等注入（一致就跳过；不一致才重写）+ `emqx ctl authz cache-clean all`
  （不清缓存会读到 1 分钟前的旧结论）+ 规则摘要打印；`--strict` = 最小权限档（publish 仅 `+/up`、
  subscribe 仅 `+/down` 与 `ota/notify`）。
- `docker-compose.yml`：`sources = [built_in_database, file]`；新增只绑回环的 `18083`（管理 API）
  与 API Key 引导文件 `api-keys.txt`（`EMQX_API_KEY__BOOTSTRAP_FILE`，格式 `key:secret:administrator`）。
- `deploy-broker.sh`：生成/沿用 API Key（**0640 root:1000** —— 0600 root:root 会让容器里 uid 1000 的 EMQX
  读不到，日志 `failed_to_open_the_bootstrap_file`，随后所有 API 调用 401；而且引导文件**只在启动时读一次**，
  改完要 recreate 容器 ⇒ 脚本会先实测 API Key，不通用就 `--force-recreate`）；跑 sync-authz；第 6 组自检
  = 按设备隔离验收（给了 `ISOLATION_CREDS/ISOLATION_NODE` 才跑）。

**验收件 `backend/scripts/ops/mqtt-isolation-check.sh`（含两个"不这么做就会自欺"的点）**：
1. **订阅侧的判据是 SUBACK 返回码，不是 token error** —— paho 对 `0x80` **不设** `token.Error()`
   （`net.go` 只写 `subResult`）⇒ 原来的探针把"被拒"读成"通过"，一度让整个实验结论反过来。必须读
   `SubscribeToken.Result()`。
2. **发布侧客户端看不到拒绝**（EMQX `deny_action=ignore`），且门卫**会节流同类日志**
   （实测 `log_events_throttled_during_last_period` 把拒绝日志丢掉 ⇒ grep 日志同样会把"拒绝"读成"没拒绝"）。
   所以发布侧判据用管理 API 的 `authorization.deny` 计数增量，并配一条"发自己的 topic 不该涨"的正向对照。
3. 每次探测换一个 clientid / 清授权缓存：EMQX 按 client 缓存授权结果 1 分钟，复用 clientid 会串结论。

**实测结论**：`PASS=6 FAIL=0`（自己订阅 ALLOWED；别人的 command/down、别人的通配、全体通配全 DENIED(0x80)；
发布自己不计拒绝、发布别人计入拒绝）。**反向对照**：故意把规则写成 `rmng/dev/NEGCTL-0000/#` ⇒ 脚本
`FAIL=2`、退出码 1（证明它不是"永远 PASS"）。**strict 档位**：6/6 通过、真机照常上报
（shadow version 98→100）、且"订阅自己的 `shadow/up`"被判拒（own-prefix 档位下允许）。
真机在隔离生效后仍然正常：门卫 `KORVO2-0000 … subscriptions=6`、平台 shadow version 连续自增、
最近 5 分钟只有探针那次预期的拒绝。

### 10.4 ③-a 固件档位（`--firmware-preset`）：把"发的是哪一版"变成产物自带的事实

`sdkconfig.defaults`（台面：Wi-Fi 文件 + PANEL_API）与 `sdkconfig.defaults.production`（量产：强制凭证、
不内嵌证书、关 Wi-Fi/面板）以前靠"记得叠加哪个"，且用哪个 defaults 只有构建命令知道。
现在 `build-all.sh --firmware-preset production|bench`（缺省 `production`，写错即 exit 2）：
产物目录 `output/firmware/<tcid>/korvo2_oneye-<preset>/`，并写入 `preset.txt`、`sdkconfig.txt`、
`preset-check.txt`（逐项生效开关）、`board-config.txt`、`srmodels.bin`、`SHA256SUMS`。
实测两份真构建：production ⇒ `WIFI_FILE is not set` / `PANEL_API is not set` / `CREDS_REQUIRED=y` /
`CLOUD_PORT=18886`；bench ⇒ `WIFI_FILE=y` / `PANEL_API=y` / 无 `CREDS_REQUIRED`。

### 10.5 ④ 控制台/账号/密钥：把"现状"和"缺口"分开写清

新增 `backend/docs/ops/量产上线前检查单.md`（§0 一页结论 + 逐条可复跑验证命令 + §6 缺口与已接受风险），
以及可直接启用的 `deployment/console/nginx/oneye-console-tls.conf`（443 + 80→301、TLS1.2/1.3、ACME 目录、
安全头、与现有 origin 同构路由）。**没有假装收口**：控制台今天仍是 HTTP（安全组限源 IP）、
CRL 未开（且证书还没有分发点）——两条都写明"放量前必须做"及顺序。

### 10.6 这轮又踩到的两个"工具级"坑（写下来免得重犯）

1. **`openssl s_client` 的 mTLS 反向自检在 TLS1.3 下不可靠**：不客户端证书时 `-brief` 形式
   5 次里 4 次 **rc=0**（TLS1.3 的"证书缺失"拒绝发生在握手之后）⇒ 自检会报"居然放行了"。
   钉 `-tls1_2` 后 5/5 rc=1 且带 `certificate required` 告警。现在自检两者都认（rc≠0 或告警文本）。
2. **`sed 's/},{/\n/g'` 可以切 JSON 规则，`tr '}' '\n'` 不行** —— 后者会把 topic 里的 `${username}`
   从 `}` 处切断；另外 curl 输出**没有尾换行**，`while read` 会把最后一行整行丢掉（要补一个 `echo`）。
