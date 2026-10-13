# ALPHA-SD-FALLBACK（2026-10-13）—— ① SD 卡配网失败 ⇒ **自动回落 BLE 配网模式**；② SDK 侧「网络线程不得含 flash 写」机械判据；③ 量产口径订正（文件通道 = 量产品类形态，凭据明文后果已接受）

> 用户裁定（逐字，最高依据）：
> 「**D2.SD卡配网失败回落BLE 配网模式**」
> 「**B2.(a)**」（= 落机械判据）
> 「**B1.量产支持且凭据明文**」（⇒ 文件通道是**量产品类形态**；明文密码后果 = **用户已接受**；⛔ 不再写"仅台面可用"）
>
> ⛔ 本批**不 commit / 不 push / 不烧写**（固件由 lead 烧）。
> ⛔ 未改 `mobile/`、`cloud/`、`backend/`、`contracts/`；⛔ 未改 `sdkconfig.defaults.ble`（无卡槽机型既有设定）；
> ⛔ 未改唯一判据 `main/wifi_prov_policy.h`（指纹逐值不变，见 §3.3）。
> 构建：WSL `~/esp/esp-idf-5.5.5/export.sh`、`ADF_PATH=/mnt/e/workspace/rainmaker-oneye/embedded/esp-adf`；产物落**仓库内**。

---

## ① D2 修法（旧 → 新）＋ 机械判据 ＋ 负向对照必红

### 1.1 旧 → 新（落码点唯一：`main/korvo2_oneye_main.c` 的 `wifi_prov_boot()`）

| # | 位置 | 旧行为（逐字留痕） | 新行为（本批） |
| --- | --- | --- | --- |
| ① | C3 失败分支（`wifi_prov_release_held("file-connect-failed")` 之后） | 只释放采信槽 ⇒ 落到 BLE 段仍按 `KEY_GATE` **待机** | **置一次性回落标志** `file_fell_back = true;`（本函数**局部量** ⇒ 每次上电最多一次） |
| ② | BLE 段（`wifi_prov_ble_start()` 之后） | `if (key_gate) { …"长按 SET 键进入"… } else { …"开机即广播"… }` | 新增**首分支** `if (file_fell_back) { wifi_prov_ble_pairing_enter(); …"**无需按键**"… } else if (key_gate) { …原样… } else { …原样… }` |
| ③ | 阶段读数标签 | `res_report("ble-ready（NimBLE 已起栈；gate=y 时未广播、入网前）")` | `res_report("ble-ready（NimBLE 已起栈；入网前；gate=y 且文件通道未回落时未广播）")`（同一处、同三个量；只把标签写准 —— 回落时此刻**在广播**） |

**旧行为的真机后果（本批修掉的正是这条）**：卡上有凭据但本次 20 s 入网失败 ⇒ 用户**必须再按一次 SET（或复位）**
才进 BLE 配网模式（`ALPHA-SDCARD.md` §3.4 旧口径已就地留痕并订正）。
**新行为**：释放采信槽后**自动**走 `wifi_prov_ble_pairing_enter()` —— 它与「长按 SET 进入配网模式」是**同一条**路径
（`ble_prov_key_toggle()` 的第二分支），因此 **300 s 窗口 / 超时自动退出**语义**一致**（不是"把 autostart 置 true"那种只覆盖 gate=y、且不起窗口计时的近似做法）。

### 1.2 ⛔ 保留的三条边界（本批不许动）

1. **`KEY_GATE` 原有语义逐字保留**：`wifi_prov_ble_set_autostart(!key_gate)` 与
   `#ifdef CONFIG_ONEYE_FW_BLE_PROV_KEY_GATE` 仍在；**开机且无文件**（未命中 / 解析失败 / 无卡槽）⇒ 仍**待机**、长按进入
   （⛔ 不是"开机即广播"）。回落**只**由 `if (file_fell_back)` 门控。
2. **唯一判据 / 通道收敛点不变**：`main/wifi_prov_policy.h` 的 rank 与 `wifi_prov_channel_is_authoritative()`
   **逐字未改**（sha256 与上一批**同值** = `ef98c177a0420c87ac658f59da9a8fe5011f37fc954031ad72e195eb6e16a604` ⇒ 机械证据，见 §3.3）；
   ⛔ 未新增通道 / 帧 / 字段 / 错误码；`oneye_start()` 仍只有收敛点下游**一个**调用点（A1 仍断言唯一）。
