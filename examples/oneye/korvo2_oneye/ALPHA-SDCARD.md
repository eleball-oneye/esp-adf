# ALPHA-SDCARD（2026-10-13）—— 台面固件**打开 SD 凭据文件通道**（`ONEYE_FW_ENABLE_WIFI_FILE=y`）

> 🔴 **后续批次订正（2026-10-13 · ALPHA-SD-FALLBACK）**：本报告的 §3.4「需再按一次 SET（或复位）」
> 边界**已修掉**（用户裁定「**D2.SD卡配网失败回落BLE 配网模式**」⇒ 自动进入 BLE 配网模式，**无需再按键**）；
> §3.5 的安全口径**已按用户裁定「B1.量产支持且凭据明文」订正**（文件通道 = **量产品类形态**；
> 明文密码后果**已由用户接受**；⛔ 不再写"仅台面"）。两处旧字均**逐字留痕**在本文件内。
> 交付件与门禁读数以 **[ALPHA-SD-FALLBACK.md](ALPHA-SD-FALLBACK.md)** 为准（构建目录 `build-alpha-sd-fallback/`）。

> 任务（用户逐字裁定，最高依据）：
> 「**打开 `ENABLE_WIFI_FILE` 的固件才是支持 SD 卡的，这种情况下才需要自动回连。其他情况都是靠按键重新触发配网**」
> ⇒ 本批交付一个**只加档、不改代码**的台面变体：卡根目录 `oneye-wifi.txt` 在位 ⇒ **插卡 + 复位即自动入网（自动回连）**；
> 卡不在位/解析失败 ⇒ **回落按键触发配网**。
> ⛔ 本批**不 commit / 不 push / 不烧写**（固件由 lead 烧）。
> ⛔ 未改 `mobile/`、`cloud/`、`backend/`、`contracts/`；⛔ 未改业务判据（只加档 + 构建）；
> ⛔ 未改 SDK / ADF 任何源码；⛔ 未改既有 7 份 defaults 一字。

---

## ① 新增档 / 覆盖方式（旧 → 新）＋ 机械证据

### 1.1 旧 → 新（叠加链上该符号的三次落点）

| # | 文件:行 | 内容（逐字） | 效果 |
| --- | --- | --- | --- |
| 旧① | `sdkconfig.defaults:101` | `CONFIG_ONEYE_FW_ENABLE_WIFI_FILE=y` | 机型能力位（有卡槽机型 y）——**默认意图是对的** |
| 旧② | `sdkconfig.defaults.ble:23` | `# CONFIG_ONEYE_FW_ENABLE_WIFI_FILE is not set` | **覆盖成 n**（该文件设计意图 = 无卡槽机型）；后写覆盖 ⇒ 旧产物实测 `is not set` |
| **新③** | **`sdkconfig.defaults.bench-sdcard:60`** | **`CONFIG_ONEYE_FW_ENABLE_WIFI_FILE=y`** | **本批唯一新增语义**：排在**最后一份** ⇒ 覆盖回 y |

**选用的覆盖方式 = 新增 `sdkconfig.defaults.bench-sdcard` 并排 `SDKCONFIG_DEFAULTS` 末位**
（而不是 `-DCONFIG_...`）：理由 —— ① 与工程既有"台面档"机制（`sdkconfig.defaults.bench-local`）同构，`tools/check-sdkconfig-defaults.py` 能**逐项机械核验**（`-DCONFIG_...` 是 CMake 层注入，该门禁扫不到）；② 本档不污染量产默认（⛔ 不在 `sdkconfig.defaults.production` 的引用链里）；③ 改动**只新增 1 个文件**，既有 7 份 defaults 一字未动。

**新档内容（4 项，其中 3 项是"显式重述既有值"以保证回落通道同产物可达）**：

| 符号 | 新档取值 | 语义 |
| --- | --- | --- |
| `CONFIG_ONEYE_FW_ENABLE_WIFI_FILE` | **y** | ★ 本档唯一新增语义：C3 文件通道（rank 0）启动即读卡 |
| `CONFIG_ONEYE_FW_ENABLE_BLE_PROV` | y | 回落通道 C1/C2（rank 1）：卡不在位/解析失败 ⇒ 落到按键配网 |
| `CONFIG_ONEYE_FW_BLE_PROV_KEY_GATE` | y | **逐字保持上一批台面件的既有行为**（开机不广播、长按 SET 进配网），⛔ 不因本档顺带改成"开机即广播" |
| `CONFIG_ONEYE_FW_ENABLE_SDCARD` | y | 卡槽机型（值同 `sdkconfig.defaults:106`，显式写出仅为可审计） |

