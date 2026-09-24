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

## 11. 第三轮：按"量产生产环境"把 ③ 的"产物可查询"与 ④ 的 TLS/CRL 真正落地（2026-09-24 晚）

### 11.1 ③-a 的收尾：固件产物要能回答"装了哪些模块功能"

`--firmware-preset` 之前只给了"哪些开关生效"的**逐项核对**（`preset-check.txt`）与全集
（`sdkconfig.txt`，8 万字节）。量产要的是**一眼能答**，所以产物里新增两份：

- **`modules.txt`（人读版清单）**：身份（preset/工具链/IDF/板卡/flash/云端端点/承载/是否强制凭证）、
  **被链接的 6 个 SDK 预编译归档**（base/mpp/event/log/link/ble，各带 sha256）+
  **`sdk_src_sha256`**（与 CMake 哨兵同源：源码改过没重生成归档，这里就会变）、
  按组摘录的生效开关（应用层/无线/承载与加密/音频语音/存储/分区/内存/日志/协议栈）。
  **"没开"也列**（`# CONFIG_X is not set`）—— 出货事实里"没开"和"开了"一样重要。
- **`size-components.txt`**：`idf.py size-components` 原文，按组件列 flash/DRAM/IRAM 占用
  （`liboneye_dev_base.a 92387`、`libmbedtls.a 94329`、`libfatfs.a 194965`…）
  —— 这是"装了哪些模块"的**硬证据**，也能用来比两次构建的体积漂移。

实测（production 档位真构建）：两份文件都生成（12 KB / 20 KB），`preset-check.txt` 仍 PASS。
顺带修一个自己造的 bug：编辑时把函数的 `local` 行并进了上一行的注释里 ⇒ `target: unbound variable`
（`bash -n` 不报，构建时才炸）——**改完 build-all.sh 必须真跑一次构建**，只做语法检查不够。

### 11.2 ④ 的 TLS：控制台真的上 HTTPS 了

- `deployment/console/enable-tls.sh`（新）：装 certbot → HTTP-01（webroot 就是控制台静态根）签发/复用
  → 证书落到 `/etc/nginx/certs`（key 0600）→ **装续期钩子**（`deploy/00-oneye-reload-nginx.sh`：
  续成功后同步证书 + reload，否则"续了但 nginx 还用旧证书"）→ 切站点 → 自检（`https 200` 且
  `ssl_verify_result=0`、`http 301`）。
- 实测：`https://product-testing.oneye.me/` **200 / verify=0**（外网视角也是），`http` **301**；
  证书到期 2026-12-23，`certbot.timer` 已排。
- `install-console.sh` 现在**记得** TLS 状态（`/etc/oneye/console-tls.enabled`）：重部署**不会**
  把控制台悄悄降回 HTTP（要降得显式 `--no-tls`）；它的自检也改成按实际启用的站点选探针。
- **两个坑**：① nginx 1.18 **不认** `http2 on;`（≥1.25.1 的语法）⇒ 必须 `listen 443 ssl http2;`；
  ② 启用 TLS 后 80 只回 301，原来那套"打 `http://127.0.0.1` + `Host:` 头"的断言会**全部读成 301**
  ⇒ 改成 `--resolve <域名>:443:127.0.0.1 https://<域名>`（**不加 `-k`**，要的就是真校验证书链）。

### 11.3 ④ 的意外收获：申领链路"每请求 1.5~13 s"的停顿（已修，1.5s→0.7ms）

现象：控制台申领页每次 `/claim/v1/claim/batch` 等 1.5~13 s，而 shadowd 的 `/v1/*` 只要 1~3 ms；
验收脚本的 fresh-token 探测因此偶发 `000`（**不是**验收脚本的锅）。
定位（`ss -tnp` 抓 socket）：请求期间 claimd 在往 **`169.254.169.254:80`（EC2 实例元数据 IMDS）**
建连并卡在 `SYN-SENT`。链路是 `claim.CurrentVariant()` → `LoadClaimingConfig()` →
没设 `CLAIMING_CONFIG_FILE` ⇒ 走 **AWS SSM** ⇒ SDK 解析凭据时探 IMDS；腾讯云 CVM 上那是黑洞地址。
**而参数读失败没有任何东西可缓存** ⇒ 每个请求都重来一遍。
修法（语义不变）：`claimd.env` 加 `CLAIMING_CONFIG_FILE=/etc/oneye/claiming-config.json`
（本环境没有 AWS ⇒ 申领配置走本地文件；文件不存在 = "从未配置" = 与读不到 SSM **同义**）+
**两个 env 都加** `AWS_EC2_METADATA_DISABLED=true`。
实测：同一路径 **1.5~13 s → 0.7 ms**；整套 `console-domain-acceptance.sh` 从"偶发 FAIL"变成
**in-host PASS 24 · FAIL 0**，全程 2.6 s。

