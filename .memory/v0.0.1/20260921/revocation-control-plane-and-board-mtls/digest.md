# Memory digest

> generated on compaction 2026-09-21T09:55:54.119Z

This is an automatically generated checkpoint condensing an earlier span of the conversation to free up context. Treat the captured context as established background and build on it without restating it. Continue the task directly from the messages that follow, without acknowledging this checkpoint.

<compacted-summary>
## Primary Request and Intent
- Original: add git submodule `git@github.com:ELEboysss/oneye-iot-index.git` under `embedded\esp-adf\docs`; mirror `oneye-iot-index` into `E:\workspace\oneye-wiki-content\resource\物联云平台\API契约` with an md index page; keep architecture map and implementation aligned; "将改动都推送".
- User 2026-09-21 six-item decision (verbatim): "告警不用做签名。生产环境就在同一个服务中批量生成机器码（SN、一机一密凭证等）并做签名服务。吊销设备这个功能可以给出web访问形式，支持查看吊销名单和导入。开发板的证书进入登记簿。设备证书有效期是否能为10年或永久，不需要去维护它。要写验收脚本。申领的 HTTP 入口要做自研实现"
- Standing preferences: push everything (commit+push, no staging questions); plain-language background/necessity before operating steps ("我不懂这么多术语，要尽量为我交代背景和必要性，然后告知我操作步骤"); refuses fake positives ("宁少勿假"); values recorded self-corrections (原说法保留 + 标明被推翻 + 说明为什么错).
- Goal `goal-b9ddb653-403d-43b2-823c-f4fd30a41956` is **phase=complete** (revision 2, 10/14 rounds, activation disarmed).

## Key Technical Concepts
- Repo topology: outer `E:\workspace\rainmaker-oneye` branch `esps3_korvo2_20260915` → `backend/` (eleball-oneye/oneye-iot), `embedded/esp-adf/` (oneye_s3_korvo) → `components/oneye-dev-sdk` (main), `docs/oneye-iot-index` (main → ELEboysss/oneye-iot-index); separate `E:\workspace\oneye-wiki-content` (main).
- HEADs pushed, all ahead/behind 0/0: outer `2e8d6fe`, backend `891dcc1`, esp-adf `cfe51873`, SDK `90b5a50`, index `33defee`, wiki `73a7ef7f` (wiki md index also has `73a7ef7`).
- CVM `master2` = 175.178.190.187; beds `oneye-mtls` 18884 (CRL off), `oneye-crl` 18885 (`ENABLE_CRL_CHECK=true`), `oneye-platca` 18886 (off), `oneye-emqx`, `oneye-storage` (PostgreSQL 16.15). Never create/replace those 5 containers; restarting `oneye-crl` is sanctioned (disconnects devices, they retry).
- Two CAs: dev CA `CN=oneye-dev-ca,O=oneye-dev` at `/home/ubuntu/mtls-test/certs/{ca.crt,ca.key}`; platform CA `CN=oneye-iot-device-ca` at `/home/ubuntu/oneye-platform-ca`, notAfter **2036-09-17T11:27:40Z** (irreplaceable).
- Broker trust anchor `/home/ubuntu/mtls-test/certs/ca.crt` now holds **2 certs** (dev CA + platform root). Decoupled: `openssl.cnf` `certificate = /home/ubuntu/crl-dp/dev-ca-only.crt`.
- CRL DP served by `python3 -m http.server 8080 --directory /home/ubuntu/crl-dp/www`; hairpin iptables rule in `/home/ubuntu/crl-dp/fix-hairpin.sh` (host cannot reach its own public IP).
- Signer defaults: `leaf_days=3650`, `batch=up to 500`, `crl_days=7`, `SIGNER_ADDR=127.0.0.1:8088`, token at `/tmp/signerd.token`.
- Key boundary: **Go's `x509.Verify` does not check CRLs** (`VerifyOptions` has no `CRLs` field) ⇒ only a real broker can answer "is this cert still usable"; a CRL only applies to certs issued by that CA.

