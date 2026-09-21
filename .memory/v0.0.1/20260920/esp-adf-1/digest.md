# Memory digest

> generated on compaction 2026-09-21T02:01:03.649Z

This is an automatically generated checkpoint condensing an earlier span of the conversation to free up context. Treat the captured context as established background and build on it without restating it. Continue the task directly from the messages that follow, without acknowledging this checkpoint.

<compacted-summary>
## Primary Request and Intent
- Original (turn 1): add git submodule `git@github.com:ELEboysss/oneye-iot-index.git` under `embedded\esp-adf\docs`, copy master `index.html` into it; then "梳理oneye-dev-sdk的物联协议实现以及嵌入式设备能力，将设备面契约更新到…oneye-iot-index的合适部分，并在index.html体现，包括mqtt topic、ws/wss、配网、一机一密、平台鉴权等，确保架构地图和实现保持一致"; also "分析一下架构设计和实际实现哪些地方是需要调优的"; "缺陷进行落地修复并同步设计文档和架构index"; mirror `oneye-iot-index` into `E:\workspace\oneye-wiki-content\resource\物联云平台\API契约` with an md index page; "更新后端和嵌入式的docs，务必和实际实现对齐"; "将改动都推送".
- D3: "直接做 CRL DP，现在就重签全部设备证书". Batch A: "先做批次 A（门禁+契约卫生+到期预警+文档同步）". Flow: "像这次一样直接提交并推送".
- D5: "设备在 HTTP 面复用同一张 mTLS 证书".
- Device testing: "没有 DP 时放行，以后可能会补上域名。可用CVM和已连接串口的korvo2-2开发板实测"; board scope "先只做非侵入式取证（推荐）" (later relaxed); "P-a可以做。重烧完整功能面固件验证".
- Goal 2 checklist (verbatim): "①重建 C3/IDF6.0.3 归档 2. 可做 3. 做 4. 做 5.全跑 6.用SNTP 7.选b自研自托管 8. 先实现，后续重新签 9.目前域名和证书未定，以后会部署反代和生产环境 10.国内没有AWS环境，真的需要的话只考虑自研" — i.e. ①rebuild C3/IDF6.0.3 archives ②pgstore real-DB concurrency/volume ③fix hello_oneye on IDF 5.5.5 ④real YAML parse gate ⑤all device-face downlink faces (command/shadow/log ack/ota notify) ⑥SNTP + strict TLS server verification ⑦self-hosted CA (incl. CRL DP extension in issuance + real issuance→ledger→device online with platform-CA cert) ⑧CRL DP end-to-end ⑨production EMQX mTLS/domain/reverse-proxy deferred ⑩no AWS. "所有取证回写设计文档/契约/index 后推送".
- Latest user turn: "等待缺陷修复和开发任务子代理反馈，然后继续按优先级处理后续".

## Key Technical Concepts
- Repo topology (all remote refs verified in-sync at checkpoint): outer `E:\workspace\rainmaker-oneye` branch `esps3_korvo2_20260915` → `backend/` (remote `eleball-oneye/oneye-iot`) + `embedded/esp-adf/` branch `oneye_s3_korvo` (remote `eleball-oneye/esp-adf`) → `components/oneye-dev-sdk` branch `main` (remote `eleball-oneye/oneye-dev-sdk`), `docs/oneye-iot-index` branch `main` (remote `ELEboysss/oneye-iot-index`). Separate: `E:\workspace\oneye-wiki-content` branch `main`.
- Tips: outer `08a10b41`, backend `dff45722`, esp-adf `75785966`, SDK `05775b47`, index `751555bf`, wiki `faae1cfa`.
- Three dev beds on CVM `master2`/175.178.190.187: `oneye-mtls` 18884 (dev CA, no CRL check), `oneye-crl` 18885 (dev CA + `ENABLE_CRL_CHECK=true`, CRL served at `http://175.178.190.187:8080/oneye-ca.crl`), `oneye-platca` 18886 (platform CA `CN=oneye-iot-device-ca`, `enable_crl_check=false`), plus `oneye-emqx` (1883/8883/8083/18083) and `oneye-storage` (5432).
- Bed env (authoritative): `ENABLE_CRL_CHECK=true`, `VERIFY=verify_peer`, `FAIL_IF_NO_PEER_CERT=true`, `CACERTFILE=/certs/ca.crt`, `CERTFILE=/certs/server.crt` (`CN=oneye-emqx-mtls`, SAN `175.178.190.187/127.0.0.1/localhost`), `EMQX_MQTT__PEER_CERT_AS_USERNAME=cn`.
- **Converged CRL model (one rule)**: broker must hold a CRL relevant to the peer cert's issuer; if it cannot obtain one, the connection is refused. A DP lets it fetch; without a DP only what is already cached can be used. **No fail-open.**
  - DP + fetched + listed ⇒ reject `certificate_revoked` (client `sslv3 alert certificate revoked`, alert 44)
  - DP + fetched + not listed ⇒ accept; DP + fetch fails (404, cache cold) ⇒ reject `{bad_crls,no_relevant_crls}` (`fetched_crl`=0)
  - no DP + cached CRL lists it ⇒ **reject `certificate_revoked`** (falsifies "no-DP escapes revocation")
  - no DP + cached CRL does not list it ⇒ accept
  - no DP + genuinely cold cache ⇒ **reject** `{bad_crls,no_relevant_crls}` (`fetched_crl`=0) — verified with board held in reset 300 s (> EMQX ~100 s boot)