### 11.4 ④ 的 CRL：从"一句口号"到"有证据的吊销"

这轮把 CRL 这条链上**四个真缺口**补掉（都是实测发现的，不是读代码猜的）：

| # | 缺口 | 证据 | 修法 |
| --- | --- | --- | --- |
| 1 | **名单没人刷新**：平台 CA 的名单停在 2026-09-22（旧 openssl cron 2026-09-21 退役时，"改由签发服务导出"接上了、**定时发布没接**）⇒ 这期间**吊销谁都不生效** | `ls -l` 名单 mtime；`/etc/cron.d` 里只有 dev CA 那条 | `deployment/signer/publish-crl.sh`（导出→校验 issuer/nextUpdate→**原子替换**→`--check`）+ `oneye-crl-publish.timer`（每 6h） |
| 2 | **签发服务跑在 `/tmp/signerd` 且手工起**：`/tmp` 一清或重启即没，签发与名单导出同时消失且无人知道 | `/proc/<pid>/exe -> /tmp/signerd`、无 systemd 单元 | `install-signerd.sh` → `/opt/oneye/signerd` + `oneye-signerd.service`（Restart=always、开机自起；本机因 CA 在 `/home/ubuntu` 而用 `ProtectHome=read-only`） |
| 3 | **容器取不到 CRL 端点**：腾讯云 EIP **没有 hairpin**，容器→自己公网 IP`:8080` 必超时 ⇒ 开 `enable_crl_check` 就是**全量拒连** | 容器内 `curl` 12s 超时；`fix-hairpin.sh` 的 DNAT 只覆盖旧网段 `172.18/16` | `docker-compose.yml`：**固定网段 172.28.0.0/16 + `extra_hosts: crl.oneye.me → 172.28.0.1`**（容器内实测 `200 / 1978B / 1.8ms`） |
| 4 | **新证书不带 DP**：`claimd.env` 的 `CA_CRL_DP_URL` 是空的 ⇒ 即便开了开关，新设备也是"无 DP 证书"（行为随缓存漂移） | `openssl x509 -ext crlDistributionPoints` 为空 | 归一为定稿值 `http://crl.oneye.me:8080/oneye-iot-device-ca.crl`，真签一张验证已带 DP |

**端到端验收件** `scripts/ops/crl-enforcement-check.sh`（可复跑，**不碰生产监听器**）：
签两张带 DP 的证书 → 吊销其中一张（`/v1/revoke` → PG 台账）→ `publish-crl.sh` 重发 →
**重启门卫**作废名单缓存 → 临时建 `ssl:crlprobe`（:18887，`enable_crl_check=true`）→ 对照连接：
未吊销 `ALLOWED`、已吊销 **`remote error: tls: revoked certificate`** → 删掉临时监听器。
实测 **PASS=9 FAIL=0**。两个坑也记在里面：① **不要用 `emqx_crl_cache:evict/1` 代替重启** ——
实测 evict 之后门卫反而**放行**被吊销的证书（缓存项清空但对端没重新取名单）；
② 探针 SNI 必须用门卫证书里的 `mqtt.oneye.me`（控制台域名不在 SAN 里）。

**仍未做（下一步，顺序不能反）**：18886 上 `enable_crl_check` 还是 `false`。
开之前必须先把现网那台设备**重签成带 DP 的证书**并重新灌注（设计 §3.6.1 的"存量重签"），
否则无 DP 证书在冷缓存下会被拒。回滚只需把开关改回 false —— 唯一不可逆的点（证书里印的 DP）
已经印上了。

## 12. 第四轮：CRL 收口 —— 存量重签 + 打开开关（2026-09-24 晚，目标第 2 轮）

上一节留的"最后一步"做完了，顺序严格按"先证书、后台账、最后开关"：