### 1.2 ★ 机械证据：产物 sdkconfig 里 `=y`（旧 vs 新，同一行号直接对照）

`grep -nE '^CONFIG_ONEYE_FW_(ENABLE_WIFI_FILE|ENABLE_BLE_PROV|ENABLE_SDCARD|BLE_PROV_KEY_GATE)' <build>/sdkconfig`

| 符号 | **新**：`build-alpha-sdcard/sdkconfig` | 旧：`build-alpha-scanfix/sdkconfig`（上一批台面件） |
| --- | --- | --- |
| `CONFIG_ONEYE_FW_ENABLE_WIFI_FILE` | **`:621:` `CONFIG_ONEYE_FW_ENABLE_WIFI_FILE=y`** | `:621:` `# CONFIG_ONEYE_FW_ENABLE_WIFI_FILE is not set` |
| `CONFIG_ONEYE_FW_ENABLE_BLE_PROV` | `:624:` `=y` | `:624:` `=y` |
| `CONFIG_ONEYE_FW_BLE_PROV_KEY_GATE` | `:629:` `=y` | `:629:` `=y` |
| `CONFIG_ONEYE_FW_ENABLE_SDCARD` | `:636:` `=y` | `:636:` `=y` |

**第二条独立机械证据（编译期 `#if` 分支真的翻面了）** —— 对 `korvo2_oneye.bin` 逐子串计数：

| 子串（源码里的分支留痕） | 新产物 | 旧产物（scanfix） | 判定 |
| --- | --- | --- | --- |
| `本机型未启用 SD 卡`（`#else` 分支，`:1265`） | **0** | 1 | ⇒ 新产物**没有**走 else ⇒ `#if CONFIG_ONEYE_FW_ENABLE_WIFI_FILE` **命中** |
| `未找到 Wi-Fi 凭据文件`（`wifi_prov_load_file()` 内，`:516`） | 1 | 1 | 该函数体始终编入（调用点被 `#if` 门控） |
| `file-connect-failed`（`wifi_prov_boot()` 内 `#if` 段，`:1259`） | **1** | 1 | 同上（字符串常量在同一 TU） |

**第三条证据：CMake 缓存里两个 `-D` 都落对**（`build-alpha-sdcard/CMakeCache.txt`）：
```
SDKCONFIG:UNINITIALIZED=…/build-alpha-sdcard/sdkconfig
SDKCONFIG_DEFAULTS:UNINITIALIZED=sdkconfig.defaults;sdkconfig.defaults.esp32s3;sdkconfig.defaults.production;sdkconfig.defaults.ble;sdkconfig.defaults.ble-key;sdkconfig.defaults.diag;sdkconfig.defaults.bench-local;sdkconfig.defaults.bench-sdcard
```
⚠️ **本批第一轮构建踩到的坑（已修，如实登记）**：首轮**漏传 `-DSDKCONFIG=<build>/sdkconfig`** ⇒ IDF 把生成物写回**工程根 `sdkconfig`**，而 `kconfgen` 对已存在的根 `sdkconfig` **不按 defaults 重算** ⇒ 产物实际用的是**根目录那份过期 sdkconfig**（当时 `WIFI_FILE` 恰好还是 y，但 `CLOUD_HOST` 仍是真云 `mqtt.oneye.me`、`TLS_INSECURE` 未开）——即"看起来对、其实不是本档"。处置：`rm -f sdkconfig sdkconfig.old`（均为 gitignored 生成物；默认 sdkconfig 位在 `:/`）、清空构建目录后按**上一批口径**（含 `-DSDKCONFIG`）重建。第一轮的 bin 已被覆盖，**未作为交付物**。

### 1.3 工程根生成物清理（如实登记 + 影响面）

- 被删：`examples/oneye/korvo2_oneye/sdkconfig`（84464 B，14:28 的过期生成物）、`sdkconfig.old`（84412 B，Sep 23）。两者都是 **IDF 生成物**，`git status` 对二者**无输出**（= 忽略/未跟踪），删除**不影响任何受控文件**。
- 现在 `-DSDKCONFIG` 指向构建目录 ⇒ 工程根**不再产生** `sdkconfig` 生成物（`ls` 实测 `No such file`）。