- Real risk = **fail-closed in two forms**: ① enabling the switch with legacy no-DP certs ⇒ those devices are refused (failure mode = cannot connect, not silent skip); ② DP-bearing certs are also refused when the CRL endpoint is unreachable ⇒ CRL endpoint availability is on the connection critical path.
- **`docker restart oneye-crl` does not reliably produce a cold cache**: cold at 12:42, but not at 12:55/13:01 (no-DP accepted, `fetched_crl`=0); cause unknown (hypothesis: persisted `data` volume/`mnesia`). ⇒ "rejected on a cold cache ⇒ it has no DP" is unsound here; judge DP presence by "can it trigger a CRL fetch" or by decoding the cert/firmware.
- `http://` CRL cached until `nextUpdate`=2036 ⇒ republishing a CRL has no effect without a broker restart (152 rejections over 11 min, zero re-fetches).
- Alert codes: no client cert ⇒ TLS1.2 `alert 40 sslv3 alert handshake failure` (in-band via openssl; TLS1.3 = `certificate_required`); untrusted CA ⇒ `tlsv1 alert unknown ca` (48) **only on 18885** (dev CA is its only trust anchor); platform CA works on 18886.
- Device `rc` codes: `-6` = `ERR_TLS`, `-3` = `ERR_IO` (`rc=-3` with no ClientHello = local I/O failure).
- IDF 6.0.3 ships **Mbed TLS 4.1.1** (removed `mbedtls/gcm.h`, `mbedtls/md.h` from public API); SDK partitions by `MBEDTLS_VERSION_NUMBER >= 0x04000000` (PSA), HMAC-SHA256 hand-built on `oneye_sha256`.
- Board: ESP32-S3-Korvo-2 on **COM12**; opening COM12 pulses EN (reboots the board); with `clean_start=true` + `session_expiry_interval=0` an unsubscribed client drops pre-subscribe messages ⇒ start capture first, confirm subscription, check broker fan-out (`delivered count`) before trusting silence.
- Hidden trap: `./build-all.sh … 2>&1 | tee log` then reading `$?` yields **tee's 0**, not the script's exit code.

