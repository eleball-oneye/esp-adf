# devkitc_oneye —— ESP32-S3-DevKitC-1 上的**零音频依赖** oneye 配网固件

> **定位**：把「**配网 + 绑定**」这条链在本仓**最普通的开发板**（ESP32-S3-DevKitC-1：无 codec、
> 无 LCD、无摄像头、无 SD 卡）上做成**可构建、可自证**的产物，用于验证
> **乙类（BETA）** 与 **丙类（GAMMA）** 的配网与绑定闭环。
>
> **事实源（三条，缺一不可）**
> 1. 本地面协议：[`contracts/local/ble-gatt.md`](../../../../../contracts/local/ble-gatt.md)
>    **§4.4.2**（三型矩阵）/ **§4.4.8**（三型 × 四时点身份矩阵）/ **§4.4.10**（三组裁定落册；
>    逐字节空口载荷 / 配对与 HKDF / W2 重发 / 两档 `nvs`·`ram-only`）/ **§4.4.12**（丙-① / 丙-②）
> 2. 机读契约：[`contracts/local/link-frames.json`](../../../../../contracts/local/link-frames.json)
>    的 `ble_gatt.adv_mfg` / `ble_gatt.pair.hkdf` / `ble_gatt.pair.persist_tier` / `ble_gatt.pair.sn_mint_tier`
> 3. 设备侧实现（**只读**，⛔ 本工程不改它）：[`components/oneye-dev-sdk`](../../../components/oneye-dev-sdk)
>
> ⚠️ **本工程不是** korvo2_oneye 的"精简版"：它是**另一块板子**的固件，
> 板级前提（无音频/无卡槽）与机型档位（乙/丙）都不同；两工程的**共同部分**只有
> 「零音频依赖的配网件」，那是**复制**过来的（见 §2）。
>
> **工具链口径**：单轨 IDF **v5.5.5**（本仓既有口径）。
> ★ **本工程⛔ 不链接 ESP-ADF** ⇒ korvo2_oneye README §1 那条「必须给 IDF 打 ADF 的
> FreeRTOS 补丁」的前置**在本工程不存在**（那个补丁是给 ADF 的 `xTaskCreateRestrictedPinnedToCore`
> 用的，本工程一个 ADF 组件都不进构建）。

---

## 1. 目录结构与「裁掉了什么、为什么零音频」

| 文件 | 来源 | 说明 |
| --- | --- | --- |
| `CMakeLists.txt` | **新写** | 顶层：**不 include ADF**，只把 `oneye-dev-sdk` **一个组件目录**挂进 `EXTRA_COMPONENT_DIRS`（理由见该文件注释，⚠️ 挂 `components/` 整目录会去编 ADF 的音频件并在 `xSemaphoreHandle` 上编不过） |
| `partitions.csv` | 改写（源 = korvo2 同名文件） | ⛔ 无 `model`（esp-sr AFE 模型分区）/ ⛔ 无 `storage`（录音兜底）；`factory` 2 MB；`creds` **保留但挪到 0x210000**（另一块板子，无"按 0x510000 烧好的凭据"） |
| `sdkconfig.defaults` | 改写 | 通用缺省：bootloader/分区偏移、日志级别、**承载与端点钉死**；⛔ 无 PSRAM / FATFS / SPIFFS / ADF 段 |
| `sdkconfig.defaults.esp32s3` | 改写 | `esp32s3` / 240 MHz / 8 MB flash / **显式关 PSRAM**；⛔ 无 ADF 板卡选择、⛔ 无 esp-sr 段 |
| `sdkconfig.defaults.ble` | 复制后**订正两处** | NimBLE 全套（主机栈 12 KB、MSYS 12、共存…）；订正 = ①⛔ 不开 PSRAM ⇒ `BT_NIMBLE_MEM_ALLOC_MODE_INTERNAL`；② 保留 IDF 缺省的 Wi-Fi 缓冲（源预设的 8/16 是"Wi-Fi + BLE + AFE 三栈共存"取值） |
| `sdkconfig.defaults.beta` | **新写** | 乙档：`ONBOARDING_ROLE_BETA` ＋ `DEV_CRED_REQUIRED=n` ＋ SDK `DEV_CREDS_REQUIRED=n` |
| `sdkconfig.defaults.gamma` | **新写** | 丙档：`ONBOARDING_ROLE_GAMMA` ＋ 同上两项 `n` |
| `main/CMakeLists.txt` | 改写 | `REQUIRES` **只**留 `oneye-dev-sdk esp_wifi esp_netif esp_event nvs_flash mbedtls json bt driver esp_timer lwip` |
| `main/Kconfig.projbuild` | 改写（同源符号名/语义） | `ONEYE_FW_ONBOARDING_ROLE_{ALPHA,BETA,GAMMA}` / `ONEYE_FW_DEV_CRED_REQUIRED` / `ONEYE_FW_ENABLE_BLE_PROV` / 门控与窗口 / `ONEYE_FW_PRODUCT_KEY` / `ONEYE_FW_DEVICE_ID` / `ONEYE_FW_DIAG_PROV_TRACE`；⛔ 删掉 korvo2 的板级开关（SDCARD/LCD/KEYS/AEC/PANEL/LOG_PROBE/PROV_ATTEST） |
| `main/devkitc_oneye_main.c` | **新写**（最小 main） | 档位自证 + 身份自证 + NVS 探测 + 收敛点 + 按键 + 心跳；见 §3 |
| `main/wifi_prov.c/.h` | **复制**（源 = korvo2_oneye） | Wi-Fi 生命周期 ＋ **入网后收敛点**（唯一 `oneye_start()` 触发边）＋ 优先级判据落点 |
| `main/wifi_prov_ble.c/.h` | **复制 ＋ 1 处订正** | BLE 通道 C1/C2。**订正 = 乙档 `prov.pair.ok.sig` 的材料注入**（见 §5 缺口 ③） |
| `main/wifi_prov_status.h` | **复制** | `prov.status{connected,ip}` 上报判据（纯逻辑；固件与主机夹具同源） |
| `main/wifi_prov_policy.h` | **复制** | 通道优先级判据（纯逻辑） |
| `main/adv_identity.c/.h` | **复制** | 设备身份的**唯一**读取点（`creds` 的 `node_id`/`sn` 两串；含档位感知的 `_ex` 版） |
| `main/ble_sig_material.c/.h` | **新写** | 乙档 `sig` 材料注入点（**单独 TU**：`oneye_dev_ble.h` 与 `oneye_dev_link.h` 的 `oneye_dev_link_status_t` 同名不同构 ⇒ 同 TU 必编译失败） |
| `main/idf_component.yml` | **新写（空依赖）** | 本工程**无 registry 依赖** ⇒ `managed_components/` 构建前后都是空集合 |
| `tools/build-devkitc.sh` | **新写** | 两档固件的**受控产出脚本**（§4） |

