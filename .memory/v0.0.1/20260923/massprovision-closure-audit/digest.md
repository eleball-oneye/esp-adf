# 量产/产线批次收口审计与补齐（2026-09-23）

> **会话 id**：`massprovision-closure-audit` ｜ **仓**：`embedded/esp-adf`（本会话工作目录）｜
> **跨仓**：`backend`（oneye-iot）、总控 `rainmaker-oneye`、其内嵌 `docs/oneye-iot-index`
> **一句话**：对 2026-09-22/23 的"量产批量签发 + 设备凭证分区 + 伙伴/OEM 产线"整批做**闭环审计**，
> 找出并修掉 **1 处规划内必需但未实现**、**1 处死配置**、**1 处编译级回归**，并补齐三仓的登记与指针。

## 1. 审计结论（改动前）

设备侧/工具链侧那一批是**真闭环**（真机证据齐全）；缺口集中在"平台侧"和"配置生效性"：

| # | 缺口 | 性质 | 位置 |
| --- | --- | --- | --- |
| A | **P0-A「凭据来源对服务端可见」只做了设备侧** | 规划里写明"必需，属 backend 侧"（SDK `docs/API-creds.md` §5）却无消费者；只有一份**未跟踪、无调用方、注释引用不存在的 `Fingerprints`** 的草稿包 | `backend/src/rmneo/credsource/credsource.go`（当时 `??`） |
| B | **量产预设里一处死配置** | `CONFIG_ONEYE_FW_TRANSPORT_MQTT_TLS`（真名 `..._TRANSPORT_TLS`）⇒ "显式钉住承载"是安慰剂；且未钉云端端点 | `esp-adf examples/oneye/korvo2_oneye/sdkconfig.defaults.production` |
| C | **`go test ./src/...` 直接编不过** | 2026-09-22 给 `devicecert.Store` 加 `GetByNodeID`，4 个测试替身没跟上 ⇒ 全仓门禁当时是**红的**（批内只跑了分范围门禁） | `backend` `shadow/cmd/shadowd`、`ca/signer` |
| D | 指针/工作区未收口 | `cloud` 远端已 `a5ebe30` 而总控仍 pin `0feebda`（工作区 `M cloud`）；backend 有 5 个未跟踪测试/构建残留 | 总控 + backend |
| E | 登记与漂移 | 新契约页三处未登记；backend `docs/README` 无 ops 段、两处版本号过期；总控 `落地计划/仓库地图/可行性评估` 未回写（`cloud/` 未入地图、pin 表停在 09-15） | 三仓文档 |

## 2. 本轮做了什么（每项都带可复跑证据）

1. **平台侧可见性（A）**：新包 `backend/src/rmneo/credsource`（判决：**只有显式 `partition` 算合格**；`unknown` 不作合格；键缺失时可按登记的公用证书指纹兜底）+ shadowd 接线
   `cred_identity.go`（`embedded` ⇒ WARN **+ `device_alarm`** + gauge `shadowd_device_identity_embedded`；恢复 ⇒ INFO；**键缺失不喊**）。
   证据：`go test ./src/rmneo/credsource/... ./src/rmneo/shadow/cmd/shadowd/...` 全绿（含"重复上报不二次告警 / 恢复后再回退要再喊 / 键缺失静默 / 指纹兜底 basis 可区分 / 空清单不参与"）。
2. **死配置（B）**：改对符号名 + 补钉 `CONFIG_ONEYE_FW_CLOUD_HOST/_PORT`；新增门禁 `tools/check-sdkconfig-defaults.py`（查"写了但不存在的符号"与"预设未生效"，**带负控**）。
   证据：① 量产预设真构建 `BUILD_EXIT=0`，`korvo2_oneye.bin=0xf08f0`、**53% free**；② `--sdkconfig <产物> --require sdkconfig.defaults.production` ⇒ **11 项全部存在且已生效**（含 `TRANSPORT_TLS=y`、`CLOUD_HOST="mqtt.oneye.me"`、`CLOUD_PORT=18885`）；③ 开发用 `sdkconfig` 哈希前后一致（未被污染）；④ 红线守卫 A/B：`REQUIRED=y`+内嵌证书 ⇒ **exit 2** + 两条修法（B = 上面的 exit 0）；⑤ 负控：把 `_MQTT_TLS`/拼错的 `is not set` 塞回去 ⇒ 门禁 **exit 1** 并逐行点名。
3. **编译级回归（C）**：给 4 个替身补 `GetByNodeID`（`shadowd.failingStore`、`ca/signer` 的 `failingStore`/`invisibleStore`/`failAfter`），全仓门禁复跑。
4. **指针/工作区（D）**：bump `cloud` 指针；删掉 5 个未跟踪残留（`claim_handler.exe` + 4 个测试计时残留）并把两类加进 `backend/.gitignore`。
5. **登记与漂移（E）**：契约页 #21 三处登记（`domain/README` + `contracts/README` + `评审记录` **R5 待签**）；`backend/docs/README` 新增 **E. 运维与产线作业 · ops/** 段并修两处版本号；设计文档 §8 标题/仍未做清单/§7.3⑰ 漂移回写；总控 `落地计划` 新增整批登记（含指针）、`仓库地图` 补 `cloud/` 与版本基线（ESP-ADF `3cb5613e`、SDK **v0.4.0** 6 库 `d7c665e`）、`可行性评估` 第 89 行"证书注入仍缺"改写；index 台账 `设备面契约.md` 量产块与 §12#7 补 09-23 追加。