3. **只回落一次**：标志是 `wifi_prov_boot()` 的局部量 ＋ 该函数在 `app_main()` 里**恰好 1 个调用点**
   ＋ `wifi_prov_load_file()` **恰好 1 处** ⇒ ⛔ 不循环重连文件通道、⛔ 不反复重启广播。

### 1.3 机械判据（`tools/check-prov-convergence.py` 新增 **A18a–A18d**，共 4 条）

| 断言 | 判据（对**去注释后的代码面**） |
| --- | --- |
| **A18a** | `wifi_prov_boot()` 内 `file_fell_back = true;` **恰好 1 处**，且**在** `wifi_prov_release_held("file-connect-failed")` **之后**（顺序有语义：先释放采信槽，再据"本次已用掉文件通道"回落） |
| **A18b** | `wifi_prov_ble_pairing_enter()` 在 `wifi_prov_boot()` 内**恰好 1 处**、且在 `wifi_prov_ble_start()` **之后**；整份 `main/` 下该调用点**恰好 2 处**、所在函数恰为 `{ble_prov_key_toggle, wifi_prov_boot}` ⇒ ⛔ 无第三条进入路径 |
| **A18c** | 标志是函数**局部量**（`bool file_fell_back = false;`）＋ `wifi_prov_boot()` 调用点 **1 处**＋ `wifi_prov_load_file()` 调用点 **1 处** |
| **A18d** | `wifi_prov_ble_set_autostart(!key_gate)` 与 `#ifdef CONFIG_ONEYE_FW_BLE_PROV_KEY_GATE` 仍在；且 `pairing_enter()` 受 `if (file_fell_back) {` 门控 |

### 1.4 负向对照（**必须红**；实跑读数见 §5）

| 注入（把已知缺陷造回去） | 期望被捕获的断言 | 实跑 |
| --- | --- | --- |
| **删掉自动回落**（`esp_err_t prc = wifi_prov_ble_pairing_enter();` ⇒ `ESP_OK`） | **A18b** | ✓ 报红：`A18b … 期望恰好 1 处，实测 0 处` |
| 不置回落标志（`file_fell_back = true;` ⇒ `false`） | **A18a** | ✓ 报红：`A18a … 期望恰好 1 处，实测 0 处` |
| 把门控改成无条件（`if (file_fell_back) {` ⇒ `if (true) {`）= 开机即广播 | **A18d** | ✓ 报红：`A18d 缺门控 if (file_fell_back) { …` |
| 让 `wifi_prov_boot()` 被调第二遍（回落/分派跑两次） | **A18c** | ✓ 报红：`A18c wifi_prov_boot() 的调用点: 期望 1 处，实测 2 处` |

> 门禁总读数：**正向断言 A1–A18 PASS**；**负向对照 30 条全 PASS / 0 FAIL**；`GATE_EXIT=0`（§5）。

---

## ② B2 SDK 机械判据：「网络线程可达函数体内不得出现 flash 写类调用」

### 2.1 判据本体（新文件，随 SDK 仓走）

`components/oneye-dev-sdk/tools/check-net-thread-flash.py`（已接入 `tools/build-linux.sh`；同类判据先例：
`tests/test_prov_worker.c:479-676` 的 `rx_path_has_no_blocking_call()` —— 同样是"读 SDK 源码 + 结构断言"，其
CMake 侧注入点见 `tests/CMakeLists.txt:60-62` 的 `ONEYE_TEST_SDK_SRC_DIR`；本判据走 `tools/` 脚本形态，
**不改**宿主单测组数 ⇒ `28 组 / 320 用例` 基线不变）。

- **根**：由源码里的 `oneye_osal_thread_start(&t, "<name>", <entry>, …)` **自行解析**（⛔ 不硬编码入口函数），
  取名字 ∈ {`oneye_net`, `oneye_link`, `oneye_prov`} 者 ⇒ 实测 3 条根：
  `oneye_net=bint_net_thread`（`src/internal/oneye_internal.c`）、`oneye_link=link_wan_thread`（`src/oneye_dev_link.c`）、
  `oneye_prov=link_prov_worker`（`src/oneye_dev_link.c`）。
