# Memory digest

> generated on compaction 2026-09-22T09:10:37.745Z

This is an automatically generated checkpoint condensing an earlier span of the conversation to free up context. Treat the captured context as established background and build on it without restating it. Continue the task directly from the messages that follow, without acknowledging this checkpoint.

<compacted-summary>
## Primary Request and Intent
- Standing preferences: push everything (commit+push, no staging questions); explain background/necessity in plain language before steps ("我不懂这么多术语，要尽量为我交代背景和必要性，然后告知我操作步骤"); refuses fake positives ("宁少勿假"); values recorded self-corrections (原说法保留 + 标明被推翻 + 说明为什么错).
- 2026-09-21 six-item decision (verbatim): "告警不用做签名。生产环境就在同一个服务中批量生成机器码（SN、一机一密凭证等）并做签名服务。吊销设备这个功能可以给出web访问形式，支持查看吊销名单和导入。开发板的证书进入登记簿。设备证书有效期是否能为10年或永久，不需要去维护它。要写验收脚本。申领的 HTTP 入口要做自研实现"
- DNS decisions: DP = `http://crl.oneye.me:8080/oneye-iot-device-ca.crl`; `mqtt.oneye.me` → `175.178.190.187`; "直接切换吧，用到的ip访问都可以改成域名访问".
- Claim auth decision (verbatim): "申领通过控制台页面登陆做认证然后批量签发".
- 2026-09-22 three answers via question tool: batch input = **两者都要（SN 为主，MAC 可为空）**; output = **目录/ZIP：每台证书+私钥 + CSV 台账**; credential storage = **要改：独立分区 + 产线写入（量产必须）**.
- Final user question: "⑥ 的最后一步你是否能验证" → answered yes and **completed it** (delivery fix + hardware closure).
- Goal `goal-0ab61074-4a41-440d-b168-efbb954324b0` (revision 1, phase=active, rounds 12/12 used, maxGoalRounds 12, armed). Objective = the 6 ordered items; ②③④⑤⑥ done with evidence, ① is user-side and still open.