---

## ② 四 bin（仓库内路径 / 大小 / sha256）＋ `IDF_EXIT` ＋ 烧写命令（⛔ 未执行）

**构建口径（与上一批逐字一致，唯一差别 = defaults 列表末位多一份 `sdkconfig.defaults.bench-sdcard`）**：
`5 份既有 defaults`（`sdkconfig.defaults` / `.esp32s3` / `.production` / `.ble` / `.ble-key`）＋ `sdkconfig.defaults.diag`
＋ 台面档 `sdkconfig.defaults.bench-local` ＋ **新增档 `sdkconfig.defaults.bench-sdcard`（末位）**
＋ `-DSDKCONFIG=<构建目录>/sdkconfig`（⛔ 不落工程根）＋ `-DONEYE_FW_CERT_DIR=<空目录>` ＋ `-DONEYE_SDK_LIB_OUT_DIR=/nonexistent-oneye-lib-out`（源码回落）
；`source ~/esp/esp-idf-5.5.5/export.sh`、`ADF_PATH=/mnt/e/workspace/rainmaker-oneye/embedded/esp-adf`；构建目录**在仓库内** `build-alpha-sdcard/`。

**`IDF_EXIT=0`**（两次都 0；交付结论取**第二次**，即带 `-DSDKCONFIG` 的那次）。
（旁证：第一轮 `[1588/1588]` 是"沿用旧配置"的增量；第二轮 `[1721/1721]` —— 改变了 `SDKCONFIG`/defaults 链 ⇒ **全量重配重编**，说明第二轮确实按新档重算过。）

| 偏移（⛔ 无 erase、⛔ 不写 `0x510000`） | 文件（Windows 仓库内路径） | 字节 | sha256 |
| --- | --- | --- | --- |
| `0x0` | `E:\workspace\rainmaker-oneye\embedded\esp-adf\examples\oneye\korvo2_oneye\build-alpha-sdcard\bootloader\bootloader.bin` | 20832 | `60f7bc4cf1bf60d5d943140b3542da41867a4fee265e2959753ba0a82219e432` |
| `0x8000` | `…\build-alpha-sdcard\partition_table\partition-table.bin` | 3072 | `dbb160ed9a93d3b036b5344ab12bcc88833d5d17df03c36c49a8f1c4fefac1bd` |
| `0x10000` | `…\build-alpha-sdcard\korvo2_oneye.bin` | **1668000**（`0x1973a0`） | `8db7a72b18ab61d5dbe84a950434750297050ed090615de97443deef62187c49` |
| `0x410000` | `…\build-alpha-sdcard\srmodels\srmodels.bin` | 337952 | `e93f68f637e71d71c3a0a271f1b7709ea0f58d4fe3315e031c8435bf865e51d6` |

- sha256 **Windows `Get-FileHash` 与 WSL `sha256sum` 两侧一致**（逐值比对）。
- 余量：`factory 4 MB` ⇒ 余 **2,526,304 B（`0x268c60`）= 60.23%**（`check_sizes.py` 自报 `0x268c60 bytes (60%) free`）。
- `partition-table.bin` / `srmodels.bin` 与上一批**逐字节相同**（未动分区表与模型）。
- **源码指纹（构建后复核；本批⛔未改任何源码）**：
  `main/wifi_prov.c=3ca08cc235ac3b0b12bab17482ed74ae4860125df32e47823343c96e072b41a4`（= scanfix 批同值）、
  `main/wifi_prov.h=a5d74fc8075cdfd66012ef60c41f84e7a46d1f94679594b8c960594aed391c33`、
  `main/korvo2_oneye_main.c=1b26072299db32cd9363aba76f2db503ee93ef8b54c45ab7c6f2e085a543cc6a`、
  `main/wifi_prov_policy.h=ef98c177a0420c87ac658f59da9a8fe5011f37fc954031ad72e195eb6e16a604`、
  `tools/check-prov-convergence.py=5faeb9fe497eafc6e3e90e6f61746ea96f57f7e2feb2d1c1f9bccbe4ef1d3911`、
  `sdkconfig.defaults.bench-local=bff1014518f8f907fe0da75d83478b1b7615569541af1ae149db45ca0026aae4`、
  **新增** `sdkconfig.defaults.bench-sdcard=e43ba4f269923007b90e85e310134b5a69e6106032ecde6412ac27a8471985fa`（6440 B、UTF-8 无 BOM、LF、无未闭合 `/*`）。