### 1.1 零音频依赖是怎么落地的（**三层，⛔ 不是"编了但没用"**）

| 层 | 落法 | 判据 |
| --- | --- | --- |
| **构建图** | 顶层 `CMakeLists.txt` **不 include ADF**，只挂 `oneye-dev-sdk` 一个组件目录 | ADF 的 `audio_board` / `audio_hal` / `esp_peripherals` / `display_service` … **不进构建图**（实测：挂 `components/` 整目录会去编它们并在 `xSemaphoreHandle` 上编不过） |
| **组件依赖** | `main/CMakeLists.txt` 的 `REQUIRES` 白名单 | 源码里出现 `board.h` / `es7210.h` / `audio_board_init()` ⇒ **编不过** |
| **源码** | `main/devkitc_oneye_main.c` 里**没有** `board_selftest()` / `board_init_peripherals()` / `keys_start()` / `aec_capture_init()` / `player_init()` / `camera_api_init()` / `panel_api_*` | 不引 ADF 的 `input_key_service`（改用 IDF `driver/gpio` 读 BOOT 键） |

### 1.2 与 korvo2_oneye 的**逐项差异**（⛔ 裁掉了什么、为什么）

| korvo2_oneye 的东西 | 本工程 | 为什么 |
| --- | --- | --- |
| `audio_board_init()` / `board_expect.h` 编译期断言 / `SELFTEST_STRICT` 死等 | **无** | 就是"不能直接跑在 DevKitC-1"的三条成因（已登记结论） |
| `panel_api.c` 本地 HTTP 验证面 | **无** | DevKitC-1 场景的证据面 = **串口 ＋ App**；引 httpd 会多一份依赖，与"最小"相反 |
| `lcd_ui.c` / `camera_api.c` / `player.c` / `aec_capture.c` / `media_api.c` | **无** | 板上没有这些外设 |
| `net_probe.c` / `sntp_boot.c` | **无** | ⚠️ **如实登记**：本工程因此**没有** net_probe 自检与启动期 SNTP 授时。乙档是**严格 TLS**（生产口径）⇒ 缺 SNTP 会让"先有钟、再建链"这条顺序**只能靠 SDK 的 `cloud_ts` 授时**（`caps/down.cloud_ts`）；而 `cloud_ts` 要等 `CLOUD_LINK_UP` ⇒ **存在 bootstrap 死锁风险**（korvo2 专门为它把 SNTP 搬到收敛点）。**本工程按不引入 ADF 的取舍优先**，把它登记为缺口（见 §7） |
| `sd-root/` / `tools/provision-device.ps1` | **无** | 无 SD 卡、无 `creds` 分区灌注（乙/丙 量产形态**不需要** `creds`） |
| `tools/check-prov-convergence.py` / `tools/test-prov-priority.c` | **无（有意删）** | 那两个门禁**硬编码** `korvo2_oneye_main.c` 的文件名与 A6（`wifi_prov_load_file` 调用 ×2）/ A7（`oneye_start` 调用点）/ A13（`[res]` 三处含 `app_main`、`wifi_prov_boot`、`cloud_start_task` 与 `phase=` 取值）/ A14a 判据。本工程**结构性不同**（无 C3 文件通道、`cloud_start_task` 在丙档不编、`[res]` 有 diag 第四次调用）⇒ 直接跑会**假红**（把"机型不同"报成"收敛点被破坏"）。⇒ 本工程改为**逐条语义复核**（读数见 §6.4），⛔ 不是"少做了一道门禁" |
| `CONFIG_FREERTOS_ENABLE_BACKWARD_COMPATIBILITY` | ⛔ **不设** | 那条是给 ADF 的 FreeRTOS 补丁用的；本工程不链接 ADF |
| PSRAM（`SPIRAM*`） | ⛔ **显式关** | 零音频依赖没有需要 PSRAM 的缓冲；DevKitC-1 模组 PSRAM 随批次不同 ⇒ **不假定有** |
| `ONEYE_FW_PROV_ATTEST`（产测自证串） | ⛔ 无 | 乙/丙 量产形态无 `creds` 分区 ⇒ 自证串**恒不打印**（留着就是"看起来有、实际不产"的死配置） |