1. **存量重签（私钥不出设备）**：从现网证书取公钥（`openssl x509 -pubkey`）→
   `POST /v1/sign-pub`（signerd）⇒ 只回证书；**校验三件事**：公钥与旧证书**逐字节一致**、
   带 DP（`URI:http://crl.oneye.me:8080/oneye-iot-device-ca.crl`）、issuer = 平台 CA，且台账多一行。
   实测：旧序列 `af92322d…` → 新序列 `4e9a27ef…`，notAfter 仍被钳在 CA 到期日 2036-09-17。
2. **重新灌注**：用现有私钥 + 新证书按交付物格式组一份目录（`manifest.csv` 按声明列）→
   `mkcreds -zip` 出 `creds.bin`（16384 B、CRC32 `6d2cf73b`、指纹 `4b58cd59…`）→
   `mkcreds -verify` 自检 → `provision-device.ps1` 写凭证分区并**回读逐字节一致**（PASS）。
   ⚠️ 一个 Windows 坑：`Compress-Archive` 写进 ZIP 的是**反斜杠**路径（`devices\SN\client.crt`），
   mkcreds 找不到 ⇒ 必须用 `System.IO.Compression.ZipFile.CreateEntryFromFile` 显式给
   `devices/SN/client.crt` 这样的正斜杠条目名（WSL/宿主上都没有 `zip`）。
3. **真机自证**：串口 `ONEYE-PROV1 node_id=KORVO2-0000 cert_fp=4b58cd59…`（= 本地新证书指纹）、
   `link up: mqtt.oneye.me:18886 transport=mqtt-tls`、`esp.cred_source=partition`、
   `tx progress: app_tx=5 mqtt_pub=5 … inflight=0/8`（顺带又一次证明 ① 的修复还在生效）。
4. **吊销被替换的旧序列号**（reason `superseded_by_resign`）+ 重发名单（33 条）。
   不吊销的话，旧凭证仍然是"有效身份"——这一步容易漏。
5. **打开 18886 的 `enable_crl_check`** 并重建容器；对照验证（都在**生产监听器**上）：
   - 真机 `KORVO2-0000` 照常在线（`subscriptions=6`）、平台影子 version 继续自增；
   - 同批未吊销的带 DP 证书 `ALLOWED`；同批**已吊销**的 `remote error: tls: revoked certificate`。
6. **`deploy-broker.sh` 新增一条自检**：开着 CRL 检查时，**容器必须取得到名单**（容器连公网 IP
   没有 hairpin 是实测事实）—— 取不到就直接 FAIL，因为那等于"下一次重启 = 全量设备掉线"。

**踩到的两个坑（都已写进文档/脚本注释）**：
- **fixture 会过期**：开着 CRL 检查后，`mqtt-isolation-check.sh` 拿**重签前的旧证书**当 fixture
  ⇒ 所有探针都被 `tls: revoked certificate` 拒掉，看起来像"隔离/链路坏了"，实际是**吊销正常生效**。
  换证后必须同步更新 fixture（换成设备**当前**那份）。
- **旧夹具导致的假 FAIL 也顺带成了证据**：`deploy-broker.sh` 第 6 组自检先红后绿，红的正是
  "被吊销的旧证书连不上"。

**最终状态**：CRL 这条链 ① 端点 ② 定时发布 ③ 签发服务受管 ④ 新证书带 DP ⑤ 容器可达
⑥ 吊销端到端生效 ⑦ 开关已开 —— 全部有可复跑证据；回滚只需把开关改回 `false`。

## 13. 第五轮：开开关**之后**才暴露的两个坑（名单服务本身）

打开 `enable_crl_check` 的那一刻，8080 这个端口就从"一个静态文件服务"变成"设备能不能连上"的
必经环节（取不到名单 ⇒ 冷缓存时全量拒连）。于是两个原本只是"脆弱"的东西立刻成了生产缺陷：

1. **名单服务是手工起的 python httpd**（`setsid nohup python3 -m http.server 8080`）——
   没有单元在管、重启即没。已换成 **nginx**（systemd、开机自起），配置见
   `deployment/signer/nginx-crl.conf`，由 `install-signerd.sh` 第 6 步安装（会先停掉占着 8080
   的手工 httpd 再 reload）。
   ⚠️ 顺带撞到 **403**：nginx worker 是 `www-data`，而 `/home/ubuntu` 0750、`/home/ubuntu/crl-dp` 0700
   ⇒ 连目录都进不去。web 根因此搬到 **`/var/www/oneye-crl`**（老路径留软链，`publish-crl.sh`
   与 dev CA 的 cron 同步改新根）。
