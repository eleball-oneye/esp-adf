# Memory digest

> generated on compaction 2026-09-21T08:51:22.182Z

## Primary Request and Intent
- Standing objective (originally): mirror the device-face contract into `docs/oneye-iot-index` + wiki, keep architecture map and implementation aligned, fix defects in code and docs, push everything.
- User decisions log (verbatim where quoted):
  - "域名为oneye.me，后面服务会解析到prod.oneye.me，但是目前还不能正式切到有域名的环境。钢印方案选B，尽量只用一个实例服务即可闭环。测试数据可以留着继续使用"
  - "我不懂这么多术语，要尽量为我交代背景和必要性，然后告知我操作步骤"
  - "除了域名，接下去该做的还有什么事"
  - "告警不用做签名。生产环境就在同一个服务中批量生成机器码（SN、一机一密凭证等）并做签名服务。吊销设备这个功能可以给出web访问形式，支持查看吊销名单和导入。开发板的证书进入登记簿。设备证书有效期是否能为10年或永久，不需要去维护它。要写验收脚本。申领的 HTTP 入口要做自研实现"
- **Current turn's task (⑤ of that list, self-contained)**: implement a self-hosted (non-AWS) replacement for the two cloud dependencies of `POST /v1/claim/verify` — ① DynamoDB 号码保留表 (`src/rmneo/db/node_id_reservation_db`) and ② SSM claiming 配置 (`ca_bootstrap` / `claim.go` `ParamConfig`) — so the HTTP claim path runs end to end. Then: unit tests, full gates, **real bed verification on CVM `master2`**, docs write-back (§7.3 in the design doc), commit **without push**, and report in the prescribed 6-part format.

## Key Technical Concepts
- Repo topology: outer `E:\workspace\rainmaker-oneye` (branch `esps3_korvo2_20260915`) → `backend/` (remote `eleball-oneye/oneye-iot`) + `embedded/esp-adf/` (branch `oneye_s3_korvo`) → SDK submodule (branch `main`) + `docs/oneye-iot-index` (branch `main`); separate wiki at `E:\workspace\oneye-wiki-content`.
- Current hashes: outer `54e7a42`, backend `1b6a85a`, esp-adf `3d5ee54a`, SDK `979add1`, index `fc546ed`, wiki `b48e390` (all clean/in-sync as of last check).
- Issuer modes (`issuerbuild`): `CLAIM_ISSUER` ∈ {`kms` (default), `selfhosted`, `selfhosted-remote`}; `SelfHosted()` covers both self-hosted forms, `RemoteMode()` the remote one. **Do not change these three paths.**
- Single-instance signer service: `src/tools/signerd` + `src/rmneo/ca/signer` (server+client). Routes: `GET /healthz` (no token), `GET /v1/info`, `GET /v1/ca`, `POST /v1/sign` (service generates device key), `POST /v1/sign-pub` (signs caller-supplied PKIX public key, returns cert only). Auth `Authorization: Bearer <SIGNER_TOKEN>` (min 16 chars, constant-time). Fail-closed at startup: no token / no ledger / mode≠selfhosted / keystore missing or keyless ⇒ refuse to start (5 negatives verified on bed). Requires `PG_DSN` or `SHADOW_STORE_MEM=1`; ledger write then **read-back** before returning a cert (`readBack` helper). Audit lines `signer: ISSUED …` / `signer: REFUSED …`.
- `certissuer.Issuer` interface: `Issue(ctx, pub crypto.PublicKey, p Profile) (*Result, error)`; `Profile{CAID, CommonName, Validity, Subject}`; `Result{CertPEM, ChainPEM, CAID}`; `certissuer.DeviceCertValidity = 100 * 365 * 24 * time.Hour`; claim handler already requires ECDSA P-256 from the CSR.
- `remoteIssuer` (in `issuerbuild_remote.go`) errors on empty CN and on non-zero `Profile.Subject` (refuses to silently drop organizational attributes).
- fail-closed convention: `storebuild.Configured()` / `Describe()` / `Open(ctx)`; `ca.LoadFile` never mints.
- Known real defect precedent: concurrent DDL race ⇒ `duplicate key value violates unique constraint "pg_type_typname_nsp_index"` (SQLSTATE 23505); fixed by `pg_advisory_xact_lock` inside a transaction in `devicecert/pgstore`.
- Bed: `master2` = `175.178.190.187` (ssh alias `master2`); containers `oneye-mtls` 18884, `oneye-crl` 18885 (`ENABLE_CRL_CHECK=true`), `oneye-platca` 18886, `oneye-storage` (PG **16.15**, db `oneye`, user `oneye`, pw `oneye_dev`, port 5432 published), `oneye-emqx`. Board = ESP32-S3-Korvo-2 on Windows **COM12**. `signerd` running at `127.0.0.1:8088`, token in `/tmp/signerd.token`.
- Go 1.26.6; module `github.com/espressif/esp-rainmaker-neo`.

