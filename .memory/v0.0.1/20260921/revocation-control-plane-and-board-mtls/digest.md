# Memory digest

> generated on compaction 2026-09-22T06:56:27.661Z

## Primary Request and Intent
- Standing: push everything (commit+push, no staging questions); plain-language background/necessity before steps ("我不懂这么多术语，要尽量为我交代背景和必要性，然后告知我操作步骤"); refuses fake positives ("宁少勿假"); values recorded self-corrections (原说法保留 + 标明被推翻 + 说明为什么错).
- 2026-09-21 six-item decision (verbatim): "告警不用做签名。生产环境就在同一个服务中批量生成机器码（SN、一机一密凭证等）并做签名服务。吊销设备这个功能可以给出web访问形式，支持查看吊销名单和导入。开发板的证书进入登记簿。设备证书有效期是否能为10年或永久，不需要去维护它。要写验收脚本。申领的 HTTP 入口要做自研实现"
- DNS decisions: DP = `http://crl.oneye.me:8080/oneye-iot-device-ca.crl` (chosen, over port-80/https alternatives); `mqtt.oneye.me` repointed from a dead CLB to `175.178.190.187`.
- "直接切换吧，用到的ip访问都可以改成域名访问" → CRL DP switched, board reflashed with domain endpoint + re-issued domain-DP cert.
- "提交记忆库改动，并把所有改动push，让我pr合入主干" → memory committed/pushed; PR cannot be created (no `gh`, no token); compare URLs supplied.
- "未做 / 待我拍板的事项的背景和影响面告诉我，看需不需要做" → produced a risk-tiered analysis with live probes.
- Claim auth decision (verbatim): "申领通过控制台页面登陆做认证然后批量签发" → console-login auth, then batch issuance; production line needs no internet.
- Question answered: how credentials reach hardware — "写在固件还是与分区固化的sn做绑定？" Current answer: baked into firmware, zero hardware binding.
- Goal `goal-0ab61074-4a41-440d-b168-efbb954324b0` (revision 1, phase=active, rounds 2/12, armed) covers the 6-step ordered remediation.
- Earlier goal `goal-b9ddb653-403d-43b2-823c-f4fd30a41956` is phase=complete / disarmed.

## Key Technical Concepts
- Repo topology (all pushed, ahead/behind 0/0): outer `E:\workspace\rainmaker-oneye` branch `esps3_korvo2_20260915` → `backend/` (eleball-oneye/oneye-iot, branch `esps3_korvo2_20260915`), `embedded/esp-adf/` (eleball-oneye/esp-adf, branch `oneye_s3_korvo`) → `components/oneye-dev-sdk` (main), `docs/oneye-iot-index` (ELEboysss/oneye-iot-index, main); separate `E:\workspace\oneye-wiki-content` (main).
- Trunk for PRs: outer `main`, backend `main` (108 ahead / 0 behind = clean FF; outer 36 ahead / 3 behind). esp-adf `master` is **Espressif upstream mirror** — do NOT PR into it.
- CVM `master2` = 175.178.190.187 (egress 193.112.111.91). Containers (never create/replace): `oneye-emqx` (1883/8883/8083/18083), `oneye-mtls` 18884, `oneye-crl` 18885 (`ENABLE_CRL_CHECK=true`), `oneye-platca` 18886, `oneye-storage` (PG 16.15). Restarting `oneye-crl`/`oneye-emqx` is sanctioned; recreating is not.
- Platform CA `CN=oneye-iot-device-ca` notAfter **2036-09-17T11:27:40Z** (irreplaceable). Dev CA `CN=oneye-dev-ca,O=oneye-dev`.
- signerd: `/tmp/signerd`, pid restarted via python `os.execve` with patched env; listens 127.0.0.1:8088; `CA_CRL_DP_URL=http://crl.oneye.me:8080/oneye-iot-device-ca.crl`; `/v1/info` shows `leaf_days=3650 batch=up to 500 crl_days=7`.
- EMQX acl.conf placeholder syntax = **`${username}`/`${clientid}`**, NOT `%u`/`%c` (that is SQL-authz-source syntax). `deny_action=ignore` → publish is silently dropped but still ACKed.
- Host→container via docker-proxy appears as source IP **172.18.0.1**, not 127.0.0.1.
- `certissuer` is shared with upstream AWS/Matter; Matter attestation profile (Req 8.5) **forbids EKU** ⇒ EKU must stay profile-driven with empty default.