2. **证书里印着两种 DP 形态**，只留一个入口就会让另一半取不到名单：
   - `http://crl.oneye.me:8080/oneye-iot-device-ca.crl`（2026-09-21 起的定稿形态）
   - `http://crl.oneye.me/oneye-iot-device-ca.crl`（更早签发的证书里印的是这个，**不带端口**）
   实测：拿 `platca-bed/client.crt`（老形态）去连时，门卫按 80 取 → nginx 返回 **301** 跳 https →
   跟随到 443 后 `bad_cert,hostname_check_failed`（证书是 product-testing 的）⇒ `failed_to_fetch_crl`。
   **证书里的 URL 是印死的、改不了** ⇒ 只能让这个 URL 也能取到：nginx 加一个
   `server_name crl.oneye.me` 的 **:80 直给文件（不跳转）**。现在两条入口外网/容器内都 200，
   老形态 DP 的证书连 18886 通过且日志**不再出现** `failed_to_fetch_crl`。

**教训（可复用）**：把某个开关打开，等于给它依赖的那条链**升格**；原来"能跑就行"的环节
（手工进程、单入口、放 /home 下的 web 根）会立刻变成生产缺陷。开开关后**要按"这条链断了会怎样"
重新过一遍**，而不是只验开关本身生效。

## 14. 第六轮：交付包"按 node/SN 查回并再次下载"（2026-09-24，用户新需求）

**需求**：控制台「凭据身份」页要能按设备 node 或 SN 查到它那次量产签发的交付 ZIP，并**再次下载**。

**为什么本来做不到**：交付 ZIP 在申领服务里是**流式返回、服务端不留副本**；产线把文件弄丢后，
唯一能重新拿到凭证的办法是**重签**——而重签会产出第二张同时有效的证书（吊销只能吊销一张），
正是 `/v1/claim/batch` 一直在防的事。

### 14.1 落地（后端）

- 新包 `src/claim/artifacts`：`Store` 接口 + 内存实现 + `pgstore`（两张表：`claim_batches` 存整批 ZIP、
  `claim_artifacts` 存逐台记录），与 `devicecert/pgstore` 同一套做法（事务级 advisory lock 建表）。
- 批量签发时顺手入库：整批 ZIP 原样存一份 + 逐台一行（sn/node/serial/指纹/有效期/该台的 manifest 行/保留期）。
  响应头新增 **`X-Oneye-Artifact-Stored: 1|0`** —— 入库失败**不影响交付**（包已在手里），但必须让调用方
  知道"以后还能不能再下载"（否则会默认"存好了"）。控制台把它读出来渲染成提示（CORS 的
  `Access-Control-Expose-Headers` 也加了这个头）。
- 批号缺省时**服务端生成**（`B-YYYYMMDD-HHMMSS-xxxx`）：批号是"这一次交付"的定位符，空批号会让这个
  单位在库里丢失。
- 两条新路由（都走与签发同一道 `aud=admin` 门）：`GET /v1/claim/artifacts?q=&sn=&serial=&limit=`
  （元数据，**不含密钥**；查不到=空列表而不是 404）、`GET /v1/claim/artifact?sn=|serial=[&download=1]
  [&whole_batch=1]`（单台包 / 整批原文）。
- 单台包 = 从整批 ZIP 里**只取这一台的条目**重打包：`manifest.csv` 只留这一行，`batch.json`/`README.md`
  **逐字保留原批那份**（它们描述的是这一批，按单台重写会把"这批多少台/批号"改错），私钥标 0600。
  **不重新签发** —— 验收会比对指纹与台账一致。
- **保留期** `CLAIM_ARTIFACT_RETENTION_DAYS`（缺省 30 天；`0`=永久）：到点只清**载荷**、元数据留着 ⇒
  页面仍能回答"这台在某批签过"，但下载给 **410 Gone**（要再拿只能重签），而不是含混的 404。
- 每次下载：`download_count`/`last_downloaded_at` + 一行审计日志。