## Files and Code
- `backend/src/claim/claim.go`, `backend/src/claim/handlers/claim_handler/claim_handler_main.go`: claim path; `defaultBuildIssuer` now uses `issuerbuild.SelfHosted()`, `selfHostedSigner` skips the local ledger requirement in remote mode, `kmsIssuer` = SSM CA cert + KMS signer (per-request). Reservation semantics to preserve: 保留是**终身上限**、记录**永不删除**、同一 MAC 的不同写法必须落到同一条保留记录.
- `backend/src/rmneo/db/node_id_reservation_db`: DynamoDB reservation store to be abstracted behind a new interface; `backend/src/claim/ca_bootstrap` + `ParamConfig` = SSM claiming config to be replaced by env/local file.
- `backend/src/rmneo/ca/signer/{server.go,client.go,http.go,server_test.go,signpub_test.go}`: signer HTTP layer (do not modify).
- `backend/src/claim/issuerbuild/issuerbuild.go`: modes/constants (`ModeSelfHostedRemote = "selfhosted-remote"`, `SignerURLEnv = "SIGNER_URL"`, `SignerTokenEnv = "SIGNER_TOKEN"`, `CADirEnv`, `CRLDPEnv`, `LeafValidityDaysEnv`, `CAIDEnv`, `DefaultCAID = "claiming-ca"`, `CADirCommonName = "oneye-iot-device-ca"`), `Signer` struct with `CA/Issuer/CADir/CRLDistributionPoint/RemoteURL/RemoteLedger`, `Load(ledger)`, `Describe()`.
- `backend/src/claim/issuerbuild/issuerbuild_remote.go`: `remoteIssuer`, `publicKeyPEM` (ECDSA P-256 only), `daysFromValidity`, `loadRemote()` (startup `Healthy`+`Info`, refuses on missing `ca_id` or `ca_id` mismatch).
- `backend/scripts/ops/oneye-crl.sh`: subcommands `check|check-file|publish|revoke|restore`; publish **requires** `--out` or `--url` (no default name — two CAs share one web dir); refuses when nextUpdate > `--max-age-days` (default 90) or expired.
- `backend/docs/ops/域名切换与CRL运维.md`: domain cutover runbook + CRL ops (do not modify).
- `backend/docs/architecture/设备凭据吊销与签发台账设计.md`: §3.6.1 batch B op sheet + results, §3.7.2 single-instance signer, §7.x review register — **add §7.3 here**.
- `backend/contracts/api/mqtt/传输规范.md` §8.1: device-face downlink status, log/up no-traffic finding, batch B record, refuted "irrelevant CRL ⇒ reject" expectation.
- `deployment/signer/{Dockerfile,README.md,signerd.service}`: signer deployment (host-built binary + alpine + chmod 0755; systemd hardened with `ReadOnlyPaths=/etc/oneye/ca`).
- `backend/scripts/e2e/claim-selfca-e2e.sh`: reference for the bed E2E style (one-shot container, self-cleaning, must not touch the five named containers).