## Files and Code
- `backend/src/rmneo/ca/ca.go`: `DefaultLeafValidDays = 3650`, `MaxLeafValidDays = 36500`, `CertNotAfter()`, clamp `notAfter` to `c.cert.NotAfter`, added `BasicConstraintsValid: true, IsCA: false`.
- `backend/src/utils/certissuer/certissuer.go`: `DeviceCertValidity = 10 * 365 * 24 * time.Hour`.
- `backend/src/rmneo/ca/signer/{server.go,client.go,batch.go,revoke.go,ui.go}`: `DefaultValidityDays`, `MaxBatchCount`, `leafDays()`, `checkRequestedValidity()` (400 not 502), `POST /v1/batch`, `GET /v1/revoked`, `POST /v1/revoke`, `GET /v1/crl`, `GET /ui` (Basic auth, username ignored), `CRLNextUpdateDays` (default 7, max 365), `errNoLedger`→500 vs 502, `MaxResponseBytes`/`MaxBatchResponseBytes`.
- `backend/src/rmneo/ca/crl.go`: `BuildCRL`, `CRLPEM`, `ParseSerial` (hex only), `ReasonCode`, `DescribeCRL`, `RevokedEntry` — first `x509.CreateRevocationList` in the repo.
- `backend/src/rmneo/devicecert/{devicecert.go,pgstore/pgstore.go}`: `Store.Revoked(ctx)` + mem/PG impls.
- `backend/src/tools/{signerd,machinecode}/main.go`: `SIGNER_BATCH_MAX`, `CRL_NEXT_UPDATE_DAYS`, `leafValidityDays()`, `batchMaxCount()`; machinecode exports `ca.pem`+`<SN>/{cert,key}.pem`+`manifest.csv`+`manifest.json`.
- `backend/src/rmneo/reservation/{reservation.go,pgstore/pgstore.go}`, `backend/src/claim/claimbuild/claimbuild.go`, `backend/src/claim/ca_bootstrap/claimingconfig.go`: `CLAIM_BACKEND` (unset/`aws`|`selfhosted`, invalid ⇒ refuse start), `CLAIMING_CONFIG_FILE`, node binding.
- `backend/scripts/ops/signer-acceptance.sh` (100755), `broker-crl-ab.sh` (100755), `claim-entry-acceptance.sh` (100755), `gates-mutation-check.sh` (100755), `oneye-crl.sh` (100644, pre-existing).
- `backend/scripts/e2e/claim-http-selfhosted-e2e.sh` (100755).
- `backend/docs/ops/域名切换与CRL运维.md` §五 (cron retirement), §七–§八 (service-generated CRL, A/B/A′ evidence, §8.1 one-click gate).
- `backend/docs/architecture/设备凭据吊销与签发台账设计.md` §7.2, §7.3 (⑭–⑲), §5 #12.
- `backend/docs/architecture/系统架构设计.md` §6 version row **v0.81** + header line.
- `backend/deployment/signer/README.md` (routes, env table, EKU difference table), `backend/deployment/container/README.md` (`CLAIM_BACKEND`).
- `embedded/esp-adf/components/oneye-dev-sdk/src/internal/oneye_osal_tls_mbedtls.c`: `ONEYE_TLS_DIAG`, `oneye_tls_fail(site, ret)`, logs raw mbedtls code + `mbedtls_ssl_get_verify_result`.
- `embedded/esp-adf/examples/oneye/korvo2_oneye/MTLS-接入与排障.md` (new), `main/certs/` (gitignored).
- `embedded/esp-adf/docs/oneye-iot-index/index.html` + `设备面契约.md`: 365→3650 fixes, 2026-09-21 record block.

