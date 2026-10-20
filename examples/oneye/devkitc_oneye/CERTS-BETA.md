# `certs-beta/` —— 乙型（BETA）**本机型专用**内嵌证书目录（2026-10-20 建档）

> 本文件**入库**；`certs-beta/` 目录**不入库**（含设备私钥，见 `.gitignore`）。
> 目的：结束「乙档借 `korvo2_oneye/main/certs`」的历史（README §7 ⑦ 那条待裁定项），
> 把"这台机型嵌的是谁的证书"变成**一处可读、可复算**的事实。

## 1. 目录内容（三件，构建入参 `-DONEYE_FW_CERT_DIR=<本目录>`）

| 文件 | 是什么 | 用途 | 是否入库 |
| --- | --- | --- | --- |
| `ca.crt` | **设备侧信任锚** = 两张 CA 的 bundle（`platca` + `realca`，1190 B / 2 PEM） | 设备验**服务端证书**（mbedTLS `tls_ca_pem`） | ⛔ |
| `client.crt` | 设备**客户端证书**（品类级，见 §2） | mTLS 客户端身份（入网准入前提） | ⛔ |
| `client.key` | `client.crt` 的匹配私钥（EC P-256） | 同上 | ⛔ **永不回显/永不打印** |

## 2. 证书事实（2026-10-20 用 `openssl` 实读，⛔ 非转述）

```
subject   = CN=OYSPK02-0001              ← 品类级（productKey `OY-SPK-02`），⛔ 不是一机一密
issuer    = CN=oneye-iot-device-ca        ← 注意：bundle 里两张 CA **subject 完全相同**
serial    = 35AD9051518F44C94908E63B390C62B0
notBefore = Oct 13 04:51:18 2026 GMT
notAfter  = Oct  4 12:57:38 2036 GMT
AKI       = 73:90:F0:50:…:93:35:6D        ← = **realca** 的 SKI（不是 platca 的 75:7B:92:8F:…:8F:E6）
EKU       = TLS Web Client Authentication；CA:FALSE
```

⇒ 判"谁签的"**只能看 AKI/SKI**，看 subject 会得出错误结论（两张 CA 同名，这是历史事实，⛔ 不重新命名）。

## 3. 三处对齐证据（可复跑；命令只读）

| 断言 | 命令（要点） | 2026-10-20 实测 |
| --- | --- | --- |
| 设备信任锚能验**云服务端**证书 | `openssl verify -CAfile ca.crt <cloud>/certs/server.crt` | **OK**（server.crt `CN=oneye-emqx-platca`，AKI = platca 的 SKI） |
| 本机客户端证书**在锚内** | `openssl verify -CAfile ca.crt client.crt` | **OK**（对 `platca` 单张 = error 20 ⇒ 证明它确是 realca 签的） |
| 设备信任锚 == **broker 现用锚** | `sha256sum <cloud>/certs/ca.crt` | `e8ea3a7e4f5a0d118cb71250231727215125ed89dec4002ae1c9d0f923ff3fb3`（1190 B / 2 PEM）与本目录 `ca.crt` **逐字节相同** |
| 私钥与证书配对 | `openssl x509 -pubkey` vs `openssl pkey -pubout` | **相同** |

云侧只读读数（`docker inspect oneye-emqx`，2026-10-20）：
`VERIFY=verify_peer`、`FAIL_IF_NO_PEER_CERT=true`、`ENABLE_CRL_CHECK=false`、
`CACERTFILE=/certs/ca.crt`、`ENABLE_AUTHN=false`、全局 `PEER_CERT_AS_USERNAME=cn`
⇒ TLS 层对**本证书**放行；但 **username 会被覆盖成 `CN=OYSPK02-0001`**（见 §5）。

## 4. 为什么**不**用 dev catca 签客户端证书（⛔ 别试）

本机 dev 栈那份 bundle（`cloud/deploy/local/init/emqx/certs/deviceface-ca-bundle.crt`，1935 B / 3 PEM）
里第 3 张是 **dev-only 品类 CA** `CN=oneye-dev-catca`；
**生产/云上的锚恰好只有 2 张**（platca + realca，sha256 `e8ea3a7e…`）⇒ 用 catca 签的叶子会被 broker
以 `unknown_ca / bad certificate` 拒。故本目录的叶子必须由 **platca 或 realca** 签 —— 现值 = realca。

## 5. 已知平台侧不一致（**本批只出方案，⛔ 未改云**）

| 面 | 值 |
| --- | --- |
| 证书 CN（EMQX 会把它当 username） | `OYSPK02-0001`（品类级，**所有乙型同一个值**） |
| 固件 `device_id` / MQTT `client_id` 缺省 | `devkitc-0001` |
| 云库 node / ACL 段 `rmng/dev/{node}/#` | `DEVKITC0001` |

⇒ 修好栈溢出后**大概率**撞的下一道门：TLS 过、CONNECT 过（`ENABLE_AUTHN=false`），
但 **ACL 按 `OYSPK02-0001` 判、topic 按 `DEVKITC0001` 发** ⇒ 首条 publish 被 deny ⇒ `disconnect` 循环。
云侧建议（待用户裁）：乙型 deviceface 监听器**不设** `peer_cert_as_username`（或 `transport` 按
MQTT username/clientId 认 node），甲型保留 `CN=node`。详见批次报告
`E:\workspace\_oneye-logs\DEVKITC-STACK-CERT-FIX-20261020.md`。

## 6. 复现 / 轮换

- 重新生成构建入参：把现役签发 CA（platca 或 realca）签出的**品类级**叶子三件放进本目录即可
  （需要 CA 私钥；本机与云 CVM `/opt/oneye` 下**没有** CA 私钥 ⇒ 换签属签发权动作，得走平台）。
- `ca.crt` 的**唯一**正确造法：把 broker 现用锚原样拷来（`platca` + `realca`，顺序不变），
  ⛔ 不要只放一张 —— 只放 `realca` 会导致设备**验不过云服务端证书**（server 是 platca 签的）。