## Files and Code
- `backend/contracts/api/mqtt/传输规范.md` §8.1 — the main record: CRL cache boundary + retraction; four-round device-face/downlink status; ⑦⑧ closure; unified CRL rule + fail-closed risk; two-batch `no_relevant_crls` attribution; cert-size口径 (PEM 1269/1188/1196 B vs DER 154 B, "尺寸不可作判据"); final "docker restart 不总能产生冷缓存" limit. Changed in `c1af9ce`, `83a21cb`, `8ff6333`, `4822047`, `6445b86`, `f3fab98`→`a3fab98`, `f27dd0f`, `25f89d9`, `dff4572`.
- `backend/docs/architecture/设备凭据吊销与签发台账设计.md` (§3.2.3 V0/V1/V2 + legend, §5 #8/#11, §3.5.1, 已闭环 list) — changed in `66c93ce`, `aa24468`, `e55c878`, `135a9f2`, `84f3929`, `e652b87`.
- `backend/docs/practice/{本地开发与门禁.md,测试与自动化.md}` — gate口径 update (HTTP 97 routes, YAML真解析, H group) in `66c93ce`.
- `backend/src/rmneo/devicecert/pgstore/pgstore.go` — `ensureSchema` with `pg_advisory_xact_lock(0x6F6E6579654443)` inside a transaction; fix commit `008af5f`.
- `backend/src/rmneo/devicecert/pgstore/pgstore_openrace_test.go` — opt-in reproducer (`PGSTORE_OPEN_RACE`); header table before/after: PG15.19 16/40→0/40, PG16.15 14/40→0/40, PG17.11 7/40→0/40.
- `embedded/esp-adf/build-all.sh` — failure propagation fix `138efaee` (+276/-35): `FAILED_TC`/`SKIPPED_TC`/`BUILT_TC` ledger, `tc_begin`/`tc_commit`/`tc_rollback`, `STALE.md`, judgement before report, `publish_to_repo_lib` no longer swallows `cp` failures, `usage()` auto-slices header.
- `embedded/esp-adf/components/oneye-dev-sdk/docs/INTEGRATION.md` — hello_oneye IDF 5.5.5 troubleshooting rows + the `tee` exit-code trap (`05775b4`).
- `embedded/esp-adf/components/oneye-dev-sdk/src/oneye_dev_log.c:856-863` (`set_level`/`set_uplink`/`dump`/`ack`, `lg_apply_ack` is `void`), `oneye_dev_event.c:684-686` (`ack`/`confirm`/`capture`/`mute`).
- `embedded/esp-adf/output/.build/hwverify/` — hardware evidence logs: `task7-serial-18886.log`, `broker-task7.log`, `clean-stageA|B|C-*`, `rule4-experiment.log`, `rule5-nodp.log`, `isolated-3step.log`, `embedded-cert-proof.log`, `cert-reevidence.log`, `downlink-serial3-utf8.log`, `repair-hairpin.log`, plus `serial-crlneg2.log`, `serial-downlink2.log`, `serial-sntp.log`.
- CVM bed scripts: `/home/ubuntu/crl-dp/{revoke-and-publish.sh,restore-and-publish.sh,warm-crl-cache.sh,fix-hairpin.sh,tls-probe.sh,openssl.cnf,index.txt,www/oneye-ca.crl}` — both publish scripts **hardcode serial 1000** (`CERT=$DP_DIR/korvo2-0001-dp.crt`).

## Errors and Fixes
- `build-all.sh` silent success: failure still printed "完成。产物树：" and wrote a report with zero failure marks, listing stale artifacts as current results; publish path had `cp … 2>/dev/null || true`. Fixed in `138efaee`. **The "compile failure still exits 0" premise was not reproducible under direct invocation** (pre-fix returned 1); the reproducible exit-0 paths were `2>&1 | tee` and a wrong toolchain spec — recorded with the downgrade.
- `pgstore.Open` concurrent DDL race ⇒ `duplicate key value violates unique constraint "pg_type_typname_nsp_index"` (SQLSTATE 23505); pre-fix 37/120 failures → post-fix 0/120; mutation-verified (replacing the lock with `SELECT 1` turns the regression test red).
- iptables cleanup ran `iptables -t nat -D PREROUTING 1` three times and ate pre-existing rules including the 8080 hairpin DNAT ⇒ every CRL fetch failed ⇒ DP certs rejected `{bad_crls,no_relevant_crls}`. Repaired; rule: `PREROUTING` showing only `-P PREROUTING ACCEPT` is "wiped", not "clean"; always re-run `fix-hairpin.sh` and verify container→public CRL URL = HTTP 200.
- `restore-and-publish.sh` timed out at 120 s through ssh but had **already flipped index.txt to `V`**; completed manually (flip `V` → `openssl ca -gencrl` → publish → `docker restart oneye-crl`).
- Self-corrections recorded (all kept in docs as "原说法保留 + 标明被推翻 + 说明为什么错"): mtime-based "board runs no-DP cert" (wrong for current firmware; right for the 19:31 build); "no-DP escapes revocation" (stale-cache artifact); "compile failure still exit 0" (tee); "220 `no_relevant_crls` unattributed" (two batches); "risk = fail-open window" (falsified by isolated 300 s-reset experiment); plus subagent's "iptables verified clean" claim.

## Pending Jobs
- ⑤'s results are written into `backend/contracts/api/mqtt/传输规范.md` but **not yet mirrored into index/wiki**.
- ③ explicit unverified sub-items of ⑤: `log/down` `ack` **effect** (only branch entry proven; `lg_apply_ack` is `void`), `dump` actually re-collecting frames, `set_uplink` never sent.
- Open user decisions: whether to gitignore `.memory/` (esp-adf, currently `?? .memory/`, makes outer show ` M embedded/esp-adf`) and `examples/esp-idf/hello_oneye/managed_components/`; ⑨ production EMQX mTLS + domain/reverse proxy; batch re-signing window for existing certs; `searchThings` model/version filtering still unwired (spec/0075).
- "DP present but CRL fetched-and-irrelevant ⇒ reject" untested; `mosquitto_pub` TLS1.3 alert mislabel not reproduced by others.

## Current Work
- All checklist items ①–⑧ have hardware or counter-example evidence and are written back to design doc + contract + index + wiki (⑤ only to contract). ⑨ deferred per user, ⑩ honored.
- Last actions: added the final §8.1 limit about unreliable cache coldness, committed backend `dff4572`, bumped outer to `08a10b41`, verified all six repos in-sync; backend worktree clean, esp-adf has only `?? .memory/`.
- ⑤ hardware result (with controls): `log/down` `set_level` ⇒ `LOG_LEVEL_CHANGED`; `dump` ⇒ `OTHER`(=DUMP_REQUESTED); `ack` ⇒ no ignore line; `event/down` `ack` ⇒ `EVENT_ACK` + `[cloud-ack] 收到 ack 但无待确认本地事件` + `OTHER`(=DELIVERY_DONE); `confirm`/`capture`/`mute` ⇒ `EVENT_DIRECTIVE`; wrong-type controls reproduce `未知 type=…（忽略）`. Note `[sdk-event] MESSAGE` appears for every envelope (including ignored controls) and is not evidence of handling.
- ⑧-B on board: revoke CRL `0x1016` ⇒ 11× `connect … failed: rc=-6`, broker `certificate_revoked: 34`, `no_relevant_crls: 0`, `No clients.`; restore CRL `0x1017` ⇒ board reconnected in ~2 s. ⑦ board: `client 709 B / key 227 B / CA 595 B` + `link up …:18886`, broker `Client(korvo2-0001, username=korvo2-0001, connected=true)`.
- Bed currently armed: `index.txt` `V 1000 / V 1001`, CRL `0x1017` (0 revocations), HTTP 200, device connected on 18885.

## Next Step
- Mirror ⑤'s verified downlink results (and its three explicit unverified sub-items) into `docs/oneye-iot-index/index.html` + wiki mirror, then bump esp-adf/outer pointers and push.

## Critical Context
- Goal 2 remains **active, not complete**: the only gap is ⑤'s "log ack" (branch entry only, effect unproven); goal round limit 16/16 is exhausted, so continuation requires the user to say "继续".
- User preferences: push everything directly (commit+push, no staging questions); values honest self-correction of earlier wrong claims; wants item-by-item prerequisites/核对 before proceeding; refuses fake positives ("宁少勿假").
- All subagents finished except `5865acec` (index/wiki write-back) which is `running` on the terminology-disambiguation round; the disambiguation note is already committed as index `751555b`/wiki `faae1cf` and pointer-bumped, so its next output would need one more bump.
- Cautions: two agents may touch the same CVM/repos concurrently (observed churn); never create/replace `oneye-emqx`/`oneye-storage`/`oneye-mtls`/`oneye-crl`/`oneye-platca`; CVM cannot hairpin to its own public IP (dial from Windows/WSL instead); bed-side logs live outside the repos at `embedded/esp-adf/output/.build/hwverify/`.
- Evidence highlights: `abi-check.sh` exit 0 on both IDF 6.0.3 toolchains (6 libs, zero `oev_*` leakage, public API 34/20); `liboneye_dev_base.a` contains `0.3.1` ×5 and `T oneye_dev_version`, 0 residue of `0.2.0`; hello_oneye clean rebuild `BUILD_EXIT=0`, `hello_oneye.bin binary size 0x93380 bytes … (42%) free`; backend gate set all exit 0 (build/vet/test/gofmt CLEAN/contract 97 routes/md_link 149 files/rmng-lint), with real-DB cases SKIPped in the offline run; 10 万行装载 1.443 s (69,319 rows/s), `Index Scan using device_certs_pkey` (exec 0.071 ms), expiry plan `bitmap … Sort` unchanged by ANALYZE; 32 concurrent same-serial ⇒ 1 row, losers report 0 rows and no error; 32/64 distinct-serial ⇒ zero lost rows.
</compacted-summary>