## Errors and Fixes
- `PGSTORE_CONCURRENCY` / `PGSTORE_OPEN_RACE` expect an **integer ≥2** (concurrency count), not `1` — giving `1` fails the gate; not a product defect.
- `pgstore`/`storebuild` integration tests run against the real `oneye` DB **leave prefixed test rows** (4 rows, pushing `expiring_7d` 0→3); cleaned by prefix; destructive tests must use a throwaway DB.
- `docker restart oneye-crl` does **not** reliably produce a cold CRL cache ⇒ never infer "cert has no DP" from cache temperature.
- `index.txt` restore must clear the revocation-date field, not just flip status to `V` (`entry 1: not revoked yet, but has a revocation date`), else the serial stays revoked.
- `crlnumber` on the CVM is root-owned (writable `serial` is not) ⇒ non-root publish cannot bump it (harmless).
- CVM **cannot hairpin its own public IP** ⇒ `oneye-crl.sh check --url` on the CVM always fails; cron must use `check-file`; external checks run from WSL.
- `signerd` was missing `Issuer:` wiring ⇒ `/v1/sign-pub` returned 500 `signer has no issuer configured for public-key signing`; unit tests did not catch it (they construct `Server` directly, bypassing startup assembly).
- Overwriting a running `signerd` binary via `scp` fails (`dest open … Failure`) leaving the old binary running ⇒ order: stop service → upload `*.new` → rename → start.
- PowerShell pitfalls hit repeatedly: `Set-Content -Encoding UTF8` in PS5.1 writes a BOM (`set: command not found`); `Get-Content -Raw` mangles UTF-8 (use `[IO.File]::ReadAllText(path, UTF8Encoding($false))`); multi-line `git commit -m` breaks (use `-F file` or single-line `-m`); inline Chinese string surgery via scriptblocks/PowerShell replacement breaks the parser (this last failure wrote **nothing**; tree verified clean at backend `1b6a85a`) — use the edit tool for Chinese text.
- WSL invocation: multi-line here-strings through `wsl -e bash -lc` drop output; use a script file + `wsl -e bash /mnt/c/...`.
- `esptool @flash_args` needs CWD = build dir (use `workdir`).
- ② Irrelevant-CRL branch: a fetched-but-irrelevant CRL is inserted into cache and the connection is **still accepted** if a relevant CRL is already cached — the earlier "DP present + irrelevant CRL ⇒ reject" expectation is **refuted** and recorded.

## Pending Jobs
- **This turn**: self-hosted reservation store + claiming config for `/v1/claim/verify`; tests; gates; bed E2E; §7.3 docs; commit (no push); report.
- Remaining goal items (goal `goal-b9ddb653-403d-43b2-823c-f4fd30a41956`, 14 rounds): ①10-year leaf validity (signer default; note leaves cannot outlive the CA, which expires 2036-09-18); ②batch machine-code generation (SN + 一机一密 credentials + cert + ledger) inside the signer service; ③CRL generation code (none exists in Go yet) + revocation web UI (view revocation list, import serials); ④onboard the board's cert into the ledger via platform CA + add platform root to the broker trust anchor (bed OK, production needs user approval) + reflash; ⑤claim HTTP entry (this turn); ⑥acceptance scripts (fold A/B/A revocation closure, signer closure, batch issuance, claim entry into a re-runnable gate).
- Open user questions: confirm whether "告警不用做" means skip alerting entirely; choose CRL watchdog fallback (a) log-only or (b) widen CRL validity 1→7 days; pick CRL hostname (`crl.oneye.me` recommended vs `prod.oneye.me/crl/...`); decide where alerts go / where the signer will ultimately run / who may use the revocation UI.