## Files and Code
- `backend/src/utils/certissuer/certissuer.go`: `Profile` gained `ExtKeyUsage []x509.ExtKeyUsage` (empty default = no extension, preserving Matter/AWS shape); added `var DeviceExtKeyUsage = []x509.ExtKeyUsage{x509.ExtKeyUsageClientAuth}`; `Issue` template now `ExtKeyUsage: p.ExtKeyUsage`; renamed unexported `subjectKeyID` → exported **`SubjectKeyID`** (2 call sites updated); doc comments rewritten.
- `backend/src/rmneo/ca/ca.go`: `IssueDeviceCert` template now `KeyUsage: x509.KeyUsageDigitalSignature` (dropped `KeyEncipherment`), `ExtKeyUsage: certissuer.DeviceExtKeyUsage`, added `SubjectKeyId: skid` (computed via `certissuer.SubjectKeyID`); added import `"github.com/espressif/esp-rainmaker-neo/src/utils/certissuer"`.
- `backend/src/rmneo/ca/signer/server.go` (`/v1/sign-pub` profile) and `backend/src/claim/handlers/claim_handler/claim_handler_main.go:392` and `backend/src/tools/issuecert/main.go:189`: profiles now pass `ExtKeyUsage: certissuer.DeviceExtKeyUsage`.
- `backend/src/rmneo/ca/eku_parity_test.go` (NEW): `TestBothDevicePathsShareOneExtensionSet` (full extension OID/criticality/value parity, SKID excluded from diff but asserted present, plus per-cert business assertions) and `TestDefaultProfileStillOmitsEKU` (guards Matter/AWS default).
- `backend/deployment/dev-stack/emqx/acl.conf`: `%u` → `${username}` everywhere; host rule → `{allow, {ipaddrs, ["127.0.0.1", "172.18.0.1"]}, all, ["$SYS/#", "#"]}`; long pitfall comments added; ends `{deny, all}.`
- `backend/deployment/dev-stack/docker-compose.acl.yml`: added `EMQX_AUTHORIZATION__NO_MATCH: deny`.
- `backend/deployment/dev-stack/README.md`: documents that the default deployment was wide open (anonymous read of `rmng/dev/#`), the two pitfalls, and that **authentication is still missing**.
- `backend/contracts/api/mqtt/传输规范.md` §8: corrected the `%u` claim, records the syntax pitfall.
- `backend/docs/architecture/设备凭据吊销与签发台账设计.md`: §3.5.0 hard deadline block (M0 2033-09-17 / M1 2034-03-17 / M2 2034-09-17 / M3 2035-09-17); earlier edits for D6, 180-day threshold, v0.13.
- `backend/docs/ops/域名切换与CRL运维.md`: §9.4 switch evidence, §9.5 device-side reflash (done), §十 hard deadline.
- `backend/docs/architecture/系统架构设计.md`: version rows v0.82 (domain), v0.83 (180d), v0.84 (domain switch executed), **v0.85** (EMQX authz closure + ACL pitfall + 2036 deadline).
- `embedded/esp-adf/examples/oneye/korvo2_oneye/main/net_probe.c`: `probe_tcp` now falls back to `getaddrinfo` when `inet_pton` fails; new `rc=-3` = DNS failure; verdict string added; includes `stdlib.h`, `lwip/netdb.h`.
- `embedded/esp-adf/examples/oneye/korvo2_oneye/MTLS-接入与排障.md`: new §5 (domain cutover, two build/flash gotchas, evidence table, pitfall ④ = net_probe false failure).
- `embedded/esp-adf/examples/oneye/korvo2_oneye/sdkconfig` (**gitignored**): `CONFIG_ONEYE_FW_CLOUD_HOST="mqtt.oneye.me"`, PORT=18885, TRANSPORT_TLS=y, TLS_INSECURE unset. Backup at `output/.build/sdkconfig.bak-before-domain`.
- `embedded/esp-adf/examples/oneye/korvo2_oneye/main/certs/`: `client.crt` serial `a0ec864f2fbc33b7af879ccfa22a2e35`, DP = domain; backups `*.bak-ipdp`.
- `.memory/v0.0.1/20260921/revocation-control-plane-and-board-mtls/digest.md` committed in esp-adf (`session/` intentionally gitignored per `.memory/.gitignore`).