---

## 2. 复用件是**复制**过来的（⛔ 未改 `korvo2_oneye/` 一个字节）

四个 `.c` ＋ 四个 `.h` 从 `examples/oneye/korvo2_oneye/main/` **复制**而来：
`wifi_prov.c/.h`、`wifi_prov_ble.c/.h`、`wifi_prov_status.h`、`wifi_prov_policy.h`、`adv_identity.c/.h`。

**对复制件唯一的改动**（⛔ 两处，逐条登记）：

1. `wifi_prov_ble.c`：`#include "ble_sig_material.h"` ＋ 在 `oneye_dev_ble_config_defaults(&bcfg)`
   之后插入 12 行调用 `oneye_dev_ble_sig_material_apply()` 填 `bcfg.sig_cert_pem` / `sig_key_pem`。
   **理由**：源目录的 `wifi_prov_ble.c` **没有**填这两个字段，而 SDK 的 `sig` 产出要求
   「档位显式 = 乙」**且**「两个 PEM 都给」⇒ 乙档即使嵌入了证书也**不会**产 `prov.pair.ok.sig`。
   ⇒ 这是**如实补齐**，不是"改判据"（判据仍在 SDK 里）。
2. `adv_identity.c/.h`、`wifi_prov.c/.h`、`wifi_prov_status.h`、`wifi_prov_policy.h`：**逐字节未改**。

---

## 3. 两档的实现要点（逐条对照契约）

### 3.1 乙类（BETA）—— `sdkconfig.defaults.beta`

| 契约要求 | 本工程落点 | 依据 |
| --- | --- | --- |
| ⛔ **无 `creds` 身份** | `ONEYE_FW_DEV_CREDS_REQUIRED=n` ＋ `ONEYE_FW_DEV_CRED_REQUIRED=n`；`creds` 分区读不到 ⇒ **正常起点**（不 fail-closed） | §4.4.8（〇）：乙有"品类资格"、无"实例身份" |
| 广播**带厂商字段**，载荷 = `0x28` ＋ 空 `sn_field` ＋ 零 `reserved` | 空口字节由 **SDK 平台层**按档位产出（`adv_sn = NULL` ＋ `tier = 乙`），本工程只把 `bcfg.adv_device_tier_set = true` / `adv_device_tier = ONEYE_DEV_TIER_B_CERT_DEVICE` 交出去 | §4.4.10（一）1.：`out[0]==0x28`、`out[1..24]` 全 0、`out[25..26]` 全 0 |
| **上平台前无实例身份** | 启动日志如实打印"**无实例身份**（正常起点）"，⛔ 不回落 `CONFIG_ONEYE_FW_DEVICE_ID` 当身份 | §4.4.8（一）T1/T2 |
| 内嵌证书 = **品类级认证 / 入网准入前提**（⛔ 非一机一密、⛔ 非身份） | `-DONEYE_FW_CERT_DIR=<非空目录>` 嵌入三件 PEM；编译期 `#error`（`BETA && !EMBED_CERTS` ⇒ 构建失败）；`credSource` 如实报 `embedded` | §4.4.8（〇）三层表 |
| `prov.verify.req` **必经**（判据 = 形态/有效性，权威在云侧） | SDK 按档位分派（`oneye_ble_verify_decide_tier()`），本工程不干预；启动日志打印"必经：node_id 形态/有效性" | §4.4.10（三）/（五） |
| 配对：⛔ **不判 `code`**，唯一判据 = `chal` 合法 | SDK 侧（⛔ 本工程无第二判据）；启动日志打印"配对判据 = 一次性 chal（⛔ 配对阶段不判 code）" | §4.4.10（三） |
| HKDF `ikm = code‖chal`、`salt = chal`、golden `4e17d73c…` | SDK 侧（`oneye_ble_pair_derive_key_chal()`）；本工程⛔ 不复算、⛔ 不改 | §4.4.10（四） |
| 绑定成功由 **App 重发同帧**置 `commit=1`（W1/W2 两段写） | SDK 侧（`oneye_ble_bindid*`，2026-10-10 新增）；⚠️ **本工程只提供 NVS 可用性证据**（启动打印 `NVS：可用/不可用`），⛔ 不代 SDK 表态档位 | §4.4.10（十）3. |
| 持久化 = **尽力而为**（`nvs`/`ram-only` 两档），写失败**仍回 rsp** | SDK 侧；⛔ 本工程**不落地任何身份**、⛔ 不写 `creds` | §4.4.10（十）2. |
| ⛔ 不声称"已确认身份" | 启动日志与心跳里**没有**这类措辞；`NVS 不可用` 时明确写"本会话有效、上电即失效，是**合法配置**" | §4.4.10（十）2.4 第 3 条 |

