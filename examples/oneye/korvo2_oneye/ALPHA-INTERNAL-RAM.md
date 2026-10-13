# ALPHA-INTERNAL-RAM —— `cloud_start` 任务栈改回**内部 RAM**（阻塞级回归修复）

> 批次：2026-10-13（设备侧）。⛔ 未 commit / ⛔ 未 push / ⛔ 未烧写（lead 烧）。
> 构建在 WSL：`source ~/esp/esp-idf-5.5.5/export.sh`、`ADF_PATH=/mnt/e/workspace/rainmaker-oneye/embedded/esp-adf`。

---

## ① 根因（真机证据 + addr2line 解栈，**不是推断**）

真机串口（`E:\workspace\_oneye-logs\serial-sdcard.txt`，15:27:30 与 15:32:31 两轮同形）：
```
assert failed: spi_flash_disable_interrupts_caches_and_other_cpu
              cache_utils.c:127 (esp_task_stack_is_sane_cache_disabled())
Backtrace: 0x40383a0d:0x3c1ed570 0x403839d5:0x3c1ed590 … 0x4200deed:0x3c1edb80
```
⚠️ backtrace 里的 SP 形如 **`0x3c1e…`** = **PSRAM 区间**（`cloud_start` 任务的栈当时落在 PSRAM）。

对**当时那份件** `build-alpha-sdcard/korvo2_oneye.elf` 逐地址 `addr2line -f -C`：

| 帧 | 符号 | file:line |
| --- | --- | --- |
| 3 | `__assert_func` | `components/newlib/src/assert.c:81` |
| 4 | `spi_flash_disable_interrupts_caches_and_other_cpu` | `components/spi_flash/cache_utils.c:135`（assert 在 `:127`） |
| 5 | `cache_disable` | `components/spi_flash/spi_flash_os_func_app.c:75` |
| 6 | `spi1_start` | `components/spi_flash/spi_flash_os_func_app.c:133` |
| 7 | `spiflash_start_core` | `components/spi_flash/esp_flash_api.c:238` |
| 8 | `spiflash_start_default` | `components/spi_flash/esp_flash_api.c:249` |
| 9 | `esp_flash_read` | `components/spi_flash/esp_flash_api.c:981` |
| 10 | `esp_partition_read` | `components/esp_partition/partition_target.c:38` |
| 11 | `oneye_creds_plat_read` | `components/oneye-dev-sdk/src/oneye_creds_plat_idf.c:78` |
| 12 | `oneye_dev_creds_load` | `components/oneye-dev-sdk/src/oneye_dev_creds.c:551` |
| 13 | `oneye_start` | `examples/oneye/korvo2_oneye/main/korvo2_oneye_main.c:1408`（本批后行号 `:1435`） |
| 14 | `cloud_start_task` | 同文件 `:840` |

⇒ **`cloud_start` 任务在 `oneye_start()` 里读 `creds` 分区**（`:1408` → `oneye_dev_creds_load` → `esp_partition_read`），
flash 驱动进"关 cache"窗口，命中 IDF 的栈位置 assert。

IDF 侧逐字依据：

- `components/spi_flash/cache_utils.c:127`
  `assert(esp_task_stack_is_sane_cache_disabled());`
- `components/spi_flash/cache_utils.c:56-59`（该 assert 的实现）
  ```
  static inline bool esp_task_stack_is_sane_cache_disabled(void)
  {
      const void *sp = (const void *)esp_cpu_get_sp();
      return esp_ptr_in_dram(sp)
  #if CONFIG_ESP_SYSTEM_ALLOW_RTC_FAST_MEM_AS_HEAP
             || esp_ptr_in_rtc_dram_fast(sp)
  #endif
             ;
  }
  ```
  ⇒ **关 cache 期间正在运行的这条任务，其栈必须在内部 RAM**（`esp_ptr_in_dram`）。
- `components/esp_partition/partition_target.c:38`（非加密分区的读，**走 `esp_flash_read`**）
  `return esp_flash_read(partition->flash_chip, dst, partition->address + src_offset, size);`