### 14.2 落地（控制台）

「凭据身份」页新增第二个卡片"量产交付包（按 node/SN 查询与再次下载）"：一个查询框（node/SN/序列号都走
`q=`，服务端同时匹配三列）+ 结果表（SN/序列号/批次/签发时间/是否已入库/已下载次数）+「下载本台」/「下载整批」；
`downloadable=false` 时按钮禁用并显示"已过保留期 —— 需重新签发"。API 层
`src/api/backend/claim-artifacts.ts`（8 条 vitest 覆盖 200/404/410/503/未配置/文件名/整批）。

### 14.3 实测（真平台，`scripts/ops/claim-package-acceptance.sh`）

`PASS=14 FAIL=0`：签发 200 + `X-Oneye-Artifact-Stored: 1`；按 SN 查到 1 台（downloadable、保留期 30d、
store=postgres）；再下载得到单台包（含 client.key 模式位 600、manifest 只一行、**指纹与记录一致**）；
下载计数=1；整批下载 200；未签过 404；匿名 401；PG 两张表都有行。
控制台验收脚本另加 3 条断言（匿名 401 / 授权可读 / **响应不含密钥材料**），in-host 从 PASS 24 变
**PASS 28 · FAIL 0**；dashboard 单测 199 条全绿（其中新增 8 条）。

### 14.4 顺带发现并修掉一个**审计缺口**（比功能本身更要紧）

验收时发现 `claimd.log` 里**只有 gateway 的拒连行**：`claim batch issuance`（"谁、哪一批、出了多少张"）
这类 rlog **Info 级审计行一条都没有** —— 因为 **rlog 的缺省等级是 ErrorLevel**。
也就是说在这之前"我们有审计"是句空话，控制面动作**没留痕**。
修法：`claimd.env` 显式 `RLOG={"level":"info"}`（安装脚本已写入并注释为什么这么设）。
复核：重跑后 `grep -e 'claim batch issuance' -e 're-downloaded' /var/log/oneye/claimd.log` 两行都在。

**教训**：写完"记审计"的代码后必须**去日志里把它找出来**才算数 —— 本次正是"断言审计行存在"这条
测试把缺口抓出来的（日志既不在 journald 里，等级又不够，"有审计"就变成了想当然）。

## 15. 第七轮：运维报"凭据身份页设备表空了、记录都丢了"（2026-09-24）

现象：页面报 `身份视图加载失败：token revoked: console-domain-acceptance`，设备表为空。
查下来是**两个独立原因叠加**，**都不是数据丢失**（库计数：`devices=15 / shadow_docs=17 /
device_certs=156`，影子与台账都在）：

1. **我自己的验收脚本把操作员的会话踢了**（真凶）：`console-domain-acceptance.sh` 最后一段按
   **主体 cutoff** 吊销 admin 的**全部在途令牌** —— 包括浏览器里那个。跑一次验收 = 所有人被登出，
   看起来就像"系统坏了、数据没了"。台账实测 `admin_tokens=57 / admin_revoked=54`，最近两条
   reason 正是 `console-domain-acceptance`。
   → 默认改为**只吊销本次运行自己那张**（从 JWT 解 jti → `/v1/auth/tokens/{jti}/revoke`）；
   主体 cutoff 挪到显式 `--revoke-subject`（会踢人，跑前警告）；新增断言"另一个会话不受影响"。
2. **身份视图是进程内状态**：`verdicts` 只在设备上报时写入，shadowd 一重启就空，而且**不会自愈**
   （只有之后上报过的设备才回来）⇒ 页面上像"记录丢了"。
   → 新增 `credIdentityWatch.Hydrate(ctx)`：启动时用**库里已有的影子**装载一遍。
   三条口径：**只装载不重放告警**（重启后重放几个月前的告警会让告警失去意义）、
   `LastSeen` 用**影子自己的 updated_at**（否则离线很久的设备看起来像刚上报）、
   与实时路径**共用同一个分类实现** `verdictFor`（两处口径迟早分叉）。实测重启后**立刻**
   `total=1 partition=1`，日志 `身份视图已从库里装载 1 台设备（共 17 个影子文档；只装载不重放告警）`。
3. 顺带把控制台的 401 收口成人话：带令牌的 401（且不是 `/v1/auth/login`）⇒ 清会话 + 回登录页 +
   抛"会话已失效（令牌被吊销或已过期），请重新登录。"，不再把 `token revoked: …` 摊在页面上。