### 3.2 丙类（GAMMA）—— `sdkconfig.defaults.gamma`

| 契约要求 | 本工程落点 | 依据 |
| --- | --- | --- |
| ⛔ **无 `creds`** | 同乙（两项 `n`） | §4.4.2 丙列 |
| ⛔ **不连云** | `oneye_start()` 在丙档**函数体第一段** `#if GAMMA` 直接 `return`（**不** `base_init` / **不** `base_start`）；`cloud_start_task` 在丙档**不编进固件**；`on_wifi_ready()` 的丙分支**不建任务** | §4.4.2 丙列：入网/绑定由 **App 代发** |
| 设备侧复核 **SKIP（零帧）** | SDK 侧按档位分派（`dev_onboarding_tier() == C` ⇒ SKIP）；本工程⛔ 不干预 | §4.4.6（二）/§4.4.10（一） |
| ⛔ **不落地身份** | 本工程**没有任何** NVS 写路径；⛔ 不写 `creds` | §4.4.10（六）4. |
| 广播厂商字段**可有可无** | SDK 按档位产出（`adv_sn = NULL` ＋ `tier = 丙`）；⛔ **不嵌入证书**（`-DONEYE_FW_CERT_DIR=<空目录>`） | §4.4.10（二） |
| 入网/绑定由 App 代发 | 收敛点下游只做 `prov.status{connected}` 上报 + 一行"⛔ 不建云链路"的告警 | §4.4.2 丙列 |

### 3.3 两档**共用**的部分

* **按键**：长按 **BOOT（GPIO0，低有效、有外部上拉）** ≥ 1 s 切换 BLE 配网模式
  （进入 / 退出）。缺省 `ONEYE_FW_BLE_PROV_KEY_GATE=n` ⇒ **开机即广播**（配网+绑定闭环验证的
  第一诉求是"上电就能被 App 发现"），长按仍可用于**退出 / 重新进入**。
  ⛔ 与 korvo2 的**本质差别**：那里是板载 6 键 **ADC 梯形 + ADF `input_key_service`**，
  这里是 **IDF `driver/gpio` 读单键**。
* **配网窗口**：`ONEYE_FW_BLE_PROV_PAIRING_WINDOW_S = 300`（与 SDK 的
  `ONEYE_DEV_SDK_BLE_CHAL_TTL_S = 300` 对齐 ⇒「chal 失效」与「窗口关闭」同一刻）。
* **心跳 / 诊断**：`CONFIG_ONEYE_FW_DIAG_PROV_TRACE=y` 时每 **5 s** 一行
  （档位 / uptime / 运行期日志级别 / Wi-Fi 状态 / 配网模式 / 广播中 / `[res]` 资源读数）
  ＋ 把 `wifi_prov` / `wifi_prov_ble` / `oneye_ble` / `oneye_link` / `oneye` 五个 tag 放宽到 DEBUG。
  ⛔ 只读状态：不发帧、不改判据、⛔ 不打任何机密（密码 / 激活码 / chal / 令牌）。
* **资源读数 `[res]`**：`pre-prov`（配网与 NimBLE 之前）· `ble-ready`（NimBLE 已起栈、未入网）
  · `cloud-start`（入网后；**丙档不产**）· `diag-heartbeat`（仅诊断构建）。
  量 = `free_internal` / `largest_internal_block` / `free_psram`。
  ⚠️ `free_psram` **恒为 0**（本工程显式关 PSRAM）—— 那是**如实读数**，⛔ 不是"PSRAM 用完了"。

### 3.4 `product_key` / 机型标识（**逐字取处与当前值**）

| 项 | Kconfig 符号 | **当前值（逐字）** | 取处 |
| --- | --- | --- | --- |
| 产品品类 | `CONFIG_ONEYE_FW_PRODUCT_KEY` | **`OY-SPK-01`** | 取值出处 = [`korvo2_oneye/main/Kconfig.projbuild:14-20`](../korvo2_oneye/main/Kconfig.projbuild)（其 help 逐字指向 `cloud/docs/00-naming-convention.md:4604-4607` 的机型清单）；本工程逐字沿用 |
| 节点标识 | `CONFIG_ONEYE_FW_DEVICE_ID` | **`devkitc-0001`** | 本工程 `main/Kconfig.projbuild`（**台面占位**；乙/丙 的实例身份在绑定后由 App↔cloud 赋予，⛔ 本值不是身份） |
| 固件版本 | `CONFIG_ONEYE_FW_VERSION` | **`0.1.0-devkitc`** | 本工程 `main/Kconfig.projbuild` |

🔴 **cloud 侧 `dev_product_credential_policy` 需要为乙/丙 配的行**（按用户口径）：

| 机型 | `credential_required` | `onboarding_actor` | 落在本工程 |
| --- | --- | --- | --- |
| 乙 BETA | **`false`** | **`DEVICE`** | `ONEYE_FW_ONBOARDING_ROLE_BETA` ＋ `ONEYE_FW_DEV_CRED_REQUIRED=n` |
| 丙 GAMMA | **`false`** | **`APP`** | `ONEYE_FW_ONBOARDING_ROLE_GAMMA` ＋ `ONEYE_FW_DEV_CRED_REQUIRED=n` |