- `components/spi_flash/spi_flash_os_func_app.c:133`（`spi1_start`）
  `cache_disable(NULL);`（在 `!CONFIG_SPI_FLASH_SHARE_SPI1_BUS && !SPI_FLASH_CACHE_NO_DISABLE` 分支）
- `components/freertos/heap_idf.c:43`
  `#define portFREERTOS_HEAP_CAPS    ( MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT )`
  ⇒ `xTaskCreate()` 的栈（走 `pvPortMalloc`）**只从内部 RAM 取** —— 这就是"改动前能跑"的原因。
- ⚠️ `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y` **只放行**外部栈（令 `xPortcheckValidStackMem`
  不再要求 `esp_ptr_internal`），⛔ **不豁免**上面这条 assert。产物实测该符号确为 y
  （`build-alpha-sdcard-internal/sdkconfig:3211`；`CONFIG_SPIRAM_USE_MALLOC=y` 在 `:1787`）。

对照：改动前的 `build-alpha-tlsram` 在同一块板上 12:51 能正常 `link up … node=KORVO2-0003`
⇒ 该改动是**回归点**（唯一差别 = 栈的落点）。

---

## ② 修复（旧 → 新）

文件：`examples/oneye/korvo2_oneye/main/korvo2_oneye_main.c`（`git diff --stat` = **1 file changed, 221 insertions(+), 7 deletions(-)**）

**旧**（本批作废；旧值逐字留痕在源码注释里，⛔ 不抹）：
```c
s_cloud_start_stack = (StackType_t *)heap_caps_calloc(1u, CLOUD_START_STACK_BYTES,
                                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);   /* ① PSRAM 优先 */
if (s_cloud_start_stack == NULL) {
    s_cloud_start_stack = (StackType_t *)heap_caps_calloc(1u, CLOUD_START_STACK_BYTES,
                                                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);  /* ② 回落 */
}
```

**新**（只有一路；⛔ 不再出现 `MALLOC_CAP_SPIRAM`）：
```c
s_cloud_start_stack = (StackType_t *)heap_caps_calloc(1u, CLOUD_START_STACK_BYTES,
                                                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
```
日志串：**旧**`上云任务栈：已从 **PSRAM** 取到 %u B（旧实现走内部 RAM，见本段注释）`
→ **新**`上云任务栈：已从 内部 RAM 取到 %u B（本任务要读 creds 分区，栈落 PSRAM 会在 flash 关 cache 时命中 assert，见本段注释）`。

**逐字未变的既有语义**：栈量 `CLOUD_START_STACK_BYTES 6144u`、优先级 `CLOUD_START_TASK_PRIO 4`、
`pvParameters=NULL`、`xTaskCreateStaticPinnedToCore` 静态创建、`xTaskCreate` 旧行为回落、
唯一创建点（`on_wifi_ready()`）、失败可就地重试循环（`ulTaskNotifyTake` / `xTaskNotifyGive` / `s_cloud_start_sdk_up` / 重试前置 `deinit`）。

### 内部 RAM 余量读数（6144 是否有压力）—— **无压力**，故不给"缩小栈"选项

| 读数来源 | 时刻 | `free_internal` | `largest_internal_block` |
| --- | --- | --- | --- |
| `serial-sdcard.txt`（本轮失败件，`[res] phase=pre-prov`） | 外设就绪、配网与 NimBLE 之前 | **158839 B** | **63488 B** |
| `serial-once.txt`（2026-10-12，**当时栈就在内部 RAM**，`[res] phase=cloud-start（入网后）`） | 入网 + SDK 起栈之后 | **10979 B** | **7680 B** |

- 判据口径：`heap_caps_calloc(1u, 6144, MALLOC_CAP_INTERNAL)` 需要**连续** 6144 B。
  上表第 2 行是"6144 B 栈**已经**从内部 RAM 拿在手上、且 SDK 也已起来"之后的最大连续块 = **7680 B > 6144 B**；
  且改动前的 `build-alpha-tlsram` 真机是**跑通**的（`link up`），那次栈本就是内部 RAM。
- ⇒ **6144 B 内部 RAM 可满足**；未触发任何"缩小栈 / 绕开 flash 访问"的替代方案（故无此选项需裁定）。

---

## ③ SDK 网络线程栈（PSRAM）审计结论 —— **当前不会命中同一 assert（判定：可暂不动）**