**两条可复用的教训**：
① **验收脚本不许有副作用**：它有副作用（吊销全部会话）时，"跑验收"和"系统故障"在运维眼里
   长得一模一样 —— 破坏性校验必须显式开关 + 跑前警告。
② **"记录丢了"先看数据源**：这次的真相是"视图是派生且只在内存里"，数据一条没少；
   凡是**派生视图**都要问一句"重启后它会长回来吗"，长不回来就得从持久源装载。

## 16. 第八轮：`KORVO2-0000` 按编号查交付包**查不到**，但身份视图里明明有这台（2026-09-24）

**用户原话**："我以 KORVO2-0000 作为关键字查询并没能查到交付包，但按凭据身份列出的设备是有这个记录的。"

### 16.1 为什么"两边不一致" —— 它们读的是**两个数据源**，不是同一件事

| 页面 | 数据源 | 覆盖范围 |
| --- | --- | --- |
| 身份视图（设备表） | `shadow_docs`（设备**上报过的影子**，启动时 `Hydrate()` 装载） | 上过线的设备 |
| 量产交付包 | `claim_artifacts`（**交付包留档**，2026-09-24 才上线） | 只有 09-24 之后经 `/v1/claim/batch` 签发的批次 |

`KORVO2-0000` 的当前证书 `4e9a27ef…`（指纹 `4b58cd59…`）是 **09-24 当天走重签路径
`POST /v1/sign-pub` 签出来的**（存量重签，见 §12）—— 时间上晚于留档上线，但**那条路径根本不落交付包**，
所以库里没有它的包。旧证书 `af92322d…` 已吊销（09-23 签发，同样没留档）。

**结论：不是数据丢失，也不是查询坏了，是"留档能力上线前/非批量路径签发的证书在平台上没有包"。**

### 16.2 修法一：空结果要说清是"没签过"还是"签过但没留档"（后端 + 页面）

- `GET /v1/claim/artifacts` 在**查不到**时再问一次台账，命中就给 `ledger_match`
  （`node_id/serial/issued_at/not_after/revoked` + 一句人话 `reason`）；台账里没有 ⇒ 不给该字段（= 真没签过）。
  留档上线的时刻写成常量 `packageStoreEpoch = 2026-09-24T00:00:00Z`，用它把两种成因分开说。
- `reason` 按**已吊销 / 早于留档上线 / 其它（重签路径或当时没配存储）**三分支，都带上"怎么补救"。
  ⚠️ 第三分支**不猜是哪一种**：台账里没有能区分"重签路径"与"当时没配存储"的字段
  （正是 `recordCertIssuance` 对 `replaces_serial` 为空的那条说明），并列两种可能比猜一个诚实。
- 控制台（`claim-package-panel.tsx`）空态分两种渲染：有 `ledger_match` 就给黄条 + 台账事实
  （node/序列号/签发时间）+ 平台原话；没有才是"平台上没有这个 node/SN 的签发记录"。
  i18n 新增 6 个 key（zh/en 对称，`npm run check:i18n` 把关）。

### 16.3 修法二：补录（`POST /v1/claim/artifact`）—— 把手上那份包登记进来

- 请求：`{sn, package_base64, serial?, batch_id?, source?, note?}`（ZIP ≤ 8 MiB）。
- 平台侧**三条**校验：① 管理员令牌（与签发同一道门）；② 包里的证书必须是**台账里这台设备的当前那张**
  （按 `devices/<sn>/client.crt` 的 CN 找 node、序列号与 `GetByNodeID`（not_after 最大的一张）一致）——
  CN 不符/不是 ZIP/缺这份证书 ⇒ `400`；台账没这台、或是旧/已吊销序列号 ⇒ `409`；③ 每次留一行审计。
- **不重新签发**、不写台账、不改证书：只是把一份已存在的交付物登记进 `claim_artifacts`/`claim_batches`，
  之后单台下载/整批下载照常工作。
- ops 脚本 `scripts/ops/claim-package-ingest.sh`（`--dry-run` 先本地体检：ZIP 可解 + 是哪台 + 序列号/指纹/有效期，
  再提交 + 复查查询）。已同步到 `deployment/console` 检查单 §2.7 与契约 §4.1。