## 3. 仍然开放（不声称"量产就绪"）

- **已拍板（2026-09-23，已回写）**：**不做** eFuse 防克隆（接受"复制 flash 即复制身份"，靠吊销兜底）、
  **不做**凭证分区加密（我们自己的量产形态不启用 flash/内容加密；伙伴开 flash 加密时"是否标 `encrypted`
  须双方确认"仍有效）、**私钥由服务端生成**（交付 ZIP 含 `client.key` ⇒ 归档按密钥介质管理）、
  **产线联网**且**批量申领在控制台 web 上操作** —— ✅ **控制台入口已实现**（见 §3.1）；
  **控制台登录收敛为一个固定账号** `admin`/`Oneye@hz2026`（最大权限；`ADMIN_LOGIN_ONLY` 缺省 1 ⇒ 其它账号
  暂不可登录；`ADMIN_PASSWORD` 无缺省值，未设即拒绝启动）；**申领服务不做 TLS**（安全组限制固定源 IP）；
  ✅ **令牌吊销/续期**与 ✅ **身份视图列表页**均已落地（见 §3.2）。
- **未做/仍开放**：控制台**前端生产托管**（今天只有 dev :5174 与 `preview`/`dist`，无 nginx 配方）、
  `CORS_ORIGINS` 从 `*` 收紧、产线工装/扫码/一拖多/PASS-FAIL 上传、存量重签迁移（排期）、
  **shadowd 生产部署**（含 `DEVICE_CRL_*`）、EMQX **authentication**（ACL 只管授权）、台账 `node_id` 专用索引。
- **口径边界**：`esp.cred_source` 是影子 `reported` 里的**普通键**（不是新 topic/能力位）⇒ 不构成契约面变更；
  指纹兜底默认**关闭**（空清单 = 不参与）；量产固件本轮只有**构建级 + 配置生效级**证据，**未上板**。

## 3.1 控制台批量签发入口（2026-09-23 落地，t08）
- 页面：`dashboard` 节点管理「**量产签发**」`/home/node-management/provision`（清单录入/上传 → `POST /v1/claim/batch`
  → 逐台状态 → 下载 ZIP；**含私钥提醒**；200/207/422 分档）。
- 通路：dashboard **直连**申领服务（`CLAIM_API_URL` / `?claim=`）+ 外壳层 CORS（`CORS_ORIGINS`，预检先于鉴权、
  `Expose-Headers` 带 `X-Oneye-Batch-*`）；422 响应体**追加** `rows`（`message` 不变）。
- 证据：`scripts/ops/claim-console-acceptance.sh --start` ⇒ **PASS 14 · FAIL 0**；dashboard `typecheck` /
  **vitest 24 文件 151 用例** / 新文件 eslint / `build` 全绿；Go `build`+`vet`（含 `-tags container`）exit 0。
- ⚠️ 既有债（与本批无关，已实测）：仓库级 `npm run lint` 里的 `check:i18n` 在 HEAD 上就红（63 个问题，`register:*` 为主）。

## 3.2 令牌吊销/续期 + 身份视图 + 控制台单一账号（2026-09-23 同日再落，t09）

- **登录收敛**：`ADMIN_LOGIN_ONLY`（缺省 1）⇒ 控制台只认 `ADMIN_USERNAME`/`ADMIN_PASSWORD` 那一对
  （产线形态 `admin`/`Oneye@hz2026`，最大权限）；用户表管理员要能登录得显式 `=0`；`ADMIN_PASSWORD` 无缺省值。
- **令牌**：`src/rmneo/auth/tokenguard`（内存 + PG `access_tokens`）+ 四条控制面路由（列出/吊销单张/
  吊销主体全部/续期）+ 控制台「访问令牌」页；所有令牌带 `jti`；shadowd 与**申领服务**两侧验签后查台账，
  台账不可读 ⇒ fail-closed；**只有台账里有行的令牌可吊销**（密钥直接签出的不在台账里 ⇒ 处置手段是换密钥）。
- **身份视图**：`GET /v1/devices/cred-identity` + 控制台「身份视图」页（侧栏名「凭据身份」，`/home/node-management/cred-identity`；
  embedded 红/unknown 中性，含 basis/source/last_seen/remedy；未装配 ⇒ 503）。
- **验收**：Go 全套 + claim 容器套件全绿（新增 14 条用例）；`contract_check` 102 路由一致；
  dashboard typecheck + **181 用例** + build 全绿。
- **控制台地址与产线板块**：见作业指导 **§12**（本机 `http://localhost:5174/?backend=…&claim=…`；
  板块 = 节点管理 → 量产签发 / 凭据身份（= 身份视图页）/ 节点 / 注册节点，以及 用户管理 → 访问令牌）。

## 4. 证据入口

- 平台侧：`backend/src/rmneo/credsource/`、`backend/src/rmneo/shadow/cmd/shadowd/cred_identity.go`
- 固件侧：`examples/oneye/korvo2_oneye/{sdkconfig.defaults.production,README.md §5.9,tools/check-sdkconfig-defaults.py}`
- 设计与作业：`backend/docs/architecture/设备凭据吊销与签发台账设计.md`（§8/§8.7.4）、`backend/docs/ops/产线凭证灌注作业指导.md`（§11）
- 台账：`docs/oneye-iot-index/设备面契约.md`（§12 #7 与量产块）
- 构建留档（gitignore 不入库）：`output/.build/korvo2_oneye-production/`、`output/.build/korvo2_oneye-guardA/out.txt`