对象：`components/oneye-dev-sdk/src/internal/oneye_osal_idf.c`（前批 `709ab45`：`heap_caps_calloc(MALLOC_CAP_SPIRAM)` 优先 → 回落内部 RAM → `xTaskCreateStaticPinnedToCore`）。
经 `oneye_osal_thread_start()` 创建的**生产线程恰好 3 条**：

| 线程名 | 入口 | 创建点 |
| --- | --- | --- |
| `oneye_net` | `bint_net_thread` | `src/internal/oneye_internal.c:1764` |
| `oneye_link` | `link_wan_thread` | `src/oneye_dev_link.c:1403` |
| `oneye_prov` | `link_prov_worker` | `src/oneye_dev_link.c:447` |

**全 SDK `src/` 里"会进 flash 关 cache 窗口"的调用点只有 2 类（穷举 grep 证据）**：
`esp_partition_read|esp_partition_write|esp_partition_erase|nvs_set_*|nvs_commit|nvs_open|esp_ota_*|f_open|f_write|esp_vfs*` 命中：
- `oneye_creds_plat_idf.c:78` `esp_partition_read`（**唯一**分区读）← `oneye_dev_creds_load`（`oneye_dev_creds.c:551`）
  ← 调用者只有：本工程 `main/korvo2_oneye_main.c:1435`（**cloud_start 任务**）、
  `main/adv_identity.c:51`（← `wifi_prov_ble.c:601` ← `wifi_prov_ble_start()`，**启动/boot 线程**）、SDK 工具与单测。
  ⇒ **没有任何一条 `oneye_osal` 线程**调它。
- `oneye_ble_bindid_nvs.c:109/111/164` `nvs_set_blob` + `nvs_commit`（**唯一** NVS 写）←
  `oneye_ble_bindid_plat_write/erase` ← `oneye_ble_bindid_write/reset` ← `oneye_dev_ble.c:968`
  （在 `ble_handle_plain_impl` 内）← `ble_on_rx` ← `gatt_rx_access`（`oneye_ble_plat_nimble.c:174`，
  注册为 GATT `.access_cb`，`:273`）⇒ 运行在 **`nimble_host` 任务**上，而该任务是
  IDF `nimble_port_freertos.c:43` 的 `xTaskCreatePinnedToCore(host_task, "nimble_host", …)`
  ⇒ `pvPortMalloc` ⇒ `portFREERTOS_HEAP_CAPS = INTERNAL|8BIT` ⇒ **内部 RAM 栈**。
- **无 OTA、无文件系统**：`esp_ota_*` / `f_open` / `f_write` / `esp_vfs*` 在 SDK `src/` 命中 **0** 处。

三条线程各自的结论：
- `oneye_net` / `oneye_link`：socket + mbedTLS + MQTT 收发；证书/私钥/CA 是
  `oneye_dev_creds_load()` 在 **cloud_start 任务**上读出来、以**内存缓冲**传进配置的
  ⇒ 运行期不再碰分区；**不触碰 flash** ⇒ 安全。
- `oneye_prov`（`link_prov_worker`）：`oneye_link_prov_execute()` 里同步执行的是
  **app 注册的 provider**（`wifi_prov_ble.c:145/186/244`）：
  · `ble_prov_scan` → `wifi_prov_scan_json()` → `esp_wifi_scan_start(&sc, true)`：**不碰 flash**；
  · `ble_prov_connect` → `wifi_prov_submit_credentials()` → `wifi_prov_connect()`：`esp_wifi_set_config()`；
  · `ble_prov_reset` → `wifi_prov_forget()`：本工程注释逐字「**不清 NVS/flash**…只断开连接并清掉 RAM 里的 STA 配置」。
  ⚠️ **唯一不能从源码证明的一项**：`esp_wifi_set_config()` 在 IDF 5.5.5 里位于**预编译 blob**
  （`components/esp_wifi/lib/esp32s3/libnet80211.a`，Kconfig/源里都没有实现）
  ⇒ 其内部是否在**调用者线程**落 NVS 无法用源码判定。本工程既有**二进制级取证**注释
  （`main/wifi_prov.c:570-575`）逐字给出：它「只把一条 ioctl 投给 Wi-Fi 任务」，
  真正写 NVS 的 `wifi_nvs_set` 在 `wifi_set_config_process` / `wifi_station_set_config_local_2` 内
  ⇒ 落在 **Wi-Fi 任务**（内部 RAM 栈）上 ⇒ 按此口径**安全**。
  ⇒ **残余风险（如实登记）**：该结论依赖上述二进制取证，不是本批新证；若日后发现
  `esp_wifi_set_config` 确实在调用线程写 NVS，则 `oneye_prov` 会命中同一 assert ⇒ **需一并改**。