⚠️ **两档共用同一个 `product_key`（`OY-SPK-01`）** ⇒ cloud 侧**不能**靠 `product_key`
区分乙与丙，而机型表是**按 `product_key` 一行**登记的 ⇒ **同一 `product_key` 上不能同时登记
乙与丙**（两者 `onboarding_actor` 不同）。**这是需要 cloud 侧裁定的缺口**（见 §7-⑤），
本工程⛔ 不自行新造机型码。

---

## 4. 两档固件的受控产出

> ⛔ **不要手敲 `idf.py build`**：四件"少一件就产出错误固件"的东西写在脚本里（脚本头逐条给理由）。

```bash
# WSL
source ~/esp/esp-idf-5.5.5/export.sh
export ADF_PATH=/mnt/e/workspace/rainmaker-oneye/embedded/esp-adf   # ⚠️ 缺它会触发组件管理器行为差异
cd $ADF_PATH/examples/oneye/devkitc_oneye

bash tools/build-devkitc.sh beta  /tmp/oneye-devkitc-certs    # 乙：证书目录须含 ca.crt/client.crt/client.key
bash tools/build-devkitc.sh gamma /tmp/oneye-empty-certs      # 丙：给一个**空**目录
```

脚本固定做的四件事：

| # | 做的事 | 不做会怎样 |
| --- | --- | --- |
| ① | `export ADF_PATH`（不导出就报错退出） | 组件管理器行为差异；且找不到 `oneye-dev-sdk` |
| ② | `-DONEYE_SDK_LIB_OUT_DIR=/nonexistent-oneye-lib-out` | 走预编译库时，**归档与源码不同源**会 `FATAL_ERROR`（版本 / `src_sha256` 对不上） |
| ③ | `-DSDKCONFIG=<构建目录>/sdkconfig` | 工作区那份 gitignored `sdkconfig` 会**盖住**预设，且构建反过来改写它 |
| ④ | `-DSDKCONFIG_DEFAULTS="…;sdkconfig.defaults.<档位>"` | 档位回落 Kconfig 缺省 = **甲**（最严）⇒ 编出一个与预期**不同档**的件 |

### 4.1 产出（3 bin；⛔ 无 `erase_flash`）

> ⚠️ **本工程只有 3 个可烧写镜像**（⛔ 不是 4 个）：korvo2 的 3 + 1 里，第 4 件是
> `srmodels/srmodels.bin`（esp-sr 的 `model` 分区），**本工程零音频依赖、没有 `model` 分区**
> ⇒ **不产出**。统一烧写口径 = 「**3 bin**」。
> 若把 `creds` 分区算作"第 4 件"：本工程**两端都不提供 `creds.img`**
> （乙/丙 的量产形态**不需要** `creds` 分区，契约 §4.4.8（〇））。

**乙（BETA）** —— `bash tools/build-devkitc.sh beta /tmp/oneye-devkitc-certs`

| 镜像 | 大小（B） | sha256 |
| --- | ---: | --- |
| `build-beta/bootloader/bootloader.bin` | 20 832 | `ea45eea0df2f7c44d7c0f28daf1563aa52438a76d8ccf93beee976ee1366d8b8` |
| `build-beta/partition_table/partition-table.bin` | 3 072 | `447a2fbaeb35db4fd34ebee0edbd9c689810d24f4581a17bd02915b3be71267d` |
| `build-beta/devkitc_oneye.bin` | 1 252 064 | `a341a79b3cb2720458bf9b38631667752f22f17b734be46b7b6852aef257f3a7` |

**丙（GAMMA）** —— `bash tools/build-devkitc.sh gamma /tmp/oneye-empty-certs`

| 镜像 | 大小（B） | sha256 |
| --- | ---: | --- |
| `build-gamma/bootloader/bootloader.bin` | 20 832 | `4c708000c68d21f07df32add721c12af00902c16f59b11c044cd674f169604ea` |
| `build-gamma/partition_table/partition-table.bin` | 3 072 | `447a2fbaeb35db4fd34ebee0edbd9c689810d24f4581a17bd02915b3be71267d` |
| `build-gamma/devkitc_oneye.bin` | 1 208 800 | `ca9d4bdb530482067ec4935b3a725b63acb721800e18ab41973f2ac00d07b5e2` |

> ⚠️ 两档的 `partition-table.bin` sha256 **相同**是**预期**：分区表由 `partitions.csv` 唯一决定，
> 而两档**不分区**（同一块板子、同一份表）。`bootloader.bin` 不同是因为它嵌了不同的构建元数据。

**两档 `IDF_EXIT=0`**（脚本内 `idf.py build` 的退出码；✅ 见 §6.3）。
**体积余量**：乙 `0x131ae0 / 0x200000`（余 40 %）；丙 `0x1271e0 / 0x200000`（余 42 %）。

### 4.2 烧写（ESP32-S3-DevKitC-1，3 个偏移）

```bash
# WSL（把 /dev/ttyACM0 换成实际口；Windows 侧用 COM13）
python -m esptool --chip esp32s3 -b 460800 \
  --before default_reset --after hard_reset \
  write_flash --flash_mode dio --flash_size detect --flash_freq 80m \
  0x0     build-beta/bootloader/bootloader.bin \
  0x8000  build-beta/partition_table/partition-table.bin \
  0x10000 build-beta/devkitc_oneye.bin
```