- ⇒ **scanfix 批（A17 扫描修复）＋ cloud_start PSRAM ＋ `on_wifi_ready` 可重试**都在本产物里（`wifi_prov.c` 指纹与 scanfix 批一致即为源级证据）。

**`managed_components` 前后一致（BEFORE == AFTER）**：
`files=2362`、`list_sha=80ae13a8e2f4d9b575d5550163415301f0c6c17f6db6044219fcdf57f2418c7a`、
`files_sha=f8beed4a536e6dbdd4a6ef2d7b3b0d115c81760e7cb0a97412dc35f4251c4593`（与上一批同值）。

**烧写命令（⛔ 本批未执行，由 lead 烧；四偏移、无 erase、不写 `0x510000`）**：
```
python -m esptool --chip esp32s3 -b 460800 --before default_reset --after hard_reset write_flash \
  --flash_mode dio --flash_size detect --flash_freq 80m \
  0x0      embedded/esp-adf/examples/oneye/korvo2_oneye/build-alpha-sdcard/bootloader/bootloader.bin \
  0x8000   embedded/esp-adf/examples/oneye/korvo2_oneye/build-alpha-sdcard/partition_table/partition-table.bin \
  0x10000  embedded/esp-adf/examples/oneye/korvo2_oneye/build-alpha-sdcard/korvo2_oneye.bin \
  0x410000 embedded/esp-adf/examples/oneye/korvo2_oneye/build-alpha-sdcard/srmodels/srmodels.bin
```
（⛔ 无 `erase-flash`；⛔ 不写 `creds` 分区 `0x510000` —— 设备身份/凭据保留。台面档云仍指本机栈
`192.168.110.208:18830`，烧写后仍需 `node tools/bench-mqtt-fwd.js` 端口转发。）

---

## ③ 卡上文件要求（给用户的逐字说明）

### 3.1 路径与文件名

- 固件读的路径逐字是 **`/sdcard/oneye-wifi.txt`** ⇒ 对应**电脑上看到的 = 卡根目录下的 `oneye-wifi.txt`**（**不要**放进任何子文件夹）。
- 文件名必须**逐字** `oneye-wifi.txt`（小写、连字符 `-`、扩展名 `.txt`）。
- 模板件（可直接拷到卡根目录改名使用）：`embedded/esp-adf/examples/oneye/korvo2_oneye/sd-root/oneye-wifi.txt`
  （其第 15/16 行逐字 `ssid=your-ssid` / `password=your-password`，**入库的是占位符，不是真实密码**）。

### 3.2 分区 / 格式要求（按工程既有 SD 挂载实现）

- 工程挂载实现 = ADF `components/esp_peripherals/lib/sdcard/sdcard.c` → `esp_vfs_fat_sdmmc_mount()`（**FAT 文件系统**），
  由 `main/korvo2_oneye_main.c:210` 的 `audio_board_sdcard_init(s_periph_set, SD_MODE_1_LINE)` 以 **1-line SD 模式**挂载到 `/sdcard`。
- **必须**：卡为 **FAT32**（MBR 分区表）。⚠️ **exFAT / NTFS 不适用**（FATFS 不识别）。
- ⚠️ **格式需在电脑上预先做好**：挂载配置 `format_if_mount_failed = false`（`sdcard.c:88`）
  ⇒ **固件不会替你格式化卡**；卡格式不对 ⇒ 挂载失败 ⇒ 退化成"卡不在位"的行为（见 3.4）。
- 其他参数（仅供排障参考）：`allocation_unit_size = 64 * 1024`、`max_files = get_sdcard_open_file_num_max()`（板级自检读数 5）。
- 内容编码：**UTF-8**；行尾 **LF 或 CRLF 均可**（解析器 `trim()` 会去掉行尾 `\r`）。文件大小无需关心（逐行 ≤159 字节即可）。

### 3.3 内容模板与解析规则

```
# oneye-wifi.txt —— 放到卡根目录；# 或 ; 开头的行是注释
ssid=你的WiFi名
password=你的WiFi密码
```

