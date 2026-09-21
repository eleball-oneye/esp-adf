# Memory digest

> generated on compaction 2026-09-21T07:49:24.985Z

This is an automatically generated checkpoint condensing an earlier span of the conversation to free up context. Treat the captured context as established background and build on it without restating it. Continue the task directly from the messages that follow, without acknowledging this checkpoint.

<compacted-summary>
## Primary Request and Intent
- Original (turn 1): add git submodule `git@github.com:ELEboysss/oneye-iot-index.git` under `embedded\esp-adf\docs`, copy master `index.html` into it; then "梳理oneye-dev-sdk的物联协议实现以及嵌入式设备能力，将设备面契约更新到…oneye-iot-index的合适部分，并在index.html体现，包括mqtt topic、ws/wss、配网、一机一密、平台鉴权等，确保架构地图和实现保持一致"; "分析一下架构设计和实际实现哪些地方是需要调优的"; "缺陷进行落地修复并同步设计文档和架构index"; mirror `oneye-iot-index` into `E:\workspace\oneye-wiki-content\resource\物联云平台\API契约` with an md index page; "更新后端和嵌入式的docs，务必和实际实现对齐"; "将改动都推送".
- Goal 2 checklist (verbatim): "①重建 C3/IDF6.0.3 归档 2. 可做 3. 做 4. 做 5.全跑 6.用SNTP 7.选b自研自托管 8. 先实现，后续重新签 9.目前域名和证书未定，以后会部署反代和生产环境 10.国内没有AWS环境，真的需要的话只考虑自研" — all of ①–⑧ closed with hardware/counter-example evidence; ⑨ deferred by user; ⑩ honored. Marked **complete**.
- Later: `.memory` 落档进 git（并保留在库）; "我们写的example要进git"; "反代等域名确定后再落地"; "目前没有生产设备，删除所有存量证书，当前在用的开发板可以使用新证书"; "searchThings没有前置依赖项的话可以做了".
- Then (goal `goal-b06ffb0f`, complete): 域名 `oneye.me`（服务将解析到 `prod.oneye.me`，**当前不切**）+ 钢印方案 B（单实例签名服务）。
- Then user asked (verbatim): "接真库验证，如果需要外部支持可告知我。平台CA在正式生产环境中还需要的的条件是那些？是否还有其他需要关注和待办/" and "我不懂这么多术语，要尽量为我交代背景和必要性，然后告知我操作步骤".
- **Latest decisions (verbatim)**: "告警不用做签名。生产环境就在同一个服务中批量生成机器码（SN、一机一密凭证等）并做签名服务。吊销设备这个功能可以给出web访问形式，支持查看吊销名单和导入。开发板的证书进入登记簿。设备证书有效期是否能为10年或永久，不需要去维护它。要写验收脚本。申领的 HTTP 入口要做自研实现"
- User preference: plain-language explanation of background/necessity before operating steps.