**建议的机械判据（未落码，待裁定）**：在 SDK 门禁里断言
「`oneye_osal_thread_start` 的 3 个入口函数（及其同步可达的 app provider 回调）函数体内不得出现
`nvs_commit|nvs_set_blob|nvs_erase_key|esp_partition_write|esp_partition_erase|esp_ota_`」，
并加"把 `nvs_commit` 放回 ⇒ 必须报红"的负向对照。

⛔ **本批未动 SDK 一个字节**（`git -C components/oneye-dev-sdk status --porcelain` 只有本轮之前既存的
`M docs/INTEGRATION.md` / `M tools/abi-check.sh`）。

---

## ④ 门禁同步（`tools/check-cloud-start-psram-retry.py`）

- **A17a 语义反转**：旧断言逐字留痕（模块 docstring 与 `check()` 内注释各一份，⛔ 不抹）——
  旧值 = 「`A17a 取栈顺序：PSRAM 申请在内部 RAM 申请**之前**（先 PSRAM、失败回落）」，
  实现 = `PSRAM_ALLOC 存在 ∧ INT_ALLOC 存在 ∧ PSRAM 偏移 < 内部 RAM 偏移`。
  新断言 = **必须内部 RAM**：① `INT_ALLOC`（按 `CLOUD_START_STACK_BYTES` 取 `MALLOC_CAP_INTERNAL`）必须存在；
  ② **全文件不得**出现"按 `CLOUD_START_STACK_BYTES` 从 PSRAM 取栈"的申请（回归点）；
  ③ `cloud_start_stack_alloc()` 函数体内不得出现 `MALLOC_CAP_SPIRAM`；
  ④ 代码里必须有新日志串「已从 内部 RAM 取到」、且**不得**有旧日志串「已从 **PSRAM** 取到」。
  ⚠️ 为此新增 `strip_comments_only()`：`strip_comments_and_strings()` 会连字符串字面量一起抹，
  判不了"日志串还在不在"；而旧串按留痕纪律写在**注释**里 ⇒ 两个函数各管一半，缺一不可。
- **负向对照（13 条全 PASS / 0 FAIL）**，其中 A17a 新增 3 条：
  ① 把内部 RAM 取栈**造回 PSRAM**（只换 caps，重演真机那一步）⇒ 必须红 ✅被捕获；
  ② 把**旧的两路形状**（PSRAM 优先 ＋ 失败回落内部）**逐字造回去** ⇒ 必须红 ✅被捕获；
  ③ 把旧日志串「已从 **PSRAM** 取到」写回代码 ⇒ 必须红 ✅被捕获。
- 其余 A17b–A17j 与 `check-prov-convergence.py` 的 A1–A17 全部维持真跑 **PASS**。

---

## ⑤ 两档四 bin ＋ `IDF_EXIT` ＋ 烧写命令（⛔ 未执行）

口径：`-DSDKCONFIG=<构建目录>/sdkconfig`（⛔ 不落工程根）＋ `-DONEYE_FW_CERT_DIR=<空目录>`
＋ `-DONEYE_SDK_LIB_OUT_DIR=/nonexistent-oneye-lib-out`（源码回落）＋ 显式 defaults 链；
产物**落仓库内**。`IDF_EXIT` 两档均 **0**。`managed_components` 前后一致
（`files=2362`、list sha `80ae13a8…`、files sha `f8beed4a…`，BEFORE == AFTER）。

### 档 1：SD 卡台面档 ⇒ `build-alpha-sdcard-internal/`
defaults 链：`sdkconfig.defaults;…;sdkconfig.defaults.diag;sdkconfig.defaults.bench-local;sdkconfig.defaults.bench-sdcard`

| 偏移（⛔ 无 erase、⛔ 不写 `0x510000`） | 文件（仓库内路径） | 字节 | sha256 |
| --- | --- | --- | --- |
| `0x0` | `embedded/esp-adf/examples/oneye/korvo2_oneye/build-alpha-sdcard-internal/bootloader/bootloader.bin` | 20832 | `44030031484ce3c12141a844eb8a583884f011c16f0ce4837bd7793265fe804d` |
| `0x8000` | `…/build-alpha-sdcard-internal/partition_table/partition-table.bin` | 3072 | `dbb160ed9a93d3b036b5344ab12bcc88833d5d17df03c36c49a8f1c4fefac1bd` |
| `0x10000` | `…/build-alpha-sdcard-internal/korvo2_oneye.bin` | **1668128**（`0x197420`） | `9ff59de4197e7a8549008d771abfcfbf50e392e10ba3d8f5c79d06fe20c13e43` |
| `0x410000` | `…/build-alpha-sdcard-internal/srmodels/srmodels.bin` | 337952 | `e93f68f637e71d71c3a0a271f1b7709ea0f58d4fe3315e031c8435bf865e51d6` |

余量：`factory 4 MB` ⇒ `0x268be0 bytes (60%) free`。

### 档 2：非 SD 台面档（文件通道关）⇒ `build-alpha-scanfix-internal/`
defaults 链：同档 1 但**末位不带** `sdkconfig.defaults.bench-sdcard`

| 偏移（⛔ 无 erase、⛔ 不写 `0x510000`） | 文件（仓库内路径） | 字节 | sha256 |
| --- | --- | --- | --- |
| `0x0` | `embedded/esp-adf/examples/oneye/korvo2_oneye/build-alpha-scanfix-internal/bootloader/bootloader.bin` | 20832 | `268479809b251da88f89e48cb7ccaf289c59ead38c9ee3871f011af8cc12574e` |
| `0x8000` | `…/build-alpha-scanfix-internal/partition_table/partition-table.bin` | 3072 | `dbb160ed9a93d3b036b5344ab12bcc88833d5d17df03c36c49a8f1c4fefac1bd` |
| `0x10000` | `…/build-alpha-scanfix-internal/korvo2_oneye.bin` | **1666880**（`0x196f40`） | `7ffbda918795e9a35bf0b9511239d4694310e88d0e80c21c1d71c20ac5d8995e` |
| `0x410000` | `…/build-alpha-scanfix-internal/srmodels/srmodels.bin` | 337952 | `e93f68f637e71d71c3a0a271f1b7709ea0f58d4fe3315e031c8435bf865e51d6` |

余量：`0x2690c0 bytes (60%) free`。

**配置自证**（`CMakeCache.txt` 逐字）：
`SDKCONFIG:UNINITIALIZED=…/build-alpha-sdcard-internal/sdkconfig`；
`ONEYE_FW_CERT_DIR:PATH=…/.scratch/certs-empty`；`ONEYE_SDK_LIB_OUT_DIR:PATH=/nonexistent-oneye-lib-out`；
构建日志：`…/liboneye_dev_base.a；本次使用参考实现源码构建`、`未提供证书材料（…certs-empty）⇒ 不嵌入证书（匿名/明文形态）`。
工程根 **未**产生 `sdkconfig` / `sdkconfig.old`（`ls` 实测 No such file）。

**产物 sdkconfig 关键行**：

| 符号 | SD 档 | 非 SD 档 |
| --- | --- | --- |
| `CONFIG_ONEYE_FW_CLOUD_HOST="192.168.110.208"` | `:607` ✅ | `:607` ✅ |
| `CONFIG_ONEYE_FW_CLOUD_PORT=18830` | `:608` ✅ | `:608` ✅ |
| `CONFIG_ONEYE_FW_TLS_INSECURE=y` | `:615` ✅ | `:615` ✅ |
| `CONFIG_ONEYE_FW_TRANSPORT_TLS=y` | `:613` | `:613` |
| `CONFIG_ONEYE_FW_ENABLE_WIFI_FILE` | `:621` **`=y`** ✅ | `:621` **`is not set`** ✅ |
| `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y` | `:2418` | `:2418` |
| `CONFIG_ONEYE_DEV_CREDS_REQUIRED=y` | `:2893` | `:2893` |
| `mqtt.oneye.me` 出现次数 | **0** | **0** |

**ELF / bin 日志串 occurrence（正负对照）**：

| 判据 | SD 档 ELF | SD 档 bin | 非 SD 档 ELF | 非 SD 档 bin |
| --- | --- | --- | --- | --- |
| **新串** `上云任务栈：已从 内部 RAM 取到` | **1** | **1** | **1** | **1** |
| **旧串** `上云任务栈：已从 **PSRAM** 取到` | **0** | **0** | **0** | **0** |
| 裸旧串 `已从 **PSRAM** 取到` | **0** | **0** | **0** | **0** |
| `上云任务栈` 总计数 | 2 | — | 2 | — |

（第 2 条 = "旧串必须消失"的**负向对照**：旧值只留在**注释**里，⛔ 不进产物。第 4 行的 2 = 新串 ＋ 回落提示串。）

**烧写命令（⛔ 本批未执行；由 lead 烧；四偏移、无 erase、不写 `0x510000`）**
```
# 档 1：SD 卡台面档
python -m esptool --chip esp32s3 -b 460800 --before default_reset --after hard_reset write_flash \
  --flash_mode dio --flash_size detect --flash_freq 80m \
  0x0      embedded/esp-adf/examples/oneye/korvo2_oneye/build-alpha-sdcard-internal/bootloader/bootloader.bin \
  0x8000   embedded/esp-adf/examples/oneye/korvo2_oneye/build-alpha-sdcard-internal/partition_table/partition-table.bin \
  0x10000  embedded/esp-adf/examples/oneye/korvo2_oneye/build-alpha-sdcard-internal/korvo2_oneye.bin \
  0x410000 embedded/esp-adf/examples/oneye/korvo2_oneye/build-alpha-sdcard-internal/srmodels/srmodels.bin