## Key Technical Concepts
- Repo topology (all pushed): outer `E:\workspace\rainmaker-oneye` branch `esps3_korvo2_20260915` → `backend/` (eleball-oneye/oneye-iot) → `embedded/esp-adf/` (eleball-oneye/esp-adf, branch `oneye_s3_korvo`); trunk for PRs = outer `main`, backend `main`; esp-adf `master` is Espressif upstream (never PR there).
- Bed host: Tencent CVM `master2` = 175.178.190.187, reached from Windows via `ssh master2` (NOT in the DSH connection book; the book's `aliyun-8.136.141.88-admin` is a different host). Containers: `oneye-emqx` (1883/8883/8083/18083), `oneye-mtls` 18884, `oneye-crl` 18885 (CRL check on), `oneye-platca` 18886, `oneye-storage` (PG 5432). Never create/replace containers; restarts sanctioned.
- signerd: `/tmp/signerd`, 127.0.0.1:8088, token `/tmp/signerd.token`; CA `CN=oneye-iot-device-ca`, notAfter 2036-09-17T11:27:40Z.
- EMQX: `no_match=deny`, `deny_action=ignore`, ACL file in-container; **localhost is broadly allowed by the operations rule** `{allow, {ipaddrs, ["127.0.0.1","172.18.0.1"]}, all, ["$SYS/#", "#"]}` — external anonymous is what was closed in ②.
- Firmware: ESP32-S3-Korvo-2, WSL distro `Ubuntu-24.04`, IDF `/home/yiwei/esp/esp-idf-5.5.5`; flash via `powershell -File output\.build\flash-korvo2.ps1 -Port COM12 [-Erase] -BuildDir <dir>`; **must `serial_disconnect` before any esptool write**; build dirs `output/.build/korvo2_oneye-xtensa-esp32s3-elf-gcc-14.2.0` and `output/.build/korvo2_oneye-devcreds`.
- Tool-harness pitfalls (repeat offenders): PowerShell→ssh mangles quotes/`$var`/Chinese — always put shell logic in a script file and scp it, `sed -i 's/\r$//'` after upload; `.NET ReadAllText`/`WriteAllText` need absolute paths (process cwd ≠ PS location); `pkill -f shadowd` kills your own ssh session (use `[s]hadowd`); multi-line `git commit -m` breaks — use `git commit -F <file>`; ports must be released before flashing.
- Go env: `go build ./src/...` OOMs — use narrow package sets and `-p 2`.

## Files and Code
- `backend/src/utils/credsimage/credsimage.go` (+ `_test.go`, 10 tests): partition image format — 20-byte header `magic "ONEYECR1" | u16 version | u16 flags(bit0=has key) | u32 payloadLen | u32 CRC32/IEEE`, then UTF-8 JSON payload (node_id/sn/mac/cert_pem/key_pem/ca_pem/ca_id/serial/issued_at). `Parse` must accept **padded** images (`len(img) >= HeaderLen+n`, CRC over `HeaderLen:HeaderLen+n`); `PayloadLen`, `Filled`, `Describe`, `ErrNoKey`, `DefaultPartitionSize = 16*1024`.
- `backend/src/tools/mkcreds/main.go`: `-zip <batch.zip> -out <dir>` → per-SN padded `creds.bin` + `creds-manifest.csv` (`sn,node_id,offset,size,crc32,sha256,file`; skip no-key devices with a recorded reason; fail only if zero images); `-verify <bin>` → prints node_id/sn/mac/serial/has-key/size/CRC. Default offset `0x510000`, size 16384.
- `backend/src/claim/handlers/claim_handler/claim_batch.go`: `POST /v1/claim/batch` (selfhosted only, admin JWT), `claimBatchResource`, `maxBatchItems=500`, `deviceTrustBundleEnv = "DEVICE_TRUST_BUNDLE_FILE"`, `deviceTrustBundle()` (returns bundle+source or error), `issueBatchContext.trustBundle`, `issueBatchItem` (idempotency via `store.GetByNodeID`, `allow_reissue`, CSR-or-server-generated key, ledger-write-before-deliver), statuses `issued`/`already_issued`/`failed`, exits 200/207/422, `batchBinaryResponse` (base64+IsBase64Encoded, `X-Oneye-Batch-Ok/Failed`), input validation runs **before** infrastructure, `validSN` charset + `..` rejection.
- `backend/src/claim/handlers/claim_handler/claim_http.go`: `AUTH_SECRET` → `jwtAuthenticator` (issuer `oneye-iot`, requires `aud=admin`), identity conflict ⇒ refuse start (`httpgateway.ErrIdentityConflict`).
- `backend/src/rmneo/runtime/httpgateway/gateway.go`: `Authenticator` interface, identity verified before handler (401), demo headers ignored when real auth set.
- `backend/src/rmneo/devicecert/{devicecert.go,pgstore/pgstore.go}`: added `GetByNodeID(ctx, nodeID)` (mem scan; pg `WHERE node_id=$1 AND revoked_at IS NULL ORDER BY not_after DESC LIMIT 1`, no dedicated index).
- `backend/src/rmneo/devicecrl/devicecrl.go` (+tests 12): CRL verdict (allowed/revoked/unavailable), signature must verify against trust anchor, expiry ⇒ fail-closed, hot reload, `Reload`.
- `backend/src/rmneo/shadow/httpapi/{revocation.go,middleware.go,httpapi.go}`, `cmd/shadowd/main.go`: device-face revocation gate (401 revoked / 503 unavailable), env `DEVICE_CRL_FILE|URL`, `DEVICE_CRL_REFRESH`, `DEVICE_CRL_REQUIRED`, metrics `shadowd_devicecrl_*`; `connectMQTT` now sets `EMQX_CLIENT_ID`/`EMQX_USERNAME` default `oneye-shadowd`.
- `embedded/esp-adf/examples/oneye/korvo2_oneye/main/device_creds.{c,h}`: reads `creds` partition, header/CRC/cJSON validation, `NOT_FOUND` ⇒ fallback allowed, corrupt ⇒ caller must not fall back; `korvo2_oneye_main.c` tries partition first, sets device_id/username/client_id from the partition's node_id, refuses to connect when corrupt; `partitions.csv` adds `creds, data, 0x40, 0x510000, 16K`; `main/CMakeLists.txt` adds `device_creds.c` + `json`.
- Scripts: `backend/scripts/ops/{claim-batch-acceptance.sh, claim-auth-acceptance.sh, shadowd-crl-acceptance.sh, mkcreds-acceptance.sh, apply-emqx-acl-shadowd.sh, probe-emqx-acl2.sh, claim-entry-acceptance.sh}`.
- Docs: `backend/docs/architecture/设备凭据吊销与签发台账设计.md` (§3.3.2 closure, §5 #12 closed, §7.4/7.5/7.6, §8.1–8.6 incl. hardware evidence + trust-bundle closure), `contracts/domain/设备凭据与CA.md`, `docs/ops/域名切换与CRL运维.md` §十一, `docs/architecture/系统架构设计.md` v0.86–v0.88.

## Errors and Fixes
- `/v1/crl` returns raw PEM (not JSON) → empty CRL file → `parse CRL: x509: malformed crl`; fixed by writing PEM directly.
- `PORT` is a listen address (`:8080`), bare `18090` ⇒ `missing port in address`.
- Assertions written as "≠ bad value" gave false passes (`000`); now assert expected values.
- Cloning httptest transport and replacing `TLSClientConfig` drops RootCAs ⇒ `unknown authority`; mutate a clone instead.
- Claim container refused to start: `CLAIMING_CA_ID` default `claiming-ca` vs signer `oneye-iot-device-ca` ⇒ `refusing the mismatch` (guard working, not a defect).
- `Parse` rejected padded images ⇒ "verify always says corrupt"; fixed + regression test.
- `mkcreds` aborted whole batch on one keyless device; now skips + records.
- `zip` CLI absent on bed; python zip failed with pre-1980 timestamps → use `ZipInfo(..., date_time=(2026,9,22,12,0,0))`.
- Trust bundle env set but file not mounted ⇒ 500 `device trust bundle is unusable` (guard verified); fixed with `-v /home/ubuntu/mtls-test/certs/ca.crt:/trust/ca.crt:ro`.
- Round-12 misjudgment: the checked ZIP was stale (`out/batch-first.zip` came from an older run because the repo script lacked the save line) ⇒ wrongly concluded "env didn't take effect". Fixed by saving the first response with a `BATCH_ZIP_SAVED` flag; recorded in docs/script comments.
- Round-9 self-correction: "board didn't come back online after blanking the partition" was **wrong** — my 22 s check window was too short; device reports `link_up=True` and broker shows `korvo2-0001`. Original claim retained in docs with the correction.
- Earlier wrong diagnosis: blamed the tightened ACL for shadowd's subscribe failure; probing proved localhost is allowed by the ops rule; also my hand-built CONNECT omitted the username flag bit (0x80).
- `serial_read` returns a fixed early-boot window (misses the decisive lines) — prefer `serial_expect` or broker/device-side readback.

## Pending Jobs
- ① (user-side, still open as of last check): Tencent Cloud SG must close inbound **5432** and **18083**; keep **8080** (CRL DP) and **18885** (board) open; review 1883/8883/18884/18886.
- §8.6 decisions still open: where the private key is generated (server vs production line), node_id == SN?, partial-failure policy, eFuse anti-cloning, partition encryption, production-line network isolation.
- Optional/known gaps: shadowd not deployed (must set `DEVICE_CRL_FILE|URL`; `DEVICE_CRL_REQUIRED=1`); claim entry has no TLS; no token revocation; `GetByNodeID` lacks an index; batch path doesn't write `devices` registry or `node_id_reservations`; app partition only ~1% free (`0x76d0`).
- PRs never opened (no `gh`, no token): outer `https://github.com/eleball-oneye/rainmaker-oneye/compare/main...esps3_korvo2_20260915?expand=1`, backend `https://github.com/eleball-oneye/oneye-iot/compare/main...esps3_korvo2_20260915?expand=1`.

## Current Work
- Just closed ⑥'s final step and pushed: `backend 7672ad9` (commit message titled "feat/dcos(claim,fw): ⑥ 闭环 —— 交付物带\"设备信任包\"，并在真开发板上以分区身份上线") and outer pointer `3f4fb6a`; `md_link_check` 150 files OK; deleted the throwaway `scripts/ops/dbg-trust-bundle.sh`.
- Verified chain: delivered `ca.crt` 595 B/1 cert → **1772 B/2 certs** (`CN=oneye-dev-ca` + `CN=oneye-iot-device-ca`); `mkcreds` image readback OK; written to board `0x510000`; device `link_up=true transport=mqtt-tls`; broker shows `Client(KORVO2-9001, username=KORVO2-9001, …, connected=true, subscriptions=3)` — identity exists only in the partition.
- Board state: running the round-8 firmware with the KORVO2-9001 credentials partition; previously-verified fallback (blank partition → `korvo2-0001`) and fail-closed (corrupt → `cloud.link_up=false`) paths were also demonstrated.

## Next Step
- Report to the user that ⑥'s last step is verified (delivery + hardware closure), list the pushed shas, and re-state ① as the only remaining (user-side) item; then read the goal state and decide completion (rounds are at 12/12 with ① still open).

## Critical Context
- ② evidence (external): anonymous `rmng/dev/#` denied; own down-face allowed; other-node/own up-face denied; anonymous publish logged `not_authorized`; e2e device→shadowd delivery observed; live broker runs the repo ACL + `no_match=deny`.
- Live exposure measured from outside at round 7/11: 5432 OPEN (documented creds verified `LOGIN_OK oneye oneye`), 18083 OPEN (EMQX Dashboard), 1883/8883/18884/18886 OPEN, 8080 OPEN (needed), 18885 OPEN (needed).
- ACL shipped (`deployment/dev-stack/emqx/acl.conf`): `%u`→`${username}` fix, host rule `{ipaddrs,["127.0.0.1","172.18.0.1"]}`, `^oneye-pt` tap rule, new `^oneye-shadowd$` subscribe/publish rules (unverified from non-localhost), ends `{deny, all}.`
- Gate results: `contract_check.py` 97 routes (claim routes are outside shadowd OpenAPI); `md_link_check.py` 150 md files; claim_handler Ginkgo suite 80/80; devicecrl 12/12; credsimage 10 tests.
- Bed acceptance results: batch issuance `PASS 19 · FAIL 0`; shadowd CRL `PASS 16 · FAIL 0`; claim auth `PASS 9 · FAIL 0`; claim-entry gate `PASS 54 · FAIL 0 · PEND 0`.
- Board identity facts: `device_id = username = client_id` now sourced from the partition; device trust bundle needed for server verification; ACL username regex `^[A-Za-z0-9._-]{1,64}$` matches `KORVO2-9001`.
- Two durable judgment lessons recorded in docs/scripts: value-returning shell functions must only emit the return value on stdout; "save the first response" must happen on the first call (later calls return thin ZIPs).
</compacted-summary>