- **可达闭包**：按**函数名提及图**（比只看 `name(` 调用更保守 ⇒ 覆盖取地址/同 TU 注册的函数指针分派）。
- **禁止符号（逐字来自裁定，⛔ 不放宽/缩窄）**：`nvs_commit` \| `nvs_set_blob` \| `nvs_erase_key` \|
  `esp_partition_write` \| `esp_partition_erase` \| `esp_ota_`。

**实跑读数（本批）**：扫描 **35** 个 `.c`（解析出 **845** 个函数定义）；根 **3** 条；可达闭包 **260** 个函数；
闭包内 flash 写符号 **0 处** ⇒ `RESULT=PASS`、`B2_EXIT=0`。

### 2.2 判据强度与残余（如实登记，⛔ 不夸大）

| 维度 | 强度 |
| --- | --- |
| 分析面 | **源码面**（非二进制、非编译器 IR）；`#if` 条件编译**不建模**（按"两面都算"处理 ⇒ 宁严不松） |
| 调用图 | **按函数名**的提及图，**不是**编译器级调用图；**取地址 / 同 TU 注册**的分派可见 |
| **不可见（残余）** | ① **跨 TU 运行期装填**的函数指针分派（装填点不在闭包内）；② **预编译 blob / IDF 组件内部**的 flash 写 —— 已知唯一实例 = `esp_wifi_set_config()`（Wi-Fi 驱动内部的 NVS 写），它落在 **Wi-Fi 任务**，⛔ 不在本 SDK 的三条网络线程上；③ 内联汇编/宏生成的调用 |
| 结论口径 | 判据证明的是「**本仓源码里、三条网络线程按名字可达的任何函数体，都没有出现那份 flash 写符号表**」；对 ② 类残余**无能力**（需反汇编或运行期取证，属另一层门禁） |

**为什么这条判据值钱（本轮动机）**：今天已证实同类回归是**崩溃级**的 —— `cloud_start` 任务栈落 PSRAM ＋ flash 访问
⇒ `esp_task_stack_is_sane_cache_disabled()` assert 复位（见 `ALPHA-INTERNAL-RAM.md`）。"哪条线程允许碰 flash"必须是**结构判据**。

### 2.3 自带负向对照（实跑必红，与正跑同一次执行）

| 注入 | 结果 |
| --- | --- |
| **直接注入**：可达函数体（`link_wan_thread`）内插 `nvs_commit((nvs_handle_t)0);` | ✓ 报红（抓出 1 处：`src/oneye_dev_link.c:1231 link_wan_thread() 含 nvs_commit`） |
| **传递注入**：可达函数体调 `oneye_ble_bindid_write("injected", true);`（其平台面 = `nvs_set_blob`+`nvs_commit`） | ✓ 报红（抓出 2 处，例：`src/internal/oneye_ble_bindid_nvs.c:96 oneye_ble_bindid_plat_write() 含 nvs_commit`）⇒ 证明"可达性"不是空转 |
| **非空转阈值**：根 = 3、文件数 ≥ 10、函数表 ≥ 300、闭包 ≥ 40 | 任一不成立 ⇒ 报红（解析退化不会被当成 PASS） |

---

## ③ B1 口径订正（旧字逐字留痕）＋ 档位方案与产物 sdkconfig 证据

### 3.1 逐处订正（旧字逐字留痕，⛔ 不抹）

