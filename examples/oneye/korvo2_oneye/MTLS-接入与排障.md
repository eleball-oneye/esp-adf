# 一机一密 mTLS 接入与排障（2026-09-21 真机取证）

> 这份是 `README.md` §4「上云」的补充：把"设备带**平台 CA 签发**的证书接入开着 CRL 检查的
> 门卫"这件事的**做法、必需配置、以及三个踩过的坑**写下来。README 讲的是固件怎么用，
> 这里讲的是**证书从哪来、门卫凭什么收、出错了先看什么**。

## 1. 证书从哪来

证书由**单实例签名服务**（`signerd`，见 `backend/deployment/signer/README.md`）签发，
设备侧拿到的是一套 PEM（服务生成一机一密密钥）：

```bash
SIGNER_TOKEN=<令牌> ./issuecert -signer http://127.0.0.1:8088 \
    -node korvo2-0001 -out /tmp/board-certs -validity-days 3650
```

放进本工程 `main/certs/`（**已被 `.gitignore` 忽略，私钥绝不入库**）：

| 文件 | 内容 | 说明 |
| --- | --- | --- |
| `client.crt` | 设备证书 | CN = node_id；带 CRL 分发点 + `clientAuth` EKU |
| `client.key` | 设备私钥 | 0600；服务侧**不留存**它，丢了只能重签 |
| `ca.crt` | 信任锚 | **床上是两张的捆绑**：dev CA（门卫的**服务器**证书是它签的，设备要验它）+ 平台 CA（生产口径下只会有一张） |

`main/CMakeLists.txt` 见到三件齐备就以 `ONEYE_FW_EMBED_CERTS` 编进固件（`-DONEYE_FW_CERT_DIR=<目录>`
可换目录），main.c 据此填 `credential` / `credential_key` / `tls_ca_pem`。

## 2. 必需的构建配置（**不配就连不上，而日志会说是 TLS 失败**）

```ini
CONFIG_ONEYE_FW_TRANSPORT_TLS=y
CONFIG_ONEYE_FW_CLOUD_PORT=18885        # 床上开着 CRL 检查的那张门卫
CONFIG_ONEYE_FW_TLS_INSECURE=n          # 校验证书；dev 才允许关

# ← 这两条是 2026-09-21 实测加上的，**缺了就是 mbedtls_ssl_setup 分配失败**
CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y     # mbedtls 缓冲走 PSRAM（板上 8 MB，几乎白给）
CONFIG_MBEDTLS_DYNAMIC_BUFFER=y         # in/out 缓冲只在握手/收发的当口要
```

**为什么必须有后两条**：`mbedtls_ssl_setup` 要一次拿到 SSL 上下文（`IN_CONTENT_LEN=16384`
+ `OUT_CONTENT_LEN=4096`）。这块内存默认从**内部 RAM** 出，而本板上 AFE/摄像头/Wi-Fi 已经把它
吃得差不多 ⇒ 拿不出来。表现是**证书完全没问题**、却在握手前就失败。

## 3. 三个坑（都真实发生过，别重踩）

### 坑 ①：`rc=-6` 不等于"证书有问题"

设备侧日志只有：

```
[oneye][base][W] connect 175.178.190.187:18885 (mqtt-tls) failed: rc=-6 (0xfffffffa)
```

`-6` 是 **OSAL 的 `ONEYE_OSAL_ERR_TLS`**（抽象码），**不是** mbedtls 的错误码。
"CA 链没验过 / 私钥解析不了 / 主机名不匹配 / 分配失败"在它上面**完全同形**。

SDK 现已补上逐点诊断（`oneye-dev-sdk` `90b5a50`），同一次失败会多出两行：

```
W (4916) oneye_tls: TLS failed at mbedtls_ssl_setup: ret=-32512 (0x7f00)
W (…)    oneye_tls: TLS handshake failed: ret=-0x2700 verify_result=0x00000008
```

- `ret` = **在哪一步**、mbedtls 的原始码（`-0x7f00` = `ALLOC_FAILED`、`-0x2700` = 证书校验失败…）；
- `verify_result` 的位标志才回答**证书为什么没验过**（`0x08` = CN/主机名不匹配、`0x100` = 不受信任…）。

**先看 `ret`，再看 `verify_result`**：前者决定"是不是证书问题"。

### 坑 ②：改了 SDK 源码，`idf.py build` 不会带上它

固件链的是**预编译归档** `output/<toolchain-id>/lib/liboneye_dev_*.a`，不是现场编 SDK 源码。
所以只改 `components/oneye-dev-sdk/src/...` 再 `idf.py build`，**产物 mtime 都不动**
（本轮真的这样白跑了一轮）。正确顺序：

```bash
./build-all.sh --toolchains esp32s3@5.5.5      # 重建归档（有 toolchain.json 版本哨兵）
cd examples/oneye/korvo2_oneye && idf.py -B <build> build
```

### 坑 ③：工程里的 `sdkconfig` 可能是**上一份固件**的配置

本轮遇到磁盘上写着 `TRANSPORT_TCP`/1883，而板子当时正以 mqtt-tls 连 18885 —— 说明那份
`sdkconfig` **不是**生成当前固件的那一份。拿它当基线去比较"内存配置有没有变"会得出错误结论。
要复现固件行为，基线应当是**生成该固件的那份配置**；需要重建时就按 README §2 删掉 `sdkconfig`
让 `sdkconfig.defaults*` 说了算。

## 4. 闭环验收：A / B / A′（真门卫 + 真开发板）

| 步骤 | 操作 | 观察到的事实 |
| --- | --- | --- |
| **A** | 烧入平台 CA 证书的固件，连 18885 | 串口 `link up: 175.178.190.187:18885 transport=mqtt-tls node=korvo2-0001`；门卫日志 `clientid: korvo2-0001` 上线、订阅 `shadow/down` 等 |
| **B** | 经签名服务吊销该证书序号 → 用它导出的名单覆盖发布 → 让门卫重取名单 | 门卫**逐次** `SERVER ALERT: Fatal - Certificate Revoked`（Erlang `{tls_alert,{certificate_revoked,…}}`）；板子每次重连都被拒 |
| **A′** | 为同一台设备**重签**一张（新序号）+ 重发名单 + 重烧 | 串口重新 `link up`；台账里旧序号 `revoked=t`、新序号 `revoked=f`；名单含旧序号、不含新序号 |

**A′ 才是"吊销之后怎么办"**：本设计**没有取消吊销**，设备要恢复使用就重签一张。
（dev CA / openssl 那套库上的 `restore` 是另一条路，两者语义不同，别混用。）

运维侧的完整 runbook 在 `backend/docs/ops/域名切换与CRL运维.md`。