## Errors and Fixes
- `CA.IssueDeviceCert` never clamped leaf lifetime to CA expiry (only `certissuer` did) → added clamp + `TestLeafValidity`.
- `"already revoked"` undetectable: `RevokedAt.Before(now)` was always false because `Revoke` stores `now` → rewrote to read-before-write.
- `/v1/revoked` returned 502 with no ledger (config error) → `errNoLedger`→500; absurd `validity_days` 502→400.
- `signer.Client` capped responses at 1 MB (a 500-device batch truncates) → `doLimit` per call + refuse truncated body.
- Board `rc=-6` (`ONEYE_OSAL_ERR_TLS`) looked like a cert problem; real cause after adding TLS diagnostics: `mbedtls_ssl_setup: ret=-32512 (0x7f00)` = `MBEDTLS_ERR_SSL_ALLOC_FAILED` → fix `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y` + `CONFIG_MBEDTLS_DYNAMIC_BUFFER=y`.
- Changing SDK source + `idf.py build` silently reuses prebuilt `output/<tcid>/lib/liboneye_dev_*.a` (binary mtime unchanged) → must run `./build-all.sh --toolchains esp32s5@5.5.5` first (esp32s3@5.5.5).
- On-disk `sdkconfig` (TCP/1883) did not produce the flashed (mqtt-tls/18885) image → don't use it as a baseline.
- Broker gate's `accepted` criterion used `CONNECTED` (only TCP) → `Cipher is (NONE)` on failure meant revoked certs read as accepted; fixed to require MQTT CONNACK `20 02 00 00`.
- `docker restart` + `grep 'Listener … started'` matched the pre-restart log (docker logs persists across restart) → probe hit a down broker (`write:errno=104`); fixed to require a **new** started line + TCP port probe.
- Script counting bug: `JQ` mishandled `[N]` vs `[]`; PEM is multi-line so `grep -c .` counts lines not certs → python blocks + `|length`.
- `broker-crl-ab.sh` default `NODE=korvo2-0001` (the live board) polluted the ledger with revoked rows → default now `acct-gate`.
- `openssl` cron for the platform CA would overwrite the service-generated CRL with an empty list (= silent mass un-revocation) → retired (commented out in `/etc/cron.d/oneye-crl`).
- Deprecated/retracted: restoring `index.txt` by only flipping status to `V` keeps it revoked (must clear the revocation date); `PGSTORE_CONCURRENCY` expects count ≥2 not 1; `scp` onto running binary fails; `esptool write_flash '@flash_args'` needs `workdir`=build dir.
- PowerShell traps: `Get-Content` misreads UTF-8 (mojibake); `Set-Content` writes BOM; `powershell -File` reads as ANSI; `wsl -e bash -lc "<multi-line>"` swallows output; heredocs unsupported; inline `$(...)` and `"` in `-m` messages get mangled → use script files / `git commit -F <file>` / `[IO.File]::WriteAllText(..., UTF8Encoding($false))`.
- Misjudged `failAfter`/`invisibleStore` fakes needed `Revoked`; three test fakes + claim test fake updated.

## Pending Jobs
- Push subagent commit `3c23afa` (backend, 1 file / +1 −1, fast-forward descendant of origin, **not pushed**) and re-bump the outer `backend` pointer — I stopped tool use when the goal completed.
- User decisions awaited: (1) EKU shape inconsistency (`/v1/sign` carries `clientAuth` EKU, claim path does not) — design §5 #12, gate reports **PEND**; (2) exact meaning of "告警不用做签名" (I assumed "no alerting" ⇒ 7-day CRL validity).
- Deferred by user: domain cutover (`crl.oneye.me` vs `prod.oneye.me/crl/...`), who may use the revoke web UI, production TLS/auth for the claim service.
- Known unverified: claim service has no TLS/mTLS of its own; caller identity is a PoC (`RMNG_DEMO_IDENTITY`); `/v1/claim/verify`-end-to-end cert never used for a real broker handshake (only `/v1/sign-pub`-shaped cert was); capability bits per-authorization unimplemented; multi-instance untested; quota count+create is non-atomic.