| # | 文件 | 旧字（逐字留痕） | 新口径 |
| --- | --- | --- | --- |
| ① | `main/Kconfig.projbuild:81-82` | 「⚠️ 文件在 SD 卡上是**明文** Wi-Fi 密码：有加密通道（BLE）的机型应以 BLE 为首选通道；该残余风险的归属见 ADR-0017 D3。」 | 该通道 = **量产品类形态**（不是"仅台面"）；明文后果**已由用户裁定接受**；⛔ 不得再写成"仅台面可用 / 明文是未决残余风险"（行为未改） |
| ② | `main/wifi_prov.h:54-56`（§5 安全口径） | 「5) ⚠️ 安全口径（未消除的残余风险，如实登记）：C3 在 SD 卡上是**明文** Wi-Fi 密码。解法是让有加密通道（C1/C2）的机型以 BLE 为首选，**不是**关掉唯一通道；剩余部分按 ADR-0017 D3 归属「配网凭据落盘形态」单独决策。」 | 用户裁定「B1.量产支持且凭据明文」⇒ 正名为量产品类形态；"BLE 优先"保留为**安全增强**（不是前提）；⛔ 不再记为残余风险 |
| ③ | `ALPHA-SDCARD.md` §3.5 | 「工程侧登记：`main/Kconfig.projbuild:81-82` 逐字…」「ADR-0017 已登记该后果…」「台面档 `TLS_INSECURE=y` **+** 卡上明文 Wi-Fi 密码 ⇒ **只允许在白盒台面使用**；⛔ 不得用于交付/公网场景。」 | 同上正名；`TLS_INSECURE` 那一项改为**单列**（它是"构建档"属性，**不是**文件通道属性）；⛔ 不再写"只允许白盒台面/不得交付" |
| ④ | `ALPHA-SDCARD.md` §3.4（时序边界行） | 「…并降级到 BLE ⇒ **需要再按一次 SET（或复位）**才能进 BLE 配网模式，不是自动的」 | 已由 D2 修掉 ⇒ 指向本报告（自动进入配网模式，无需再按键）；旧字留痕 |
| ⑤ | `sdkconfig.defaults.production:75`（注释） | 「· 明文 Wi-Fi 密码的**残余风险仍在**，但归属改为「配网凭据落盘形态」单独决策」 | 正名（⛔ 未增删任何配置符号 ⇒ 产物 sdkconfig 逐项不变） |
| ⑥ | `sdkconfig.defaults.production:119`（注释） | 「SD 卡明文密码的残余风险按 ADR-0017 D3 另行决策。」 | 正名（同上，仅注释） |
| ⑦ | `README.md:205`（例程 README「安全口径」格） | 「该残余风险按 ADR-0017 D3 归属「配网凭据落盘形态」另行决策」 | 正名 + 指向 §5.12 的 D2 口径 |
| ⑧ | `docs/adr/0017-多通道配网与入网后收敛点.md` D3（`:110`）与「后果（负面/成本）」（`:200`） | D3：「② 该风险的剩余部分按 D5 如实登记，留给「配网凭据落盘形态」单独决策。」；后果：「- C3 在 SD 卡上的**明文** Wi-Fi 密码风险仍在（D3 已明确归属，未消除）；」 | 两条均就地订正为「已由用户裁定接受」；并新增 **「决策记录（2026-10-13，逐字引入）」** 段（D2 / B2 / B1 三条裁定 + 落码与判据） |

### 3.2 档位方案（量产默认**已**打开该通道 ⇒ **无需新增 defaults**）

- **结论**：带卡槽机型的量产链 = `sdkconfig.defaults` → `sdkconfig.defaults.esp32s3` → `sdkconfig.defaults.production`；
  其中 `sdkconfig.defaults:101` 已写 `CONFIG_ONEYE_FW_ENABLE_WIFI_FILE=y`，而 `.production` **不再**覆盖它
  （ADR-0017 D3 已删除该覆盖行）⇒ **量产默认已打开 C3 文件通道**，⛔ **不需要**新 defaults、⛔ 未改 `sdkconfig.defaults.ble`。
- **机械证据（量产链档**联编**，产物 sdkconfig 行）**：另建 `build-alpha-prod-sd-evidence/`（defaults 链**不含** bench/diag/ble），
  `IDF_EXIT=0`；产物 `build-alpha-prod-sd-evidence/sdkconfig`：
  ```
  621:CONFIG_ONEYE_FW_ENABLE_WIFI_FILE=y          ← ★ 量产链上该通道**已打开**
  629:CONFIG_ONEYE_FW_ENABLE_SDCARD=y
  （`CONFIG_ONEYE_FW_ENABLE_BLE_PROV` 在该链上未定义 ⇒ NimBLE 关：有卡槽机型的量产链靠 C3 满足"至少一条通道"门禁）
  ```
- **同一链的 defaults 门禁读数（如实登记，⛔ 不掩盖）**：
  `--require sdkconfig.defaults.production` ⇒ `[ OK ] sdkconfig.defaults.production：11 项全部存在且已生效`；
  同次共 **13 项 FAIL，全部归属 `sdkconfig.defaults.ble`**（`CONFIG_BT_NIMBLE_*` / `CONFIG_ESP_COEX_SW_COEXIST_ENABLE`）——
  这是因为该链**刻意不含** `.ble`（BLE 关 ⇒ NimBLE 符号在本链 sdkconfig 里根本不存在），**不是** `production` 自身的问题。