* **四偏移口径**（= IDF 缺省 ＋ 本表）：`0x0` bootloader · `0x8000` partition-table ·
  `0x10000` app · **`0x210000` `creds`（⚠️ 本工程**不烧**它：乙/丙 无 `creds`）**。
* ⛔ **不执行 `erase_flash`**：分区表偏移与 korvo2 **不同**（`creds` 从 `0x510000` 挪到 `0x210000`），
  但本工程**不使用** `creds` ⇒ 无需先擦；⛔ 也不要为了"干净"去擦 —— 那会清掉板上与本工程无关的内容。
* ⚠️ 换 `-b`（baud）不改偏移；`--flash_size detect` 会按实测 flash 走（本工程配置钉 8 MB）。
* ⚠️ **与本工程同板**若曾烧过 korvo2_oneye（其分区表把 `creds` 放在 `0x510000`、
  `factory` 4 MB）⇒ 两者**不可共存**：烧本工程前必须**按本工程的 3 个偏移重烧分区表**
  （上面那条命令已含 `0x8000`），否则会出现"分区表说 2 MB、实际残留 4 MB 旧表"的错位。

---

## 5. 缺口与占位（⛔ **如实登记，⛔ 不静默省略**）

① 🔴 **乙档的内嵌证书 = 测试件，不是品类级认证件**。
   本工程的乙档证明了「**存在一条真实的、可产出 `prov.pair.ok.sig` 的代码路径**」：
   证书材料由 `-DONEYE_FW_CERT_DIR=` 嵌入、`ca.crt` 交 mbedtls 作信任链、
   `client.crt`/`client.key` 既作 base 的 `credential`/`credential_key`，又作 `sig` 的签名材料。
   ⚠️ **但它不是产品侧的"品类级认证"件**：
   * 构建所用的三件 PEM 是**本机自签的测试件**（`CN=devkitc-category-cert`，见 §6.5 的生成命令），
     **未过任何产品/生产 CA**，⛔ 不能据此声称"乙档已通过平台入网准入"；
   * 生产上 `ca.crt` 必须是**平台 CA**（否则严格 TLS 必然握手失败）、`client.crt` 必须是
     **产品侧签发的品类级证书**；
   * **哪些是真实路径 / 哪些是占位**：
     - **真实**：证书→固件嵌入→base 凭据→TLS 客户端凭据、证书→`sig` 签名（两条都是**代码路径真实**）；
     - **占位**：证书**内容本身**（自签测试件）、`product_key`（沿用 `OY-SPK-01`，⛔ 未为 DevKitC 新造机型码）、
       `CONFIG_ONEYE_FW_DEVICE_ID`（台面占位）。
   * 对闭环验证的影响：**不会**阻断"乙档被 App 发现 → 配对（chal）→ verify 复核"这一段
     （那一段不依赖证书）；**会**阻断"乙档以证书接入平台 8883/18886"这一段
     （严格 TLS 下自签 CA 连不上真平台）—— 因此本工程在无真品类证书时**只应验证到绑定前后**。

② 🔴 **`product_key` 无法区分乙与丙 ⇒ cloud 机型表按 `product_key` 一行登记 ⇒ 冲突**（见 §3.4）。
   需要一个裁定：① 为 DevKitC 试验件新造两个机型码（要动 `cloud/docs/00-naming-convention.md` 的机型清单）；
   或 ② 接受"同一 `product_key` 只登记一档，另一档用另一块板/另一个 `product_key`"；
   或 ③ 机型表扩第三根轴（⚠️ §4.4.12（三）已论证**不需要**第三根轴）。

③ ✅ **乙档 `prov.pair.ok.sig` 的产出路径在本工程被补齐**（源目录缺 `sig_cert_pem`/`sig_key_pem` 赋值）。
   ⚠️ 如实登记：**这是本工程对复制件的唯一功能性改动**（§2-1）。
   ⚠️ 另一条登记：`sig` 只有当 `ONEYE_DEV_TIER_B_CERT_DEVICE` **显式**声明时才产出
   （SDK 的 `ble_sig_enabled()`）；本工程 `adv_device_tier_set = true` 已满足。
   ⚠️ **仍未在真机验证**："证书与私钥配对 ⇒ 签得出 `sig`"这条在**宿主单测**里有过
   （`tests/test_ble_verify.c` 的 `dev_up_tier(B, cert, key)`），真机未验（⛔ 本批不烧写）。

④ 🔴 **本工程无 SNTP 授时 / 无 net_probe**（§1.2）⇒ 乙档严格 TLS 下存在
   「设备无有效时钟 ⇒ `BADCERT_FUTURE` ⇒ 连不上 ⇒ 拿不到 `cloud_ts` ⇒ 更连不上」的
   **bootstrap 死锁风险**。⚠️ 这是"零音频依赖 + 不引 ADF"取舍的**直接代价**，如实登记；
   若要闭合，需把 `sntp_boot.c`（2 154 B、只依赖 IDF `esp_sntp`）搬进收窄点 —— 属**下一批**。