## Current Work
- Goal marked complete; final report delivered. Subagent `35ecd54d-ada1-4efa-a891-54d5c1a71b88` reported its last commit `3c23afa` fixing a stale evidence cell in design §5 #12 ("PASS 52 · FAIL 1" → PEND wording, exit 0).
- Subagent's gate re-run evidence: `PASS 52 · FAIL 0 · PEND 1 · SKIP 0` exit 0; deliberate false `CLAIM_URL` → `PASS 13 · FAIL 33 · PEND 1 · SKIP 1` exit 1 (proves PEND does not affect the exit code); `bash -n` SYNTAX_OK; `md_link_check` OK (150 files); `contract_check` 97 routes; no reset/amend/rebase.

## Next Step
- On the user's word, push `3c23afa` (`git push origin esps3_korvo2_20260915` in `backend`) and commit/bump the outer `backend` pointer.

## Critical Context
- Verified evidence: service gate `/tmp/signer-acceptance/assertions.log` 64 lines / **PASS 64 · FAIL 0**, `summary.txt` with A/B/A′; broker gate `/tmp/broker-crl-ab/` (summary + 8 probe artifacts); mutation `/tmp/gates-mutation3/` **6/6 correct failures**; claim gate `/home/ubuntu/oneye-claim-http-e2e/{e2e.out,ops-run.log,ops-out/}` 47+52 assertions; EKU probe `/tmp/eku-probe/` + persistent copy `/home/ubuntu/oneye-claim-http-e2e/eku-probe/` (`diff -r` identical).
- Real-hardware A/B/A′ (board `korvo2-0001`): cert `bd74b45c6d4e20f597e24f4c4f440e8e` → `link up: 175.178.190.187:18885 transport=mqtt-tls`; after `/v1/revoke` + republish + restart → broker `SERVER ALERT: Fatal - Certificate Revoked` (Erlang `{tls_alert,{certificate_revoked,…}}`); re-issued `28895387463c8baea6d03e738dee150d` → `link up` again. Ledger: old `revoked=t`, new `revoked=f`. Board currently online.
- Ledger accumulated ~10 rows for `korvo2-0001` (5 revoked/live pairs) from repeated gate runs with the old default NODE — hence the `acct-gate` default.
- `--broker` run result: `PASS 68 · FAIL 0 · SKIP 0`; `/v1/info`: `leaf_days=3650 batch=up to 500 crl_days=7 ca_not_after=2036-09-17T11:27:40Z`; platform CRL has ~20 revocations, nextUpdate +7 days; dev CA CRL 0 revocations, +1 day.
- Bed health confirmed: 5 containers up, no leftover one-shot containers, platform-CA cron retired (0 lines).
- EKU finding: `/v1/sign-pub` leaf = `KeyUsage(critical, DigitalSignature)` + `BasicConstraints(CA:FALSE)` + SKID + AKID + CRLDP, **no ExtendedKeyUsage**; `/v1/sign` leaf adds `EKU: TLS Web Client Authentication`. EKU-less cert accepted by real broker (`CONNACK(..., ReasonCode=0)`, `SERVER ALERT` count 0).
- Gate criterion pitfalls documented and enforced: `CONNECTED` ≠ accepted (need CONNACK); `docker logs` persists across `restart` (need new started line + port probe).
- Build facts: WSL-native IDF at `/home/yiwei/esp/esp-idf-5.5.5` (the `/mnt/e/esp/...` copy has CRLF); `ADF_PATH=/mnt/e/workspace/rainmaker-oneye/embedded/esp-adf`; build dir `output/.build/korvo2_oneye-xtensa-esp32s3-elf-gcc-14.2.0`; flash via `output/.build/flash-korvo2.ps1 -Port COM12` (serial session must be disconnected first); firmware 2% partition headroom.
- Process lessons recorded: don't `git reset --hard` in a repo another agent is actively editing (have the in-place agent do it); verify push/force decisions against real git state rather than trusting another agent's report; a gate that can never fail is worse than no gate.
- `devicecert.Store` gained `Revoked(ctx)`; all repo scripts are `100644` by convention except the three new gates (100755); `.gitattributes` enforces `*.sh text eol=lf`.
</compacted-summary>