## Current Work
- User asked to independently complete item ⑤ (self-hosted claim HTTP entry). Recon done: no `CreateRevocationList` usage anywhere in Go (CRL must be written later), `certissuer.DeviceCertValidity = 100 years`, `ca.IssueDeviceCert` default `validDays = 365`.
- A first attempt to change the signer's default leaf validity to 10 years via inline PowerShell string replacement failed at parse time; **nothing was written** (backend still `1b6a85a`, `DefaultValidityDays` occurrences = 0, worktree clean).

## Next Step
- Implement item ⑤: add an explicit `CLAIM_STORE=pg|mem|dynamodb` (default `dynamodb`, invalid ⇒ error) storage abstraction for reservations (PG store with `pg_advisory_xact_lock` + `CREATE TABLE IF NOT EXISTS` inside a transaction, mem store mirroring `devicecert.NewMemStore`) and replace SSM claiming config with env/local-file config; keep reservation semantics (terminal cap, never deleted, MAC-form normalization); tests for round-trip, idempotency, normalization equivalence, quota cap, concurrent first-create, fail-closed-when-unconfigured; `go build ./...`/`go vet ./src/...`/`go test ./src/...` green; bed E2E on `master2` issuing a real `POST /v1/claim/verify`; §7.3 docs; `python scripts/contract_check.py` if contracts change; commit locally **without push**.

## Critical Context
- Do not modify: `src/rmneo/ca/signer/*`, `src/rmneo/ca/crl.go`, `src/rmneo/devicecert/{devicecert.go,pgstore/pgstore.go}` (the `Store` interface now has `Revoked(ctx)`), `scripts/ops/signer-acceptance.sh`, `docs/ops/域名切换与CRL运维.md`.
- Do not restart/replace `oneye-emqx`/`oneye-storage`/`oneye-mtls`/`oneye-crl`/`oneye-platca`; `oneye-crl` restart is allowed but likely unnecessary. Do not touch `embedded/`. **Do not `git push`.**
- Bed hygiene: `oneye` DB is real; destructive tests need a throwaway DB; leave no test rows.
- Verification standard: no "verified" without real output; user demands 宁少勿假 — prefer marking items unverified over inventing evidence. Chinese comments must explain **why**; commit messages in Chinese with real evidence.
- Fail-closed style: missing/invalid config ⇒ refuse to start or refuse to sign; no silent fallbacks.
- Board state: running instrumented firmware (K186 `CONFIG_ONEYE_FW_LOG_PROBE` default off), cert = dev CA serial `0x2000` (DP `http://175.178.190.187:8080/oneye-ca.crl`), connected on 18885.
- CRL state: both dev (`oneye-ca.crl`) and platform (`oneye-iot-device-ca.crl`) CRLs republished with **1-day validity**, cron installed (`/etc/cron.d/oneye-crl`, 17:3 and 23:3 daily, no `--url`), script at `/opt/oneye/oneye-crl.sh`; external check passes for both URLs.
- Ledger state (real PG `oneye.device_certs`): rows for `korvo2-0002`, `korvo2-window`, `korvo2-e2e`, `korvo2-signpub`; board's cert serial 2000 is **not** in the ledger; `/v1/claim/verify` cannot complete without this turn's work.
- Reported evidence highlights: signer E2E on bed issued `korvo2-e2e` (serial `c48f9ada09f9a5fcfbc6a3fe5f2c24bf`), `/v1/sign-pub` issued `korvo2-signpub` (serial `10a3f759c3601900f5a0930e2804b789`, cert public key byte-identical to the device's local `${"pub"}` SHA-256 `6ea99245eb6ae695bcbe0933e88717e762a665fce98cc42f5bc744b259bcd465`), RSA public key ⇒ HTTP 400; 40 concurrent `Open()` on a fresh DB ⇒ 0 failed / 40 ok; 100k rows loaded in 1.692 s (59,098 rows/s); 32 same-serial ⇒ exactly 1 row.