# 档 2：非 SD 台面档
python -m esptool --chip esp32s3 -b 460800 --before default_reset --after hard_reset write_flash \
  --flash_mode dio --flash_size detect --flash_freq 80m \
  0x0      embedded/esp-adf/examples/oneye/korvo2_oneye/build-alpha-scanfix-internal/bootloader/bootloader.bin \
  0x8000   embedded/esp-adf/examples/oneye/korvo2_oneye/build-alpha-scanfix-internal/partition_table/partition-table.bin \
  0x10000  embedded/esp-adf/examples/oneye/korvo2_oneye/build-alpha-scanfix-internal/korvo2_oneye.bin \
  0x410000 embedded/esp-adf/examples/oneye/korvo2_oneye/build-alpha-scanfix-internal/srmodels/srmodels.bin
```
（⛔ 无 `erase-flash`；⛔ 不写 `creds` 分区 `0x510000` ⇒ 设备身份保留。台面云仍指本机栈，需 `node tools/bench-mqtt-fwd.js` 端口转发。）

---

## ⑥ 门禁读数（本批实跑）

| 门 | 命令 | 读数 |
| --- | --- | --- |
| 本批结构门禁 | `python3 tools/check-cloud-start-psram-retry.py` | **正向 A17a–A17j PASS**；**负向对照 13/13 全捕获、0 FAIL**；`GATE_A17_EXIT=0` |
| 应用收敛门禁 | `python3 tools/check-prov-convergence.py` | **A1–A17 PASS**（含夹具实跑与 26 条负向对照）；`GATE_PROV_EXIT=0` |
| defaults（SD 档） | `python3 tools/check-sdkconfig-defaults.py --sdkconfig build-alpha-sdcard-internal/sdkconfig --require sdkconfig.defaults.bench-local --require sdkconfig.defaults.bench-sdcard` | **PASS**：bench-local 6 项 / bench-sdcard 4 项全生效；`GATE_BENCH_SD_EXIT=0` |
| defaults（非 SD 档） | 同上但 `--sdkconfig build-alpha-scanfix-internal/sdkconfig --require sdkconfig.defaults.bench-local` | **PASS**；`GATE_BENCH_NOSD_EXIT=0` |
| SDK 宿主单测（基线 **28 组 / 320 用例 / 0 失败**） | `components/oneye-dev-sdk/build-host-scanfix/bin/oneye_dev_tests` | **28 组 / 320 用例 / 断言 7298 次 / 失败 0 次 / 失败组 0**；`TESTS_EXIT=0` |
| ABI（基线 `oneye_dev_ble` **23** / `oneye_dev_link` **34**） | `bash tools/abi-check.sh lib/x86_64-linux-gnu-gcc-13.3.0`（cwd = SDK） | **OK**：ble **23**、link **34**（+base 56 / mpp 16 / event 10 / log 15）；6 库、内嵌件符号零外泄；`ABI_EXIT=0` |

---

## ⑦ `git status` ＋ "未 commit"

- 总控仓（分支 `esps3_korvo2_20260915`，HEAD `586a665`）：本批相关的**唯一**条目 =
  ` M embedded/esp-adf`（submodule**内容**已改、**指针未动** —— ⛔ 未 commit ⇒ 指针自然不会动）。
  其余 `M mobile/**`、`M backend`、`M .memory/...` 均为**并行批次/本轮之前既存**，⛔ 非本批（本批 ⛔ 未碰 `mobile/**`、`cloud/**`、`backend/**`、`contracts/**`）。
  ⚠️ 全量 `git status` 会因 `esp-repo/esp-idf` 下嵌套 submodule 的 gitlink 损坏而卡死
  （`fatal: not a git repository: …/lib_esp32c6/esp32c6-bt-lib`）⇒ 取证时用 `--ignore-submodules=dirty` 限定作用域。
- `embedded/esp-adf`（分支 `oneye_s3_korvo`，HEAD `55ebdeec`）：
  - 本批**受管改动恰好 1 个**：`examples/oneye/korvo2_oneye/main/korvo2_oneye_main.c`（**221 insertions(+) / 7 deletions(-)**）；
  - 本批**新增未跟踪**：`examples/oneye/korvo2_oneye/tools/check-cloud-start-psram-retry.py`（A17 门禁）、
    `examples/oneye/korvo2_oneye/build-alpha-sdcard-internal/`、`…/build-alpha-scanfix-internal/`、本报告。
  - ⚠️ `git status --porcelain` 另有 **2041** 条 ` M` —— 是**既有** CRLF/文件模式噪声
    （抽查 `.gitignore` = 72 insertions/72 deletions，纯换行重规范化），**非本批产生**。
- `components/oneye-dev-sdk`（分支 `main`，HEAD `709ab45`）：`M docs/INTEGRATION.md`、`M tools/abi-check.sh`
  —— **均为本轮之前既存**；本批**未改 SDK 任何文件**（含 `oneye_osal_idf.c` 一字未动）。
- 指纹：`main/korvo2_oneye_main.c = 728fb328a45c681d6af6953b7e0e58ebb34d4045a2b76c779d1364459c2acb04`、
  `tools/check-cloud-start-psram-retry.py = 2c3f67da02651b82d974c3df66c50e0fb1cabeb996bae2189fc1bcf11d683fc7`、
  `tools/check-prov-convergence.py = 5faeb9fe…`（未动）、`main/wifi_prov.c = 3ca08cc2…`（未动）、
  `main/wifi_prov.h = a5d74fc8…`（未动）、`main/wifi_prov_policy.h = ef98c177…`（未动）、
  `sdkconfig.defaults.bench-local = bff10145…`（未动）、`sdkconfig.defaults.bench-sdcard = e43ba4f2…`（未动）。
- 编码：两个改动文件 `BOM=False`、`CRLF=0`、LF；块注释开启/收尾相对 HEAD **各 +14**（本批新增注释全部成对）。
  ⚠️ 如实登记：`main/korvo2_oneye_main.c` 的裸 `/*` 比 `*/` 多 1 —— **HEAD 就已如此**（157/156），
  **不是本批引入**；该差异是词法扫描假阳性（文件 `IDF_EXIT=0` 编译通过），本批未处理。
- ⛔ **未 commit、未 push、未烧写**（⛔ 也未 `git add`）。