⑤ ⚠️ **`managed_components`**：本工程 `main/idf_component.yml` 为**空依赖**，
   构建前 `managed_components/` **不存在** ⇒ 构建后仍不存在（空集合 = 空集合）。读数见 §6.3。

⑥ ⚠️ **本工程不做 NVS 写**（乙档的 W1/W2 两段写在 SDK 内）。
   ⇒ 本工程能给的是「**媒介前提成立**」（启动打印 `NVS：可用`），
   ⛔ **不能**给"`commit` 位真的置上了"的证据 —— 那要等真机跑 App 侧 W1/W2。

---

## 6. 测试读数（本轮实测）

### 6.1 SDK 宿主单测

```bash
cd embedded/esp-adf/components/oneye-dev-sdk && bash tools/build-linux.sh --run
```

**读数：`=== 汇总：27 组 / 313 用例，断言 7059 次、失败 0 次，失败组 0 ===`；`HOST_EXIT=0`**（与基线一致）。

### 6.2 ABI 门禁

```bash
cd embedded/esp-adf/components/oneye-dev-sdk
bash tools/abi-check.sh lib/x86_64-linux-gnu-gcc-13.3.0
```

**读数：`OK: 符号白名单门禁通过（6 个库；内嵌件符号零外泄）`**；
逐库：`oneye_dev_ble` **公共 API 23 个**（`abi/oneye_dev_ble.symbols` = 23 行）、
`oneye_dev_link` **公共 API 34 个**（`abi/oneye_dev_link.symbols` = 34 行）—— 与任务基线一致。

### 6.3 `managed_components` 构建前后一致 + 两套固件 `IDF_EXIT`

| 项 | 乙（BETA） | 丙（GAMMA） |
| --- | --- | --- |
| **构建后** `<build>/managed_components/` | **不存在** | **不存在** |
| **构建后** 工程目录 `managed_components/` | **不存在**（`ls` 无输出） | **不存在** |
| 构建**前** | 不存在 | 不存在 |
| ⇒ 结论 | **一致（空集合 = 空集合）** | **一致（空集合 = 空集合）** |
| `IDF_EXIT` | **0** | **0** |
| 构建期 `error:` 计数 | **0** | **0** |
| 构建期 `warning:` 计数 | **1** | **1** |

> ⚠️ 两档各有 **1 条 warning**（如实登记，⛔ 不隐藏）：IDF 的
> `HINT: You are maybe using pre FreeRTOS V8.0.0 data types … Please turn on
> CONFIG_FREERTOS_ENABLE_BACKWARD_COMPATIBILITY explicitly`。
> **本工程有意不开**该兼容位（它只为 ADF 的 FreeRTOS 补丁存在，本工程不链接 ADF）；
> 该提示是**提示级**、⛔ 不阻断构建，且本工程代码**没有**使用 `xSemaphoreHandle` / `xTimerHandle`
> 这类旧类型（`error:` 计数 0 可证）。
> `ADF_PATH` 已导出（两档构建脚本都会先校验它，未导出即**退出码 2 拒绝构建**）⇒ 与"缺 ADF_PATH
> 会触发组件管理器行为差异"这条已知风险无关。

**乙档 `sig` 材料确实进了固件**（⛔ 不是"配了但没生效"）：

| 检查 | 乙 | 丙 | 说明 |
| --- | --- | --- | --- |
| ELF 里 `_binary_ca_crt_*` / `_binary_client_crt_*` / `_binary_client_key_*` 符号数 | **6**（三对 start/end） | **0** | `EMBED_FILES` 生效的直接证据 |
| CMake 提示 | `嵌入证书材料（ca.crt/client.crt/client.key）` | `未提供证书材料（…/main/certs）⇒ 不嵌入` | — |
| `build-*/sdkconfig` | `# CONFIG_ONEYE_DEV_CREDS_REQUIRED is not set` | 同左 | 两档**都不要求** `creds` 分区（乙/丙 的正确取值） |
| `build-*/config/sdkconfig.h` | `#define CONFIG_ONEYE_FW_ONBOARDING_ROLE_BETA 1` | `#define CONFIG_ONEYE_FW_ONBOARDING_ROLE_GAMMA 1` | 档位**显式**声明（乙的 `sig` 产出条件之一） |
| 构建进来的 ADF 音频/外设组件 | **0 个**（`build-*/esp-idf/` 里只有 `oneye-dev-sdk` 命中 `oneye`，无 `audio_*` / `esp_periph*` / `display*`） | 同左 | 「零音频依赖」在**构建图**上的读数 |

> ⚠️ 诚实边界：`sig` 的**运行期**产出（"证书与私钥配对 ⇒ 签得出 base64"）**未在真机验证**
> （⛔ 本批不烧写）；宿主侧由 `tests/test_ble_verify.c` 的 `dev_up_tier(B, cert, key)` 覆盖。

### 6.4 应用门禁（`check-prov-convergence.py`）—— ⛔ **不适用，改为逐条语义复核**

**为什么不适用**：该脚本 `HERE/..` 定位自身工程、只读 `main/korvo2_oneye_main.c` 与
`korvo2_oneye_main.c` 的符号，其 A6/A7/A13/A14a 判据**按 korvo2 的结构写死**
（见 §1.2 表）。本工程**结构性不同**（无 C3 文件通道 / 丙档不建 `cloud_start_task` /
诊断构建多一处 `[res]`）⇒ 跑它只会把"机型不同"报成"收敛点被破坏"。