## Errors and Fixes
- `ca.go` used `certissuer.DeviceExtKeyUsage` without importing it → added import (Go imports are per-file).
- `certissuer.ParseCertificatePEM` takes `string`, not `[]byte` → 3 call sites fixed.
- Parity test caught a **third real divergence**: path A wrote no SKID (Go only auto-adds for CA certs) → fixed by explicit `SubjectKeyId`.
- EMQX ACL first deployed with `%u` → equipment denied its own tree; log: `username: korvo2-0001, topic: rmng/dev/korvo2-0001/#, authorization_permission_denied`; fixed to `${username}`.
- First ACL attempt also missed `172.18.0.1` → host runbook commands would be denied.
- `PUBACK RC:0` misread as "accepted"; log shows `cannot_publish_to_topic_due_to_not_authorized` under `deny_action=ignore`.
- Earlier mis-claims corrected: "board cert has no DP" (bad grep; it had IP DP); "Dashboard password is `oneye_dev`" (env var present but login returns 401 ⇒ real password unknown).
- `esptool` failed with `Could not open COM12, the port is busy` → must `serial_disconnect` first.
- `go build ./src/...` hit `fatal error: runtime: cannot allocate memory` → build/test narrower package sets with `-p 2`.
- PowerShell: shell vars inside double-quoted strings get eaten (`\$C` → `\`) → use single-quoted here-strings + base64-encoded remote scripts.
- Full-scan probe misuse: server-side `curl` to own public IP times out (no hairpin) — always verify public reachability from outside or from a container.

## Pending Jobs
- **④ EKU unification**: code+tests done and green; still to do — verify live on the CVM (issue one cert via `/v1/sign` and one via `/v1/sign-pub`, diff extensions), update `contracts/domain/设备凭据与CA.md` (still says `KeyUsage = DigitalSignature | KeyEncipherment`), update spec/0087 §5 #12 PEND item, add a version row, commit+push, outer pointer bump. Existing certs with the old shape (board's `a0ec864f…` = DS only, no KE) are unaffected; no reissue required unless shape parity is demanded for already-issued certs.
- **⑤ shadowd CRL check** (Go `x509.Verify` does not check CRLs) — tie to shadowd deployment.
- **⑥ Claim entry console-login auth + batch issuance** (+ device-side credential partition design draft before coding).
- PRs still unopened: outer `https://github.com/eleball-oneye/rainmaker-oneye/compare/main...esps3_korvo2_20260915?expand=1`, backend `https://github.com/eleball-oneye/oneye-iot/compare/main...esps3_korvo2_20260915?expand=1`; ready-made titles drafted; no `gh`, no token.
- User-side: Tencent Cloud SG must close inbound **5432 / 18083** (recommended also 1883 / 8883).

## Current Work
- Goal round 2/12, step ④ in progress. Just ran `go test -count=1 -p 2 ./src/utils/certissuer/... ./src/rmneo/ca/...` → all ok, and `go test -count=1 -p 2 ./src/claim/...` → all 6 packages ok (claim, ca_bootstrap, claimbuild, claim_admin, claim_handler, issuerbuild), EXIT=0.
- ② (EMQX authorization closure) and ③ (2036 hard deadline) are complete, committed and pushed: backend `1f9f094` (EMQX fix) → `7a0f00a` (deadline docs + v0.85); outer `314623c`.
- ② evidence (external): anonymous `rmng/dev/#` denied; device own down face allowed; other node's down face denied; device own up face denied (per-face least privilege); anonymous publish logged `not_authorized`; end-to-end delivery device→`oneye-shadowd` received `{"online":true,"probe":"acl-e2e"}`; retained probe cleared. Live broker runs the repo's acl.conf + `no_match=deny`.
- 1883 now has zero clients; board still connected on 18885.

## Next Step
- Finish ④: verify the two live signing paths on the CVM produce identical extension sets, then update `contracts/domain/设备凭据与CA.md` (KeyUsage row), spec/0087 §5 #12 (PEND → resolved), and `系统架构设计.md` with a v0.86 row; run `md_link_check`/`contract_check`; commit+push backend and bump the outer pointer.

## Critical Context
- Exposure findings (all probed from outside): before the fix `oneye-emqx` had `authentication = []`, `no_match = allow`, stock acl.conf ending `{allow, all}.` ⇒ anonymous read of real device message `rmng/dev/korvo2-0001/status/up {"online":false,"reason":"lwt","ts":3967}`; logs show CENSYS/scanner/nmap probes. PostgreSQL 5432 open with documented creds verified working (`LOGIN_OK oneye oneye`). 18083 Dashboard publicly reachable (`HTTP 200`, `<title>EMQX Dashboard</title>`); real password unknown. 8883 serves the EMQX stock demo cert (`C=CN,ST=hangzhou,O=EMQ,CN=Server`, issuer `RootCA`). 9093 (chatd) reachable, HTTP 404.
- **Authentication on 1883/8883 is still absent** — anyone knowing a node_id can set `username` and gain its permissions; fixing needs mTLS (container recreation) or password auth (firmware change).
- Credential provisioning today: certs+key embedded at build time via objcopy (`ONEYE_FW_EMBED_CERTS`, `-DONEYE_FW_CERT_DIR=`), `device_id = username = client_id = CONFIG_ONEYE_FW_DEVICE_ID` (compile-time); **no eFuse use anywhere in SDK/example**; MAC printed but unused; no credential partition; no device-side claim/CSR client (README TODO only). Server-side claim design already keys on `(claimant_id, mac_addr)` and accepts a device-generated CSR (`/v1/claim/verify {mac_addr, csr}`). Recommended: keep `ca.crt` in firmware; put `node_id`+`client.crt`+`client.key` in a dedicated partition written at the factory; optional eFuse (SN or pubkey fingerprint) for anti-cloning — decide before production tooling is frozen.
- Board `korvo2-0001` on COM12: firmware binary contains `mqtt.oneye.me` once and `175.178.190.187` zero times; serial shows `link up: mqtt.oneye.me:18885 transport=mqtt-tls node=korvo2-0001`, self-test 21/0, net-probe `rc=0 … 42ms OK（TCP 已建立）`; broker fetched CRL over the domain (`fetching_crl → fetched_crl → new_crl_url_inserted`); bed server certs (18884/18885/18886) SAN now `IP:175.178.190.187, IP:127.0.0.1, DNS:localhost, DNS:mqtt.oneye.me`; `broker-crl-ab.sh --node acct-gate` = PASS 17 · FAIL 0; external `mqtt.oneye.me:18885` gave `Verify return code: 0 (ok)` + real MQTT `CONNACK (0)`.
- Gate results: `contract_check.py` 97 routes; `md_link_check.py` 150 md files; `gates-mutation-check.sh` 6/6.
- Build facts: WSL native IDF `/home/yiwei/esp/esp-idf-5.5.5`, `ADF_PATH=/mnt/e/workspace/rainmaker-oneye/embedded/esp-adf`, build dir `output/.build/korvo2_oneye-xtensa-esp32s3-elf-gcc-14.2.0`, flash via `output/.build/flash-korvo2.ps1 -Port COM12`; `examples/**/sdkconfig` is gitignored; **never** use `build-all.sh --firmware` to change the endpoint (regenerates sdkconfig from defaults); delete `*.S` before rebuilding after cert swaps.
- Serial session is currently connected to COM12 in this session (disconnect before any further flashing).
- Process lessons: a diagnostic that lies is worse than none; verify "will it bite" by mutation; don't trust client-side ACK for MQTT authorization; check logs for authoritative verdicts.