- 语法：`key=value` 或 `key: value`（等号两侧空白被去掉）。
- `ssid` 键名也接受 `wifi_ssid`；密码键名接受 `password` / `pass` / `psk` / `wifi_password`。
- **`ssid` 必须非空**才算命中（`parse_creds()` 的 `have_ssid` 判据）；密码可留空 = 开放网络（日志会写"为空（开放网络）"）。
- 命中后串口应出现（逐字）：
  `[wifi_prov] 从凭据文件读取 Wi-Fi：/sdcard/oneye-wifi.txt（ssid=…，密码已提供）`
  → `Wi-Fi 已获取 IP：192.168.x.x（ssid=…，来源=file:/sdcard/oneye-wifi.txt）` → 随后自动上云。
- 命中"读到了文件但**本次入网失败**"时，串口会打 `SD 卡凭据未能入网（来源 …）⇒ 释放采信槽，降级到其它通道`，
  然后走下面的 BLE 回落（此时按 3.4 的按键口径操作）。

### 3.4 ⚠️ 边界（逐条：文件不存在 / 解析失败 / 卡未挂载时的行为）

| 情形 | 固件行为（逐字落码依据） |
| --- | --- |
| **卡未插 / 挂载失败** | `wifi_prov_load_file()` 先试 `/sdcard/oneye-wifi.txt` 失败 → 回落试 `/spiffs/oneye-wifi.txt`（板内 `storage` SPIFFS 分区，1 MB）→ 都没有 ⇒ `ESP_ERR_NOT_FOUND`，串口打 `未找到 Wi-Fi 凭据文件（试过 /sdcard/oneye-wifi.txt 与 /spiffs/oneye-wifi.txt）` |
| **文件不存在 / 文件名写错 / 放进子目录** | 同上（等价于"未命中"） |
| **解析失败**（无 `ssid` 行、全注释、`ssid=` 后为空、把 `:` 和 `=` 都用错） | `parse_creds()` 返回 false ⇒ 等同"未命中"。⚠️ 只写 `password=` 不写 `ssid=` ⇒ **也判未命中** |
| **上述任一 ⇒ 之后的走向** | `wifi_prov_boot()` 继续往下走 **C1/C2 BLE**（`ENABLE_BLE_PROV=y`）⇒ 本档 `KEY_GATE=y`：**开机只待机、不广播**，串口打 `下一步：**长按 SET 键**进入 BLE 配网模式…` ⇒ **这正是用户要的"靠按键重新触发配网"**（C5 Kconfig 兜底 SSID 本档为空 ⇒ 不会顶替） |
| ⚠️ **"卡上有凭据且已提交、但本次入网失败"的时序边界** | `wifi_prov_submit_credentials_src()` 成功即 `wifi_prov_connect()` **阻塞等 IP（上限 20 s）**；失败才 `wifi_prov_release_held("file-connect-failed")` 并降级到 BLE ⇒ **需要再按一次 SET（或复位）**才能进 BLE 配网模式，不是自动的。**🔴 2026-10-13 订正（用户裁定「D2.SD卡配网失败回落BLE 配网模式」）**：本行描述的"需再按一次 SET"**已修掉** —— 见 [ALPHA-SD-FALLBACK.md](ALPHA-SD-FALLBACK.md)（`wifi_prov_boot()` 的 C3 失败分支置一次性标志 ⇒ BLE 段自动 `wifi_prov_ble_pairing_enter()`，等价长按 SET，**无需再按键**；`KEY_GATE` 的"开机且无文件时待机"语义逐字保留）。旧字逐字留痕：本行旧口径逐字为「…并降级到 BLE ⇒ **需要再按一次 SET（或复位）**才能进 BLE 配网模式，不是自动的」 |
| ⚠️ **运行中插卡不会即时入网** | 文件通道**只在启动时读一次**（唯一调用点 `korvo2_oneye_main.c:1248`）；运行中插卡只有 1 Hz 自动挂载定时器把卡挂上，**不会**触发读文件 ⇒ **必须复位**（这就是任务口径"插卡 + 复位即自动入网"） |
| ⚠️ **凭据不落 NVS** | `wifi_prov_forget()` 注释逐字"本函数**不清 NVS/flash**（…）**只**断开连接并清掉 RAM 里的 STA 配置" ⇒ **启动期采信靠"每次都读卡"**：卡持续在位是"复位即自动回连"的前提；同一会话内掉线由既有 `WIFI_EVENT_STA_DISCONNECTED` 分支自动重连（≤20 s 配网窗口之外不受影响） |
| **优先级** | FILE rank 0 > BLE rank 1 > default rank 2（`main/wifi_prov_policy.h:78-85`）；未入网时文件凭据会**抢占**BLE 的提交；已入网后非权威通道一律被拒（`REJECT_ALREADY_UP`），只有 BLE/App 是权威通道（"断旧连新"） |