### 16.4 实测（真平台）

- 补录前：`?q=KORVO2-0000` ⇒ `count=0` + `ledger_match{serial=4e9a27ef…, revoked=false, reason="…没有它的交付包留档…"}`
  （直连 9091 与控制台同源 `/claim/…` 两条路都验过）。
- 补录 `/tmp/resign/resign-KORVO2-0000.zip`（3894 B，含 client.key）⇒ `serial=4e9a27ef… batch=INGEST-KORVO2-0000-20260924-085312`；
  复查 `count=1 downloadable=true`，`ledger_match` 消失。**现场核对 PASS=7 FAIL=0**
  （同源查询 / 下载 200 / 指纹一致 / manifest 单行 / 含私钥 / 审计行落盘 / 负例）。
- 负例：把**已吊销**的旧证书塞进同一台设备的包 ⇒ `409 ... the package carries serial af92322d…,
  but the live certificate for KORVO2-0000 is 4e9a27ef…`。
- 回归：`claim-package-acceptance.sh` **PASS=14 FAIL=0**；`console-domain-acceptance.sh` in-host
  **PASS 29 · FAIL 0**（含"identity view still loads for a fresh session"）；dashboard 单测 **205 条全绿**。

### 16.5 顺手修掉的安装脚本坑

`install-console.sh` 写 `claimd.env` 的那段注释里有**反引号**（`` `claim batch issuance` ``），
而它在**双引号**里 ⇒ bash 把它当命令替换执行，安装时报 `line 184: claim: command not found`，
写进 env 的注释还被吞掉两个词。改成单引号。**教训**：往文件里写的注释也会被 shell 解析，
要在双引号字符串里放反引号，先问一句"这会不会被当成命令替换"。

### 16.6 可复用的教训

① **"两个页面不一致"先问数据源，再问覆盖窗口**：这次不是 bug，是"派生视图"（影子）与
   "后加能力"（留档）覆盖范围不同；把覆盖窗口写成空结果里的**明确说明**，比让操作员自己猜要值钱。
② **后加能力要给它补一条"存量补录"入口**：留档只能覆盖上线之后的签发，存量设备否则永远查不到 ——
   补录把"历史包"接进来，是这类能力上线时的标准配套（安全校验一条都不能省：只收台账当前那张证书）。

## 17. 第九轮：列表分页 + node_id/sn 两栏 + 真机复验"下载的包能不能上线"（2026-09-24）

用户的三个要求：①问清"查询条件里 node 是 node_id 吧？sn 是设备序列号？"；②两个列表都要分页、
每页 20 条，且**结果里 node_id 与 sn 各占一栏（即使取值相同）**；③把 `E:\tmp\prd-test\claim-package-KORVO2-0000`
里的产物烧到开发板，验证能否正常上线。

### 17.1 三个编号讲清楚（这轮现场最容易混的一处）

| 名字 | 是什么 | 例 |
| --- | --- | --- |
| `node_id` | 平台侧设备身份 = **证书 CN**（来自设备 `creds` 分区） | `KORVO2-0000` |
| `sn` | **产线标签上的设备编号** | `KORVO2-0000` |
| `serial` | **证书自身的序列号**（x509 serial，与台账/CRL 同口径） | `4e9a27efdffba844b890c57ad066b502` |

本项目量产口径 **`node_id == sn`**（设计 §8.6 #2；`claim_batch.go` 里 `batchRow{SN: sn, NodeID: sn}`）。
所以：交付包结果表**拆成 `node_id` / `sn` 两列**（页面还写着"serial 是证书序列号，不是设备编号"）；
身份视图只有 `node_id`（**影子文档里没有 sn 字段**，页面上写明这点，不是编一列假数据）。
契约 §4.1 有对照表 —— 把它写进契约是因为**拿设备编号去 CRL 里核吊销**这种错用真的发生过。

### 17.2 分页：服务端切页，且 `count` 与 `total` 是两件事

- 交付包列表：`page` / `page_size`（缺省 20、上限 200；`limit` 是同义旧写法），
  PG 侧 `SELECT count(*)` + `LIMIT/OFFSET`，两者**共用同一个 WHERE**（各写一遍迟早出现
  "第 2 页有行、total 说只有 1 页"）。