- **⚠️ 边界（如实登记）**：D2 自动回落的**目标通道是 BLE**；因此量产链档（BLE 关）的固件里
  「已自动进入 BLE 配网模式 / 无需按键」两串**计数 0**（回落分支被 `#if CONFIG_ONEYE_FW_ENABLE_BLE_PROV` 编译掉），
  只有 `file-connect-failed`（WIFI_FILE 分支）在位。⇒ **D2 的自动回落只在"文件通道 ＋ BLE 通道同开"的构建上有落点**；
  无 BLE 通道的机型不存在"回落 BLE"这件事（其后续路径仍是既有 C5/无通道分支）。
- **编译回归对照（三种机型配置都编过）**：`WIFI_FILE=n ＋ BLE_PROV=y ＋ KEY_GATE=y`（= 无卡槽机型的既有链路
  `defaults → .esp32s3 → .production → .ble → .ble-key`）另编一次 ⇒ **`IDF_EXIT=0`**、产物 `sdkconfig`
  `:621 # CONFIG_ONEYE_FW_ENABLE_WIFI_FILE is not set` / `:624 ENABLE_BLE_PROV=y` / `:629 BLE_PROV_KEY_GATE=y`、
  编译日志中 `file_fell_back` **0 行**（无未用变量/未初始化告警）⇒ D2 的 `#if` 分层声明在"没有文件通道"的机型上**不产生回归**。
  （另一种配置 `WIFI_FILE=y ＋ BLE_PROV=n` 由 §3.2 的量产链档构建覆盖。）

### 3.3 源码指纹（sha256；证明"改了什么、没改什么"）

| 文件 | 上一批（ALPHA-SDCARD 报告） | 本批 | 判定 |
| --- | --- | --- | --- |
| `main/wifi_prov_policy.h` | `ef98c177a0420c87ac658f59da9a8fe5011f37fc954031ad72e195eb6e16a604` | **同值** | ★ 唯一判据**逐字未改**（机械证据） |
| `main/wifi_prov.c` | `3ca08cc235ac3b0b12bab17482ed74ae4860125df32e47823343c96e072b41a4` | **同值** | 本批未动（含并行批次的 A17 扫描修复） |
| `main/korvo2_oneye_main.c` | `1b26072299db32cd9363aba76f2db503ee93ef8b54c45ab7c6f2e085a543cc6a` | `cd417e1c8e29f5bb6974e736c27aec782c766893fc422b7ca83255d2fb6fca0a` | ★ D2 落码（旧→新） |
| `main/wifi_prov.h` | `a5d74fc8075cdfd66012ef60c41f84e7a46d1f94679594b8c960594aed391c33` | `4e175a0a86b88e67443f681dfe7b915fde130bf521c3fdbf204f2f46ae602143` | ★ B1 订正（§5）＋并行批次 A17 头注 |
| `tools/check-prov-convergence.py` | `5faeb9fe497eafc6e3e90e6f61746ea96f57f7e2feb2d1c1f9bccbe4ef1d3911` | `54b89165c64e4c7534784662a0f03de7f445e65127dfd4a40fbaa7ecfb5c3ffd` | ★ A18 + 4 条注入 |
| `main/Kconfig.projbuild` | — | `0b53fe8996fb92ab8758dba55a75e8664d000857ea4262efda513e679dcc5db3` | ★ B1 订正 |
| `sdkconfig.defaults.production` | — | `601b328bc47f6b2191f45e0549d00567c8175763a36b551876846ea1b8bedcf9` | ★ B1 订正（仅注释） |
| `sdkconfig.defaults` / `.bench-local` / `.bench-sdcard` | — / `bff10145…` / `e43ba4f2…` | `de636eb1e1173df1e2c73c76d0a92dca7713976a3518ff1e008e5729f25964a` / **同值** / **同值** | 本批未动（后两者逐值不变） |
| `README.md`（例程） / `ALPHA-SDCARD.md` | — / （新） | `892bb334af7d6829d30e5bd15ecad24cd934ea520553b0797c606fb5d3b5920a` / `3190073f8ac910c2ac05bebd5d09391ea5dfffb70e5e71f77d77e70b1c5c9f61` | ★ B1 订正 / D2 指向 |
| **SDK** `tools/check-net-thread-flash.py`（**新增**） | — | `fc6b58f19a9bbab97e2fb9b14d99a6a98ed454a3f2e9de98406bcfedfa709477` | ★ B2 判据 |
| **SDK** `tools/build-linux.sh` / `README.md` | — | `5a2fea213e40f5c626fde201b5a3c24611337f17c12f1466bc8fc68914461ff5` / `7e0ee8cf5e56a7ee4989efd5823a2596efe7d6ef3fbedf674fe9608437d36e33` | ★ B2 接入 + 门禁登记 |