### 3.5 🔴 安全口径（明文 WiFi 密码 —— **2026-10-13 按用户裁定订正**）

- 卡上的 `password=` 是**明文 WiFi 密码**：任何能读到这张卡的人/程序都能读到它（拔卡即得）。
- 🔴 **2026-10-13 就地订正（用户裁定，最高依据）**：用户逐字口径「**B1.量产支持且凭据明文**」
  ⇒ ① 文件通道（C3）是**量产品类形态**（**量产支持该通道**），② 卡上凭据**明文**、
  其后果**已由用户裁定接受**。⛔ **不得**再写成"仅台面可用 / 不得用于交付"，
  也⛔ **不再**把它登记为"未消除的残余风险"。
- 旧字逐字留痕（⛔ 不抹）—— 本节此前逐字为：
  - 「- 卡上的 `password=` 是**明文 WiFi 密码**：任何能读到这张卡的人/程序都能读到它（拔卡即得）。
     工程侧登记：`main/Kconfig.projbuild:81-82` 逐字「⚠️ 文件在 SD 卡上是**明文** Wi-Fi 密码：
     有加密通道（BLE）的机型应以 BLE 为首选通道；该残余风险的归属见 ADR-0017 D3」。」
  - 「- **ADR-0017 已登记该后果**（本报告如实复述，⛔ 不新增、⛔ 不改写）：…D3…
     承载该残余风险归属；本批**只把该能力位在台面档打开**，未新增任何放宽项。」
  - 「- 台面档 `TLS_INSECURE=y` **+** 卡上明文 Wi-Fi 密码 ⇒ **只允许在白盒台面使用**；
     ⛔ 不得用于交付/公网场景。」
- ⛔ 与安全**无关**的既有放宽项仍需分开陈述（⛔ 不因本次订正而放宽）：本报告用的这份构建里
  `sdkconfig.defaults.bench-local` 的 `CONFIG_ONEYE_FW_TLS_INSECURE=y`（跳过**对端**证书校验）
  **仍只限本机台面**（本机自签栈必需）—— 那是"构建档"的属性，**不是**文件通道的属性；
  量产预设 `sdkconfig.defaults.production` 里该位仍是 `# … is not set`（见 ADR-0017 D3 / 量产档门禁）。
- ⛔ 本报告**不含任何真实 SSID/密码**；模板件里是占位符 `your-ssid` / `your-password`。

---

## ④ 门禁读数（本批实跑）

| 门 | 命令 | 读数 |
| --- | --- | --- |
| SDK 宿主单测（基线 **28 组 / 320 用例 / 0 失败**） | `components/oneye-dev-sdk/build-host-scanfix/bin/oneye_dev_tests` | **28 组 / 320 用例 / 断言 7311 次 / 失败 0 次、失败组 0**；`TESTS_EXIT=0` |
| ABI（基线 `oneye_dev_ble` **23** / `oneye_dev_link` **34**） | `bash tools/abi-check.sh lib/x86_64-linux-gnu-gcc-13.3.0`（cwd = SDK） | **OK**：ble **23**、link **34**（+base 56 / mpp 16 / event 10 / log 15）；6 库、内嵌件符号零外泄；`ABI_EXIT=0` |
| 应用门禁 | `python3 tools/check-prov-convergence.py` | **正向断言 A1–A17 PASS**；夹具实跑 PASS；**负向对照 26 条全 PASS / 0 FAIL**；`GATE_EXIT=0` |
| defaults 门禁（本批新增档一并 `--require`） | `python3 tools/check-sdkconfig-defaults.py --sdkconfig build-alpha-sdcard/sdkconfig --require sdkconfig.defaults.bench-local --require sdkconfig.defaults.bench-sdcard` | **PASS**（`GATE_BENCH_EXIT=0`）：`bench-local：6 项全部存在且已生效`、**`bench-sdcard：4 项全部存在且已生效`**，同次并检 `sdkconfig.defaults`(33)/`.ble`(21)/`.ble-key`(2)/`.diag`(3)/`.production`(11) 全 OK；`[SKIP] sdkconfig.defaults.esp32s3`（既有排除项，非静默） |
| 产物 sdkconfig 其余关键行 | `grep` | `CLOUD_HOST="192.168.110.208"`、`CLOUD_PORT=18830`、`TRANSPORT_TLS=y`、`TLS_INSECURE=y`、`DIAG_PROV_TRACE=y`、`MBEDTLS_EXTERNAL_MEM_ALLOC=y`、`ONEYE_DEV_CREDS_REQUIRED=y`；**`mqtt.oneye.me` 出现 0 次** |
| 固件内自证 | 逐子串计数于 `korvo2_oneye.bin` | `oneye-wifi.txt`=3、`从凭据文件读取 Wi-Fi`=1、`未找到 Wi-Fi 凭据文件`=1、`file-connect-failed`=1、`SD 卡凭据未能入网`=1；`本机型未启用 SD 卡`=**0**（⇒ `#if` 命中，见 §1.2） |