**逐条语义复核（对本工程源码的机械检索，⛔ 不是"注释声称"）**：

| 不变量（korvo2 门禁对应条） | 本工程读数 | 结论 |
| --- | --- | --- |
| `oneye_start()` 调用点**唯一**，且在 `cloud_start_task` 内（A1/A7） | `main` 下全部 `.c` 中 `oneye_start()` 调用 **1 处**（`devkitc_oneye_main.c:471`，函数体内）；定义 1 处；`wifi_prov_ble.c` 只在注释里提到它 | ✅ |
| `cloud_start_task` 创建点**唯一**，且在 `on_wifi_ready` 内（A2） | `xTaskCreate(cloud_start_task, …)` **1 处**（`devkitc_oneye_main.c:511`，`on_wifi_ready()` 内） | ✅ |
| ready 回调**注册点唯一**且在 `app_main` 内（A3） | `wifi_prov_set_ready_cb(on_wifi_ready)` **1 处**（`app_main()` 内，:728） | ✅ |
| 收敛点**触发边唯一**（A4） | 触发边 = `wifi_prov.c` 的 `IP_EVENT_STA_GOT_IP` → `s_ready_cb()`（**未改**，逐字复制） | ✅ |
| 通道**不得**自建上云 / 不得直连 `esp_wifi_connect`（A5/A8） | `wifi_prov_ble.c` 只调 `wifi_prov_submit_credentials()`（:191）；⛔ 无 `oneye_start`、⛔ 无 `xTaskCreate`、⛔ 无 `esp_wifi_connect` | ✅ |
| 只入网一次（一次性守卫）（A9/A10） | `on_wifi_ready()` 内 `static bool started;` 守卫（**逐字沿用**） | ✅ |
| `prov.status{connected}` 上报点**唯一**（A14a/A14h） | `main` 下全部 `.c` 中 `ONEYE_DEV_LINK_PROV_CONNECTED` **1 处**（`wifi_prov_ble.c:258`）；`oneye_dev_link_prov_report_status()` 调用 **1 处**（`wifi_prov_ble.c:258`）；`main` 下 ⛔ 无 `prov.status` 帧名字面量 | ✅ |
| `[res]` 读数带 `phase=` 标签、覆盖"未入网也必经"的点（A13） | `res_report()` 4 处调用（`pre-prov` / `ble-ready` / `cloud-start`（无 PSRAM 读数恒 0）/ `diag-heartbeat`），前两处**未入网也必经** | ✅（⚠️ `cloud-start` 在丙档**不产**，已在 `#if` 下，非遗漏） |
| 通道模块**不得**出现 `net_probe_report` / `sntp_boot_sync`（A11/A12） | 本工程**根本没有**这两个符号（无 `net_probe.c` / `sntp_boot.c`） | ✅（⚠️ 代价见 §5-④） |

### 6.5 两档构建命令与证书生成（可复现）

```bash
# 乙档的测试证书（⛔ 自签测试件，不是产品品类证书；生成在 /tmp，不进仓库）
CD=/tmp/oneye-devkitc-certs && mkdir -p "$CD" /tmp/oneye-empty-certs
openssl ecparam -name prime256v1 -genkey -noout -out "$CD/client.key"
openssl req -new -x509 -key "$CD/client.key" -out "$CD/client.crt" -days 3650 \
  -subj "/CN=devkitc-category-cert/O=oneye/C=CN" -sha256
openssl req -new -x509 -key "$CD/client.key" -out "$CD/ca.crt" -days 3650 \
  -subj "/CN=devkitc-category-ca/O=oneye/C=CN" -sha256
```

---

## 7. ⛔ 未做 / 需裁定

| # | 项 | 状态 |
| --- | --- | --- |
| ① | **不烧写**（用户口径：lead 烧）⇒ 两档固件**未上真机**、⛔ 无串口读数、⛔ 无 App 对端联调 | ⛔ 未做 |
| ② | `product_key` 无法区分乙/丙 ⇒ cloud 机型表按 `product_key` 一行登记会冲突 | **需裁定**（§5-②） |
| ③ | 乙档内嵌证书 = **自签测试件**（⛔ 非产品品类级认证件）⇒ 只能验证到绑定前后，⛔ 不能声明"已过平台准入" | **需裁定 / 需产品侧发证书**（§5-①） |
| ④ | 无 SNTP 授时 ⇒ 乙档严格 TLS 下有 bootstrap 死锁风险 | **需裁定**（搬 `sntp_boot.c` 进本工程？） |
| ⑤ | `dev_product_credential_policy` **今天不存在 ＋ 空表** ⇒ 读侧恒 fail-closed **按甲** ⇒ 乙/丙 的端到端路径在 cloud 侧**今天走不通**（不是缺陷，是空表 + fail-closed，契约 §4.4.10（七·三）3. 已登记） | **需 cloud 侧落行** |
| ⑥ | 乙档 `sig` 的「证书与私钥配对 ⇒ 签得出」只在宿主单测里验过，**真机未验** | 待真机 |