---

## ④ 四 bin（仓库内路径 / 大小 / sha256）＋ `IDF_EXIT` ＋ 烧写命令（⛔ 未执行）＋ `managed_components`

**SD 档（含 D2 回落修复）构建口径**：`sdkconfig.defaults` ＋ `.esp32s3` ＋ `.production` ＋ `.ble` ＋ `.ble-key` ＋ `.diag`
＋ 台面档 `.bench-local` ＋ 机型补档 `.bench-sdcard`（末位）；`-DSDKCONFIG=<构建目录>/sdkconfig`（⛔ 不落工程根）、
`-DONEYE_FW_CERT_DIR=<空目录>`、`-DONEYE_SDK_LIB_OUT_DIR=/nonexistent-oneye-lib-out`（源码回落）；
构建目录 = 仓库内 `build-alpha-sd-fallback/`；**`IDF_EXIT=0`**（`[1721/1721]` 全量重配重编）。

| 偏移（⛔ 无 erase、⛔ 不写 `0x510000`） | 文件（Windows 仓库内路径） | 字节 | sha256 |
| --- | --- | --- | --- |
| `0x0` | `embedded/esp-adf/examples/oneye/korvo2_oneye/build-alpha-sd-fallback/bootloader/bootloader.bin` | 20832 | `c6569f0da7585af128c5de3a009e6ceef8967d328df86541315a783b18bb2639` |
| `0x8000` | `…/build-alpha-sd-fallback/partition_table/partition-table.bin` | 3072 | `dbb160ed9a93d3b036b5344ab12bcc88833d5d17df03c36c49a8f1c4fefac1bd` |
| `0x10000` | `…/build-alpha-sd-fallback/korvo2_oneye.bin` | **1668784**（`0x1976b0`） | `bd65dc84841afacb6d313a91b27ed733b64fc6ecc6b1fdbf3ee030882d8b6fb5` |
| `0x410000` | `…/build-alpha-sd-fallback/srmodels/srmodels.bin` | 337952 | `e93f68f637e71d71c3a0a271f1b7709ea0f58d4fe3315e031c8435bf865e51d6` |

- sha256 **Windows `Get-FileHash` 与 WSL `sha256sum` 两侧逐值一致**；余量 `0x268950 bytes (60%) free`。
- `partition-table.bin` / `srmodels.bin` 与上一批**逐字节相同**（未动分区表与模型）。
- ⚠️ `bootloader.bin` 与上一批**不同**（`60f7bc4c…` → `c6569f0d…`）：逐字节 diff = **38 B 不同**，首处差异即镜像内嵌的
  **构建时间串**（`Oct 13 2026 15:19:07` → `Oct 13 2026 17:33:22`）⇒ 该镜像是"非可复现构建"（未开 `CONFIG_APP_REPRODUCIBLE_BUILD`），
  **不是**功能差异（分区表/模型哈希不变即旁证）。
- **固件内自证（逐子串计数于 `korvo2_oneye.bin`）**：`file-connect-failed`=1、**`已自动进入 BLE 配网模式`=1**、
  **`自动进入 BLE 配网模式失败`=1**、**`无需按键`=1**、`长按 SET 键`=6、`SD 卡凭据未能入网`=3、
  `从凭据文件读取 Wi-Fi`=1、`未找到 Wi-Fi 凭据文件`=1、`BLE 配网通道**待机**`=1、`本机型未启用 SD 卡`=**0**（`#if` 命中）；
  阶段标签：`ble-ready（NimBLE 已起栈；入网前；gate=y 且文件通道未回落时未广播）`=**1**、旧串`ble-ready（…gate=y 时未广播、入网前）`=**0**。