---

## ⑤ `git status` ＋ "未 commit"

`embedded/esp-adf`（分支 `oneye_s3_korvo`，HEAD **`55ebdeec`**，`2026-10-12 17:23:29 +0800`）：

- **本批新增（未跟踪）**：`examples/oneye/korvo2_oneye/sdkconfig.defaults.bench-sdcard`；
  `examples/oneye/korvo2_oneye/ALPHA-SDCARD.md`（本报告；与 `build-alpha-scanfix/ALPHA-SCANFIX.md` 不同，本批落在**例程目录**，避免重建时被 `rm -rf build-alpha-sdcard` 误删）；
  `examples/oneye/korvo2_oneye/build-alpha-sdcard/`（构建目录 + 6 个脚本：`build-sdcard.sh` / `gates-sdcard.sh` / `verify-sdcard.sh` / `evidence-sdcard.sh` / `clean-and-prep.sh` / `gitinfo-sdcard.sh`；`git status -uall` 计 **2158** 条）。
  ⚠️ `clean-and-prep.sh` 会 `rm -rf build-alpha-sdcard` 重建（本批第一轮踩坑后加的）⇒ 它**先把 6 个脚本备份到 `/tmp/sdcard-kit/` 再删**；重建后脚本仍在构建目录内。
- **本批改动（已跟踪文件）**：**0 个** —— `git status --porcelain -- "sdkconfig.defaults" "sdkconfig.defaults.*"` **无输出**（既有 7 份 defaults 一字未动）。
- ⚠️ **工作区既有的并行批次改动（⛔ 非本批）**：`main/wifi_prov.c`、`main/wifi_prov.h`、`main/korvo2_oneye_main.c`、`tools/check-prov-convergence.py` 呈 ` M`（scanfix 批的 A17 修复 + 并行批次；其 `sha256` 与 scanfix 批报告 §7 记录**逐值一致** ⇒ 本批**未**改动它们）。
- **工程根生成物清理**：删除了 gitignored 的 `sdkconfig` / `sdkconfig.old`（详见 §1.3）。
- ⛔ **未 commit、未 push、未烧写**（⛔ 也未 `git add`）。

---

## ⑥ 未做 / 需裁定（≤2）

1. **本档保留 `BLE_PROV_KEY_GATE=y`（开机不广播、长按 SET 配网）⇒ 请 lead 确认这就是要的回落形态。**
   依据用户裁定"其他情况都是靠按键重新触发配网"，本档把按键门控**逐字保持**上一批（`build-alpha-scanfix/sdkconfig:629` 亦为 `y`），
   避免"因为加了文件通道而顺带改了 BLE 广播形态"。若期望"卡不在位时开机即广播"（App 随时可配），
   需**新开一档**把 `CONFIG_ONEYE_FW_BLE_PROV_KEY_GATE` 置 n（⛔ 本批不动，属另一件事）。
2. **真机时序未验证**（⛔ 本批不烧写）："插卡 + 复位 ⇒ 串口出现 `从凭据文件读取 Wi-Fi` ⇒ 拿到 IP ⇒ 自动上云"
   与"拔卡 + 复位 ⇒ 串口出现 `未找到 Wi-Fi 凭据文件` + `下一步：长按 SET 键…`"两条正向时序，
   只能由 lead 烧写后按 §3.3 / §3.4 的留痕逐字确认。另：**卡必须 FAT32 且预先格式化**（固件 `format_if_mount_failed=false`，不会替你格式化）。