## Key Technical Concepts
- Repo topology: outer `E:\workspace\rainmaker-oneye` branch `esps3_korvo2_20260915` → `backend/` (`eleball-oneye/oneye-iot`), `embedded/esp-adf/` (`oneye_s3_korvo` → `eleball-oneye/esp-adf`) → `components/oneye-dev-sdk` (`main`), `docs/oneye-iot-index` (`main` → `ELEboysss/oneye-iot-index`); separate `E:\workspace\oneye-wiki-content` (`main`).
- HEADs (verified clean, ahead=0 behind=0): outer `54e7a42`, backend `1b6a85a`, esp-adf `3d5ee54a`, SDK `979add1`, index `fc546ed`, wiki `b48e390`.
- CVM `master2` = `175.178.190.187` (ins-5f3girvy), all ports now reachable (ICMP + TCP); cannot hairpin to its own public IP (so `curl` of the public CRL URL from the host fails — checks must run externally, e.g. from WSL).
- Beds: `oneye-mtls` 18884 (CRL check off), `oneye-crl` 18885 (`ENABLE_CRL_CHECK=true`), `oneye-platca` 18886 (off), `oneye-emqx`, `oneye-storage` (PostgreSQL **16.15**, db `oneye`, user `oneye`, password `oneye_dev`, 5432 published).
- Two CAs: dev CA `CN=oneye-dev-ca` at `/home/ubuntu/crl-dp` (board's cert serial `2000`); platform CA `CN=oneye-iot-device-ca` at `/home/ubuntu/oneye-platform-ca` (irreplaceable; do not re-mint).
- **"钢印方案 B" implemented**: `signerd` single instance holds CA private key via read-only mount; `CA_LEAF_VALIDITY_DAYS` default in service is 3650 days; leaves clamped to CA notAfter (**2036-09-18**) — "permanent" is impossible.
- Signature service defaults: `SIGNER_ADDR=127.0.0.1:8088`, `SIGNER_TOKEN` ≥16 chars required, ledger (`PG_DSN` or `SHADOW_STORE_MEM=1`) required, `CLAIM_ISSUER=selfhosted` required; five fail-closed startup checks verified.
- Two signing endpoints: `POST /v1/sign` (service generates device key, returns cert+key) and `POST /v1/sign-pub` (signs caller-supplied ECDSA P-256 public key, cert only, **no private key in response**).
- Claim path: new mode `CLAIM_ISSUER=selfhosted-remote` + `SIGNER_URL`/`SIGNER_TOKEN`; `issuerbuild.SelfHosted()` covers both self-hosted modes; remote mode needs no local CA_DIR or ledger.
- CRL model (converged): broker must hold a CRL relevant to the peer cert's issuer, else refuses; **no fail-open**. Republishing a CRL has no effect until cache expiry/restart.
- CRL validity was changed on the bed from `nextUpdate=2036` to **1 day** (both CAs) with daily cron; alerting explicitly NOT to be built → open question about a fallback (log-only vs 7-day validity).
- Known gaps found: **no Go-side CRL generator exists** (`x509.CreateRevocationList` absent) — needed for the revoke web UI; `certissuer.DeviceCertValidity = 100*365*24h` but `ca.IssueDeviceCert` default = 365 days.
- `/v1/claim/verify` still cannot complete (needs AWS DynamoDB reservation table + SSM claiming config) → user decided: **do a self-hosted implementation**.

## Files and Code
- `backend/contracts/api/mqtt/传输规范.md` §8.1: main ⑤ device-face/downlink record — three-state verification, log/up-traffic deviation, batch B execution, refuted "irrelevant CRL ⇒ reject" expectation.
- `backend/docs/architecture/设备凭据吊销与签发台账设计.md`: §3.6.1 batch-B operation sheet + execution results + real-DB ledger verification table; §3.7.2 single-instance signer (endpoints, fail-closed list, bed evidence, uncovered items, self-inflicted traps).
- `backend/src/rmneo/ca/signer/{server.go,client.go,http.go,server_test.go,signpub_test.go}`: HTTP layer + client; routes `/healthz`, `/v1/info`, `/v1/ca`, `/v1/sign`, `/v1/sign-pub`.
- `backend/src/tools/signerd/main.go`: service entry, env parsing, fail-closed startup, `-selfcheck`, `Server.Issuer` wired from `issuerbuild.Load(...).Issuer`.
- `backend/src/tools/issuecert/main.go`: `-signer <url>` / `-signer-token` remote path (`runRemote`).
- `backend/src/claim/issuerbuild/issuerbuild.go` + `issuerbuild_remote.go` + `issuerbuild_remote_test.go`: `ModeSelfHostedRemote`, `SelfHosted()`, `RemoteMode()`, `loadRemote()`, `remoteIssuer` (refuses non-empty `certissuer.Subject`; ECDSA P-256 only).
- `backend/src/claim/handlers/claim_handler/claim_handler_main.go`: `defaultBuildIssuer` uses `issuerbuild.SelfHosted()`; `selfHostedSigner` skips local ledger in remote mode.
- `backend/deployment/signer/{Dockerfile,signerd.service,README.md}`: host-built binary convention, non-root, read-only keystore, env table, security red lines.
- `backend/scripts/ops/oneye-crl.sh`: subcommands `check` / `check-file` / `publish` / `revoke` / `restore`; `--out` mandatory when no `--url`; preflight fails on unreachable URL, expired CRL, or `nextUpdate` > `--max-age-days` (default 90).
- `backend/docs/ops/域名切换与CRL运维.md`: domain cutover runbook (4 steps, rollback, failure triage table, cron sample, current values).
- `embedded/esp-adf/examples/oneye/korvo2_oneye/main/korvo2_oneye_main.c` + `Kconfig.projbuild`: `CONFIG_ONEYE_FW_LOG_PROBE` (default **n**) log-face forensic instrumentation.
- `embedded/esp-adf/components/oneye-dev-sdk/tests/test_log.c`: `down_ack_effect_differential` (new); header note that `down_ack_clears_pending` is a vacuous assertion; `INTEGRATION.md` counts refreshed to 20 groups / **237** cases / assertions ≈3263–3298.
- `embedded/esp-adf/.memory/.gitignore` + `v0.0.1/20260920/esp-adf-1/digest.md`: memory archived per AGENTS §8.
- Bed-side (outside repos): `/home/ubuntu/crl-dp/{www/oneye-ca.crl,www/oneye-iot-device-ca.crl,openssl.cnf,index.txt}`, `/home/ubuntu/oneye-platform-ca/{ca.crt,ca.key,openssl.cnf,db/}`, `/opt/oneye/oneye-crl.sh`, `/etc/cron.d/oneye-crl`, evidence in `embedded/esp-adf/output/.build/hwverify/*.log`, `/tmp/batchB-evidence/`, `/tmp/signerd.token`.

## Errors and Fixes
- `docker restart oneye-crl` does not reliably produce a cold CRL cache → never infer "cert lacks a DP" from cold-cache rejection.
- `PGSTORE_CONCURRENCY` / `PGSTORE_OPEN_RACE` expect a **count ≥2**, not `1`; passing `1` FAILs the gate (my usage error, not a product defect).
- Restoring a revoked entry in `index.txt` by only flipping status to `V` keeps it revoked (`entry 1: not revoked yet, but has a revocation date`); must clear the revocation-date field (awk sets `$1="V"; $3=""`).
- `crlnumber` is root-owned on the CVM (serial is ubuntu-owned) → non-root publish cannot reset it (harmless).
- `signerd` was started without `Server.Issuer` wired → `/v1/sign-pub` returned 500 `signer has no issuer configured for public-key signing`; unit tests could not catch it (tests construct `Server` directly) — found on the bed.
- `scp` onto a running `/tmp/signerd` failed (`dest open "/tmp/signerd": Failure`) so the new binary never landed and `/v1/sign-pub` 404'd; correct order: stop service → upload to `*.new` → rename → start.
- PowerShell/mojibake traps: `Get-Content -Raw` misreads UTF-8 (use `[IO.File]::ReadAllText(path, UTF8Encoding($false))`); `Set-Content` writes BOM; `powershell -File` reads UTF-8 as ANSI; `wsl -e bash -lc "<multi-line>"` swallows output (use script files via `scp`); git `-m` with embedded newlines splits into pathspecs (use `-F`).
- `esptool write_flash '@flash_args'` needs `workdir` = build dir.
- `ca.Fingerprint` takes `[]byte`; `x509.Verify` defaults to ServerAuth so device certs need `KeyUsages: []x509.ExtKeyUsage{x509.ExtKeyUsageClientAuth}`.
- `oneye-crl.sh publish` default output filename silently overwrote the other CA's CRL file → now `--out` (or `--url`) is mandatory.
- `publish --url <own public IP>` always fails on the CRL host (no hairpin) → cron uses `check-file`; external check via `oneye-crl.sh check --url`.
- Running pgstore/storebuild integration suites against the real ledger DB left 4 test rows (pushed `expiring_7d` from 0 to 3); cleaned by serial prefix.
- Inline PowerShell string surgery on Chinese source text failed at parse time and wrote nothing (verified clean) — use the editor tools instead.
- Retracted earlier claims (kept in docs): mtime-based cert inference; "no-DP escapes revocation"; "compile failure still exits 0" (tee); "220 `no_relevant_crls` unattributed"; "risk = fail-open window"; subagent's "iptables verified clean"; "⑤ only in contract" (stale).

## Pending Jobs
- ① 证书有效期改为 10 年（叶子被钳到 CA 的 2036-09-18）；"永久"不可行。
- ② 签名服务内**批量生成机器码**（SN + 一机一密密钥 + 证书 + 落台账）并导出。
- ③ **吊销 web 界面**：查看吊销名单 + 导入（批量按序列号吊销并重发 CRL）；需**新写 Go 侧 CRL 生成**。
- ④ 开发板证书进登记簿（平台 CA 经签名服务签发 + 平台根加入门卫信任名单 + 重烧）。
- ⑤ **申领 HTTP 入口自研实现**（替换 DynamoDB 号码保留表 + SSM claiming 配置，让 `/v1/claim/verify` 跑到底）。
- ⑥ **验收脚本**（吊销闭环 A/B/A、签名闭环、批量发证、申领入口，一键重跑）。
- ⑦ 域名相关：DP 地址定稿（`crl.oneye.me` 建议 vs `prod.oneye.me/crl/...`）+ DNS 生效后按 runbook 切换。
- 告警：**不做**（待确认确切含义）；兜底选项待用户选 (a) 仅日志 或 (b) 名单有效期放宽到 7 天。
- Other loose ends: SDK 内部日志不上行/固件几乎不写 `log/up`（产品决定）；`searchThings` 降级=超集且响应无机读标记；`getFieldValues` 返回 `[]`；`shadowd` 到期 gauge + `GET /v1/devicecerts/expiring` 生产接线；生产库口令/端口/TLS；`rlog` 级别调 info；测试污染真库的默认隔离；取证插桩保留/删除。

## Current Work
- Goal `goal-b9ddb653-403d-43b2-823c-f4fd30a41956` active (0/14 rounds) covering the six new items.
- Attempted the 10-year validity change via inline PowerShell string replacement; the command failed at parse time and **nothing was written** (verified: backend clean at `1b6a85a`, `DefaultValidityDays` occurrences = 0).
- Reconnaissance done: no `x509.CreateRevocationList` anywhere in the repo; `certissuer.DeviceCertValidity = 100 * 365 * 24 * time.Hour`; `ca.IssueDeviceCert` default `validDays = 365`.
- Last completed, verified work: CRL republished with 1-day validity for both CAs (explicit `--out`, correct issuers), cron installed (2 tasks, exit 0), external preflight `check` passes both URLs (`✓ 0 revocations, nextUpdate +1 day`), board reconnected after broker restart; committed backend `1b6a85a`, outer `54e7a42`.

## Next Step
- Redo the 10-year leaf-validity change with the editor tools (add `Server.DefaultValidityDays`, wire `defaultLeafDays = 3650` / `CA_LEAF_VALIDITY_DAYS` in `signerd`, use it in both `/v1/sign` and `/v1/sign-pub`), add a unit test, run gates, then proceed to the batch machine-code issuance endpoint.

## Critical Context
- User wants push-everything-directly (commit+push, no staging questions) and plain-language background+steps; refuses fake positives ("宁少勿假") and values recorded self-corrections (原说法保留 + 标明被推翻 + 说明为什么错).
- Never create/replace containers `oneye-emqx`, `oneye-storage`, `oneye-mtls`, `oneye-crl`, `oneye-platca`; restarting `oneye-crl` is acceptable but it disconnects devices (they retry).
- Adding the platform CA root to a broker's trust anchor is allowed on the test bed; in production requires user approval.
- CRL endpoint availability is on the connection critical path: with `enable_crl_check=true`, an unreachable/expired CRL ⇒ **all devices rejected** (fail-closed) — hence the daily republish matters.
- Do not sign any cert with a domain DP until the domain endpoint is verified reachable (both old and new URLs must pass the preflight).
- Ledger DB currently holds real issued certs only (e.g. `korvo2-0002`, `korvo2-window`, `korvo2-e2e`, `korvo2-signpub`); the board's dev-CA cert serial `2000` is **not** in the ledger (item ④).
- `oneye_pgstress` is a pre-existing DB not created by me; my disposable DBs (`oneye_pgtest`, `oneye_race`, `oneye_stress`) were dropped.
- Open questions for the user: (1) does "告警不用做" mean no alert system, and if so choose fallback (a) log-only or (b) 7-day CRL validity; (2) CRL hostname choice; (3) who may use the revoke web UI; (4) when `prod.oneye.me` resolves.
- Evidence locations: `embedded/esp-adf/output/.build/hwverify/` (`logface-serial.log`, `sdk-host-tests-final.log`, `batchB-serial.log`, `downlink-reverify-serial.log`), CVM `/tmp/batchB-evidence/01-before.txt`, `/tmp/signerd-evidence/`, `/tmp/signpub-evidence/`.
</compacted-summary>