- 产物 `sdkconfig`：`:621 WIFI_FILE=y`、`:624 ENABLE_BLE_PROV=y`、`:629 BLE_PROV_KEY_GATE=y`、`:636 ENABLE_SDCARD=y`、
  `CLOUD_HOST="192.168.110.208"`、`CLOUD_PORT=18830`、`TRANSPORT_TLS=y`、`TLS_INSECURE=y`、`DIAG_PROV_TRACE=y`、
  `MBEDTLS_EXTERNAL_MEM_ALLOC=y`、`ONEYE_DEV_CREDS_REQUIRED=y`；`mqtt.oneye.me` 出现 **0** 次。
- **量产链档（§3.2 证据件）**：`…/build-alpha-prod-sd-evidence/korvo2_oneye.bin` = 1372416 B
  `6a33b511216229e42ba06e3a5d7c5a419942a1b90e02261e6907c126fd693663`（`IDF_EXIT=0`、余量 67%）。
- **`managed_components` 前后一致（BEFORE == AFTER）**：`files=2362`、
  `list_sha=80ae13a8e2f4d9b575d5550163415301f0c6c17f6db6044219fcdf57f2418c7a`、
  `files_sha=f8beed4a536e6dbdd4a6ef2d7b3b0d115c81760e7cb0a97412dc35f4251c4593`（与上一批同值）。

**烧写命令（⛔ 本批未执行，由 lead 烧；四偏移、无 erase、不写 `0x510000`）**：
```
python -m esptool --chip esp32s3 -b 460800 --before default_reset --after hard_reset write_flash \
  --flash_mode dio --flash_size detect --flash_freq 80m \
  0x0      embedded/esp-adf/examples/oneye/korvo2_oneye/build-alpha-sd-fallback/bootloader/bootloader.bin \
  0x8000   embedded/esp-adf/examples/oneye/korvo2_oneye/build-alpha-sd-fallback/partition_table/partition-table.bin \
  0x10000  embedded/esp-adf/examples/oneye/korvo2_oneye/build-alpha-sd-fallback/korvo2_oneye.bin \
  0x410000 embedded/esp-adf/examples/oneye/korvo2_oneye/build-alpha-sd-fallback/srmodels/srmodels.bin
```
（⛔ 无 `erase-flash`；⛔ 不写 `creds` 分区 `0x510000` —— 设备身份/凭据保留。台面档云仍指本机栈 `192.168.110.208:18830`，
烧写后仍需 `node tools/bench-mqtt-fwd.js` 端口转发。）

---

## ⑤ 门禁读数（本批实跑）

| 门 | 命令 | 读数 |
| --- | --- | --- |
| SDK 宿主单测（基线 **28 组 / 320 用例 / 0 失败**） | `components/oneye-dev-sdk/build-host-scanfix/bin/oneye_dev_tests` | **28 组 / 320 用例 / 失败 0 次、失败组 0**；`TESTS_EXIT=0`（终态复跑断言 **7316** 次；同批内两次读数为 7303 / 7316，上一批为 7311 —— 组/用例/失败三个量稳定，**断言计数含负载相关的等待循环** ⇒ 只作读数登记） |
| ABI（基线 `oneye_dev_ble` **23** / `oneye_dev_link` **34**） | `bash tools/abi-check.sh lib/x86_64-linux-gnu-gcc-13.3.0`（cwd = SDK） | **OK**：ble **23**、link **34**（+ base 56 / mpp 16 / event 10 / log 15）；6 库、内嵌件符号零外泄；`ABI_EXIT=0` |
| **SDK 新判据 B2**（本批新增） | `python3 tools/check-net-thread-flash.py --sdk <sdk>` | **PASS**：35 个 .c / 845 函数定义 / 3 条根 / 闭包 260 个函数 / flash 写 **0 处**；自带负向对照 2/2 报红；`B2_EXIT=0` |
| 应用门禁 | `python3 tools/check-prov-convergence.py` | **正向断言 A1–A18 PASS**；夹具实跑 PASS；**负向对照 30 条全 PASS / 0 FAIL**；`GATE_EXIT=0` |
| defaults 门禁（SD 档） | `python3 tools/check-sdkconfig-defaults.py --sdkconfig build-alpha-sd-fallback/sdkconfig --require sdkconfig.defaults.bench-local --require sdkconfig.defaults.bench-sdcard` | **PASS**（`GATE_BENCH_EXIT=0`）：`bench-local：6 项全部存在且已生效`、**`bench-sdcard：4 项全部存在且已生效`**；同次 `sdkconfig.defaults`(33)/`.ble`(21)/`.ble-key`(2)/`.diag`(3)/`.production`(11) 全 OK；`[SKIP] sdkconfig.defaults.esp32s3`（既有排除项，非静默） |
| defaults 门禁（量产链档，§3.2） | `… --sdkconfig build-alpha-prod-sd-evidence/sdkconfig --require sdkconfig.defaults.production` | `production：11 项全部存在且已生效` **[OK]**；同次 **13 项 FAIL 全部归属 `sdkconfig.defaults.ble`**（该链刻意不含 BLE ⇒ NimBLE 符号不存在，非 `production` 缺陷） |
| cloud_start PSRAM 回归门禁 | `python3 tools/check-cloud-start-psram-retry.py` | **PASS**：`A17a–A17e` 静态栈**一律内部 RAM**（A17a 仍断言"内部 RAM"，旧 PSRAM 串不得出现）＋ `A17f–A17j` 失败可重试；负向对照全捕获；`PSRAM_EXIT=0` |