- 身份视图：内存快照切页，同一个 `page/page_size` 口径；响应多给一个 `total`（**筛选后**行数）。
  ⚠️ 它的 `summary` 仍是**全量**口径 ⇒ 页面必须分别标注（"共 N 台 · 第 x/y 页"），
  否则"卡片 3 台、表里 1 行"会被读成数据对不上账。
- `Count` = 这一页的条数，`Total` = 匹配总数：只回一页不说总数，分页控件只能靠"这页满没满"猜。
- 非法页参数（`page=0`、`page_size=0/201/x`）一律 **400**，不静默回第一页。
- `ledger_match` 的判据从"这一页为空"改成 **`total == 0`**（翻到第 3 页时页里没行 ≠ 查不到）。
- 前端用设计系统的 `DataTable`，但它这一版的 `pageIndex` 是**写死 0** 的（`pagination: {pageIndex: 0, pageSize}`，
  Next 只回调 `onNextPage`）⇒ 必须走**服务端分页**那套 props（这也是仓里其它列表的既有做法）。
- 控制台里每页 20 条只写一处（`CRED_IDENTITY_PAGE_SIZE` / `CLAIM_ARTIFACTS_PAGE_SIZE` + 服务端
  `artifacts.DefaultPageSize`），免得"接口说 20、页面按 10 排"。

### 17.3 真机复验：**从控制台下载的**交付包 → 上线

真板 Korvo-2 / COM12、SN=`KORVO2-0000`（证书 serial `4e9a27ef…`、指纹 `4b58cd59…`）：

1. 从控制台下载该台的交付包（不是签发时手上那份）→ 解出 `devices/KORVO2-0000/{client.crt,client.key,ca.crt}`；
2. **先擦掉凭证分区取证**：设备打印 `magic 不是 "ONEYECR1"（ff ff ff…）→ 视为从未写入` +
   `本构建要求必须有分区凭据 —— 不联网，送产线重新写入`（网络探测全 OK ⇒ **拒绝上云**，fail-closed）；
3. `mkcreds -zip <下载的包>` → 16384 B、`crc32=f6ed1bb8`、`image_sha256=1801b9c1…`；
4. `provision-device.ps1` → **exit 0 / PASS**，写 `0x510000` + 回读逐字节一致；
5. 串口：`凭据来源：creds 分区 node=KORVO2-0000 … crc32=f6ed1bb8`（与清单一致）、
   `ONEYE-PROV1 … cert_fp=4b58cd59…`（与清单一致）、`link up: mqtt.oneye.me:18886 transport=mqtt-tls`；
6. `provverify` 用**包里那张证书**验自证串 → **PASS**（7 项，含 ECDSA 验签与防重放）；
7. 平台侧新增 `scripts/ops/device-online-acceptance.sh` ⇒ **PASS=6 FAIL=0**
   （门卫 `Client(KORVO2-0000, username=KORVO2-0000, connected=true, subscriptions=6)`；
   影子 `esp.cred_source=partition`；身份视图 `partition`；证书不在 CRL）。
   负例 `--sn NOPE-9999` ⇒ PASS=0 FAIL=4 / exit 1（**能失败的门才叫门**）。

> **取证要点**：第 2 步不能省 —— 只"写一遍看到上线"证明不了是这份包起的作用（板子上原来可能
> 就是同一份身份）。这和 §16 里"先看空结果再补录"是同一个道理。

### 17.4 这轮踩到的两个工具坑（都不是产品缺陷）

1. **`emqx ctl clients list | grep -q` 会假失败**：`set -o pipefail` 下 `grep -q` 命中即关管道，
   左侧进程收 SIGPIPE ⇒ pipeline 状态非 0。**先落文件再 grep**（`device-online-acceptance.sh` 里注释写明了）。
2. **PowerShell 脚本里出现空串变量做 grep 模式**：`grep -ci "$EMPTY"` 匹配**每一行**，
   于是"证书在 CRL 里"这种假 FAIL 就出来了。**拿不到值就跳过并说明**，别拿空串去匹配。
3. （老朋友又踩一次）**`write` 出来的 .ps1 没有 UTF-8 BOM**：Windows PowerShell 5.1 按 GBK 解码，
   中文注释把语法读坏 —— 临时的 .ps1 一律**只写 ASCII**（仓里那些 .ps1 带 BOM 是有原因的）。