---

## ⑥ `git status` ＋ "未 commit"

`embedded/esp-adf`（分支 `oneye_s3_korvo`，HEAD **`55ebdeec`**，`2026-10-12 17:23:29 +0800`）：

- **本批（D2/B1）改动（已跟踪文件）**：`examples/oneye/korvo2_oneye/main/korvo2_oneye_main.c`、
  `main/wifi_prov.h`、`main/Kconfig.projbuild`、`sdkconfig.defaults.production`、`README.md`、
  `tools/check-prov-convergence.py`；`components/oneye-dev-sdk`（嵌套仓：`tools/check-net-thread-flash.py` 新增、
  `tools/build-linux.sh`、`README.md`）。
- **本批新增（未跟踪）**：`examples/oneye/korvo2_oneye/ALPHA-SD-FALLBACK.md`（本报告）、
  `examples/oneye/korvo2_oneye/build-alpha-sd-fallback/`（构建目录 + 5 个脚本）、
  `examples/oneye/korvo2_oneye/build-alpha-prod-sd-evidence/`（量产链档证据）。
- ⚠️ **并行批次改动（⛔ 非本批）**：`main/wifi_prov.c`（A17 扫描修复，指纹与上一批**同值**）、
  `tools/check-sdkconfig-defaults.py`（`esp32s3` 预设排除项）、`examples/oneye/devkitc_oneye/` 整片改动
  （另一例程的另一批）—— 本批**未**改它们；`main/wifi_prov_policy.h` 指纹与上一批**逐值相同**（⇒ 唯一判据未动）。
- 顶层仓（`rainmaker-oneye`）：`docs/adr/0017-多通道配网与入网后收敛点.md` 已就地订正＋新增 2026-10-13 决策记录。
- ⛔ **未 commit、未 push、未烧写**（⛔ 也未 `git add`）。

---

## ⑦ 未做 / 需裁定（≤3）

1. **真机时序未验证**（⛔ 本批不烧写）：判据要求的现场 = 卡上放一份**错误/过期**凭据 ⇒ 复位后串口应见
   `SD 卡凭据未能入网（来源 …）⇒ 释放采信槽` → `SD 卡凭据未能入网 ⇒ **已自动进入 BLE 配网模式**（等价长按 SET，**无需再按键**）`
   → `无需按键` → （App 侧）广播可见并可配网。此路径需 lead 烧 `build-alpha-sd-fallback/` 四区后逐字确认。
2. **"无 BLE 通道的量产链"上 D2 无落点**（§3.2 边界）：该链 BLE 关闭 ⇒ 回落分支被编译掉。
   若产品要求"无 BLE 机型也要有自动兜底"，那属于**另一条通道决策**（需裁定），本批⛔ 未做。
3. **`--require sdkconfig.defaults.ble` 在量产链档上必然 FAIL**（§3.2/§5 已归因）：若希望"量产链档也能一键跑全量 defaults 门禁"，
   需要门禁支持"按档声明参与的文件集合"（当前是靠 `-DSDKCONFIG_DEFAULTS` 链决定）——属门禁增强，待裁定。
