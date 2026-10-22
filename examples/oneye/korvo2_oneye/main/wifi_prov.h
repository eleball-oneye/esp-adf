/*
 * wifi_prov.h —— Wi-Fi 配网（**多通道**，本模块是「入网后收敛点」的唯一持有者）
 *
 * ============================================================================
 * ★ 口径（ADR-0017《多通道配网与入网后收敛点》）★
 *
 * 1) **统一不变量**：任一配网通道入网成功 ⇒ `oneye_start()` 被调用，且全工程**只有一条**
 *    调用路径：
 *        wifi_prov.c 的 IP_EVENT_STA_GOT_IP 处理器 `on_wifi_event()`
 *          → `s_ready_cb()`（本文件的 `wifi_prov_ready_cb_t`）
 *          → `korvo2_oneye_main.c:on_wifi_ready()`
 *          → `cloud_start_task`
 *          → `oneye_start()`
 *    ⇒ **通道只负责「产出凭据并交给本模块」**；⛔ 通道**不得**自己调 `oneye_start()`，
 *      ⛔ 也**不得**自建一套「入网后逻辑」（面板启动 / 授时 / net_probe / 影子上报 / 埋点
 *      都是上面那条路径的**下游**）。
 *
 * 2) **通道清单**（权威列表见 ADR-0017 D1）：
 *        C1 BLE → 移动端 App          —— `wifi_prov_ble.c`（`ONEYE_FW_ENABLE_BLE_PROV=y`）
 *        C2 BLE → 另一台嵌入式设备    —— 同 C1（对端是 `kind=device` 节点，不是新协议）
 *        C3 SD 卡 / SPIFFS 凭据文件   —— `wifi_prov_load_file()`（`ONEYE_FW_ENABLE_WIFI_FILE=y`）
 *        C4 台面 HTTP `/api/action wifi_set` —— media_api.c，同样只调本模块
 *        C5 Kconfig 兜底 SSID         —— 兜底常量，**不算通道**
 *        R1 声波 / R2 二维码          —— **预留**，见下方 `wifi_prov_channel_t` 注释
 *
 * 3) **★ 优先级（同一台设备上多通道都可得时"谁先被采信"）★**：
 *        **SD 卡 WiFi 凭据 > 蓝牙配网 > 其他（声波/二维码等预留）**
 *    ⚠️ 它是**采信规则**，**不是**互斥开关：通道该开就开（无卡槽机型照样可开 BLE），
 *       但**同一时刻只有一份凭据被采信**并据此入网；判据**不看到达顺序**只看 rank ⇒
 *       「BLE 先到、SD 后到」仍然 SD 胜出。
 *    ⚠️ **已入网（拿到 IP）且采信槽未被释放时，任何通道的提交一律被拒**（只入网一次）；
 *       显式改配（台面 `wifi_set`）与 `prov.reset` 走「先 `wifi_prov_release_held()` /
 *       `wifi_prov_forget()` 再提交」的**显式**路径 —— 那是操作者声明的"重新配网"意图，
 *       ⛔ 但**上云链路只启动一次**这一点不变（`oneye_start()` 仍只有收敛点下游一个调用点）。
 *    ★ 2026-10-12 就地订正（用户裁定，最高依据）：上面这条**多了一个例外** ——
 *       **App（BLE 通道 C1/C2）发起的配网 = 权威通道，一经受理即权威生效**：
 *       设备**先 `esp_wifi_disconnect()` 断开现有连接**、丢弃旧配置的采信态，
 *       再 `esp_wifi_set_config(新凭据)` → `esp_wifi_connect()`（"断旧连新"）。
 *       用户裁定逐字：「**已有app发起配网时设备要断现有wifi重新连接目标wifi**，否则app会卡在配网中这个流程。」
 *       ⇒ `REJECT_ALREADY_UP` 在 **BLE 通路上不可达**（`wifi_prov_channel_is_authoritative()`）。
 *       ⛔ **只对 BLE 通道**放宽：C3 凭据文件 / C4 台面改配 / C5 兜底 / 预留通道的
 *       优先级与"只入网一次"语义**一字未改**（参 `wifi_prov_policy.h` 的 rank 与判据分派）。
 *       ⛔ 旧字逐字留痕（⛔ 不抹）：本段此前逐字为
 *       「⚠️ **已入网（拿到 IP）且采信槽未被释放时，任何通道的提交一律被拒**（只入网一次）；」
 *       —— 该口径对"任何通道"的表述**已被本次裁定收窄**为"任何**非权威**通道"。
 *    判据本体 = `wifi_prov_policy.h`（纯逻辑，与主机侧单测 `tools/test-prov-priority.c`
 *    编译**同一份源**），落码在 `wifi_prov_submit_credentials*()`。
 *
 * 4) **`ONEYE_FW_ENABLE_WIFI_FILE` 是「机型能力位」**（有卡槽机型 y / 无卡槽机型 n），
 *    ⛔ **不是**「量产 vs 台面」开关 —— 把它当量产开关的后果是纯生产件一个通道都不剩、
 *    `oneye_start()` 永不调用、设备上不了云（2026-10-07 真机实测）。构建期门禁
 *    （main/CMakeLists.txt）保证「一个可用通道都没有」的配置**编不过**。
 *
 * 5) 🔴 **2026-10-13 就地订正（用户裁定，最高依据）** —— 安全口径的**正名**：
 *    用户逐字口径「**B1.量产支持且凭据明文**」⇒ C3（SD 卡/SPIFFS 凭据文件）是**量产品类形态**：
 *    **量产支持该通道 ＋ 卡上凭据为明文**；明文 Wi-Fi 密码这一后果**已由用户裁定接受**
 *    （⛔ **不再**记为"未消除的残余风险"，也⛔ **不再**写"仅台面可用"）。
 *    ⛔ 行为/判据未改：rank 仍是 FILE 0 > BLE 1 > default 2；"有加密通道（C1/C2）的机型优先用
 *    BLE"仍是有益的首选顺序（安全增强，不是前提）。
 *    旧字逐字留痕（⛔ 不抹）—— 本段此前逐字为：
 *    「5) ⚠️ 安全口径（未消除的残余风险，如实登记）：C3 在 SD 卡上是**明文** Wi-Fi 密码。
 *      解法是让有加密通道（C1/C2）的机型以 BLE 为首选，**不是**关掉唯一通道；
 *      剩余部分按 ADR-0017 D3 归属「配网凭据落盘形态」单独决策。」
 *
 * 凭据文件格式（密钥=值，逐行；`#` 注释；两侧空白与 CR 自动去除）：
 *   ssid=MyWiFi
 *   password=MyPass          # pass / psk 亦可
 *
 * 查找顺序：`/sdcard/oneye-wifi.txt` → `/spiffs/oneye-wifi.txt` → Kconfig(`CONFIG_ONEYE_FW_WIFI_SSID/_PASSWORD`)
 */

#ifndef _WIFI_PROV_H_
#define _WIFI_PROV_H_

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_PROV_SSID_MAX 33
#define WIFI_PROV_PASS_MAX 65

/* ------------------------------------------------------------------ 通道标识与优先级判据 */

/* ★ 通道枚举（`wifi_prov_channel_t`）与**优先级判据**（`wifi_prov_policy_decide()` /
 *   `wifi_prov_channel_rank()`）都在 `wifi_prov_policy.h`：那是一份**纯逻辑**头
 *   （不 include ESP-IDF），因此主机侧单测 `tools/test-prov-priority.c` 与固件
 *   编译的是**同一份判据源**，而不是各自抄一份。
 * ⚠️ 「新增通道三步」与 rank 登记也写在那里 —— 本文件不重复枚举定义，避免两处漂移。 */
#include "wifi_prov_policy.h"

/** 通道 → **规范来源标签**（新通道经 `wifi_prov_submit_credentials()` 时用它写 `wifi.source`）。
 *  ⚠️ 既有通道保留各自**更利于诊断**的历史字符串，不强行统一：
 *     C3 用 `file:<命中路径>`（例 `file:/sdcard/oneye-wifi.txt`，面板/README 已按此口径）、
 *     C4 用 `api`、C5 用 `kconfig` —— 三者都**不**经本函数，属有意保留而非漏接。 */
const char *wifi_prov_channel_source(wifi_prov_channel_t ch);

/* ------------------------------------------------------------------ 生命周期 */

/** 初始化 netif/event/wifi（幂等）；不连接 */
esp_err_t wifi_prov_init(void);

/**
 * 为 **AP 扫描**准备好 Wi-Fi（幂等）：置扫描态后 `esp_wifi_start()`，**并把 STA 收回空闲态**。
 * 扫描态下**不**自动 `esp_wifi_connect()`（否则拿不到凭据时会空转重连，
 * 在串口上刷屏且白耗电）。C1/C2 的 `prov.scan.req` 需要它在先。
 *
 * 🔴 2026-10-13〔本批 · 真机证据〕新增第二件事 —— **扫前把 STA 收回空闲态**：
 *   旧行为（本批修掉的）只置位 + `esp_wifi_start()`，**不断开**。真机后果（2026-10-13 14:17:03）：
 *   上一轮用**错误密码** ⇒ `DISCONNECTED reason=15` 被自动重连成循环 ⇒ 重试配网时的 `prov.scan.req`
 *   落在 STA **仍在 connecting** 的时刻上 ⇒ `esp_wifi_scan_start()` 返回 `ESP_ERR_WIFI_STATE`
 *   （IDF 文档逐字："WiFi still connecting when invoke esp_wifi_scan_start"）。
 *   现在：先置抑制位 `s_scan_only`（**必须先于断开**：断开事件异步派发，否则会用旧配置自动重连），
 *   再 `esp_wifi_disconnect()`（connecting / connected 都断，中止旧凭据的连接尝试），
 *   然后**等会话结束边** `WIFI_PROV_SESSION_ENDED_BIT`（有界，`WIFI_PROV_QUIET_WAIT_MS`）。
 *
 * 返回：`ESP_OK` = 已置扫描态、Wi-Fi 已起、**并且已经把 STA 收回空闲态**（断开 + 等会话结束边）；
 * 非 `ESP_OK` = 初始化 / `esp_wifi_start()` 失败（调用方**如实**上报，⛔ 不假装"0 个 AP"）。
 *   ⚠️ 断开自身的返回码**不**升级为失败（`esp_wifi_disconnect()` 空闲态调用是 OK；万一报错，
 *     只留痕并继续）—— STA 到底空不空闲由 `wifi_prov_scan_json()` 里的 `scan_start()` 定夺。
 */
esp_err_t wifi_prov_prepare_scan(void);

/**
 * 扫描周边 AP → JSON 数组（`[{"ssid":…,"rssi":…,"auth":…}]`，只含 SSID/RSSI/加密类型）。
 *
 * 🔴 2026-10-13〔本批 · 真机证据〕**失败不得被抹成"0 个 AP"**：
 *   旧行为把 `esp_wifi_scan_start()` 的 `ESP_ERR_WIFI_STATE` 当成"可以继续" ⇒ 继续
 *   `esp_wifi_scan_get_ap_records()`（本次根本没有扫描结果，条数 0）⇒ 组出 `[]` 并以 `ESP_OK`
 *   返回，link 层遂回 `prov.scan.rsp{aps:[]}`（真机 payload = 10 B）⇒ App 判
 *   「未获取到设备可用网络列表」（`ApListGate` 对空列表一律拦住，该口径**一字未改**）。
 *   现在：扫描改为**非阻塞发起 + 等 `WIFI_EVENT_SCAN_DONE`**，三种失败都**如实返回错误码**：
 *     · `esp_wifi_scan_start()` 返回非 `ESP_OK`（含 `ESP_ERR_WIFI_STATE` / `ESP_ERR_WIFI_TIMEOUT`）；
 *     · 等 `WIFI_EVENT_SCAN_DONE` 超时（> `WIFI_PROV_SCAN_DONE_TIMEOUT_MS`）⇒ `ESP_ERR_TIMEOUT`；
 *     · `SCAN_DONE.status != 0`（扫描本身失败）⇒ `ESP_FAIL`。
 *   ⇒ link 层既有映射把它变成 `error{code:"internal", msg:"prov scan failed"}`（⛔ 不新增错误码、
 *     ⛔ 不新增帧/字段）。**只有**「扫描成功且周围真的 0 个 AP」才会返回 `[]` + `ESP_OK`。
 */
esp_err_t wifi_prov_scan_json(char *out, size_t cap);

/* ------------------------------------------------------------------ 凭据来源与入网（唯一入口） */

/** 从 SD/SPIFFS 凭据文件读取（找到返回 ESP_OK 并回填；未找到返回 ESP_ERR_NOT_FOUND） */
esp_err_t wifi_prov_load_file(char *ssid, size_t ssid_cap, char *pass, size_t pass_cap,
                              char *src_path, size_t path_cap);

/**
 * 🔴 2026-10-22〔WiFi 凭据持久化批〕从 **NVS** 读回设备自己记住的那份 WiFi 凭据。
 *
 * 为什么需要它（现场缺陷本体）：配网成功 = 只把凭据交给 `esp_wifi_set_config()` + 连上，
 *   **启动流程里没有任何一处去读"上一次配网留下的凭据"** ⇒ 一复位，`wifi_prov_boot()` 只能
 *   从"SD 凭据文件"重新开始，卡不在位就落到 BLE 段 ⇒ 设备回到广播、云端显示离线
 *   （2026-10-22 上午两板实测：`rst:0x1 (POWERON)` 之后直接 `BLE 配网通道已就绪：广播中`，
 *    无 `esp_wifi_connect`、无 `GOT_IP`）。
 *
 * 语义（⛔ 不新增通道语义、⛔ 不改判据）：本函数**只读**，把读到的凭据回填给调用方；
 *   调用方（`wifi_prov_boot()`）必须经**唯一入网入口** `wifi_prov_submit_credentials*()`
 *   以 `WIFI_PROV_CHAN_NVS` 递交 —— 它和 SD 文件 / BLE 一样只是"凭据从哪来"。
 *
 * 返回：
 *   - `ESP_OK` —— 读到一份非空 ssid（`pass` 可能为空 = 开放网络）；
 *   - `ESP_ERR_NVS_NOT_FOUND` / 其它 NVS 错误 —— 没记住（首次上电 / 被 `wifi_prov_forget_wifi()` 擦过）；
 *   - `ESP_ERR_INVALID_ARG` —— 入参为空。
 * ⚠️ 只回填 ssid/pass，⛔ 不打印密码（日志只说"已提供/为空"）。
 */
esp_err_t wifi_prov_load_nvs(char *ssid, size_t ssid_cap, char *pass, size_t pass_cap);

/**
 * ★ **所有配网通道的唯一入网入口**（ADR-0017 D2/D4）★
 *
 * 做的事只有三件：① 过**优先级判据**（`wifi_prov_policy_decide()`）；② 记录来源；
 * ③ 采信的话交给连接层（连接层是本文件内部的 `static`，模块外拿不到）。
 * **入网成功之后**的一切由本模块的 `IP_EVENT_STA_GOT_IP` 处理器统一触发，通道不需要（也不许）知道。
 *
 * 返回：
 *   - `ESP_OK` —— 采信且已入网；
 *   - `ESP_ERR_TIMEOUT` —— 采信了但没连上（可再试/换凭据）；
 *   - `ESP_ERR_INVALID_STATE` —— **被优先级判据拒绝**：未采信、未发起入网，
 *     串口有 `[prov-priority]` 留痕（槽里已有更高/同级优先级的凭据，或设备已入网）；
 *     ⚠️ 2026-10-12 起：**权威通道（BLE/App）拿不到"已入网"这一种拒绝**（见上）；
 *     它能拿到本错误码的情形只剩「未入网且槽被更高优先级的 C3 凭据占用」。
 *   - `ESP_ERR_INVALID_ARG` —— ssid 为空。
 *
 * ⚠️ 本函数**阻塞**至获得 IP 或超时（缺省 20 s）。C1/C2 的 provider 在
 *    NimBLE 主机任务上下文里调用它 —— 这与既有例程 `korvo2_llm_chat/main/prov_service.c:280-287`
 *    的既有口径一致（那里同样是 `prov_service_connect(..., 20000)` 同步调用）。
 *    被拒绝时立刻返回（不阻塞），BLE 通道据此给 App 回失败帧。
 */
esp_err_t wifi_prov_submit_credentials(const char *ssid, const char *pass, wifi_prov_channel_t ch);

/**
 * 同上，但允许通道**覆盖来源标签**（本工程既有诊断口径：C3 用 `file:<命中路径>`、
 * C5 用 `kconfig`，都是历史字符串，不强行统一成 `file`/`kconfig`）。
 * ⚠️ 除标签外与上面**完全同一条**路径：同一个判据、同一个连接层、同一个收敛点。
 */
esp_err_t wifi_prov_submit_credentials_src(const char *ssid, const char *pass,
                                           wifi_prov_channel_t ch, const char *src_override);

/**
 * ★ **释放当前被采信的凭据**（清采信槽 + 来源标签）。**不**改 Wi-Fi 连接状态、⛔ 不写 flash。
 *
 * 只有两处该调它，且都必须是**调用方的显式意图**（不是判据的例外）：
 *   ① 启动期：高优先级通道（SD 凭据）**解析成功但入网失败** ⇒ 释放后降级到 BLE / 兜底，
 *      否则低优先级的通道会被判据挡在门外，设备永远只能重试那份连不上的旧凭据；
 *   ② 运行期显式改配（台面 `wifi_set`）⇒ 释放后重新提交，允许"换网"。
 * ⚠️ 它**不**等于 `wifi_prov_forget()`：后者还会断开连接、清 STA 配置、回到未配网态。
 *    `reason` 只进日志（留痕），可以传 NULL。
 */
void wifi_prov_release_held(const char *reason);

/** 记录凭据来源（展示用：`file:/sdcard/…` / `ble` / `kconfig` / `api`）。
 *  ⚠️ 只改标签，**不**改采信槽 —— 采信槽只由 `submit` / `release_held` / `forget` 改。 */
esp_err_t wifi_prov_set_source(const char *src);

/** 清除当前凭据并回到未配网态（供通道的 `prov.reset` 用；**只清当前 RAM 采信态/STA 配置；flash/NVS 里的凭据保留**）。
 *  🔴 2026-10-13 口径订正（⛔ 旧字逐字留痕 = 「只清 RAM、不写 flash」）：说的就是上面这句 —— 清的只有 RAM
 *  里的采信槽与 STA 配置，flash/NVS 里的凭据**一字未写**。
 *  同时清空采信槽 ⇒ 恢复"任一通道都可被重新采信"的状态。 */
void wifi_prov_forget(void);

/**
 * 🔴 **2026-10-21〔BOOT 长按忘记 WiFi 批〕显式"忘记这台设备记住的 WiFi"**（`wifi_prov_forget()` 的**持久化**版本）。
 *
 * 与 `wifi_prov_forget()` 的差别只有一条（⛔ 不要合并）：本函数在"清 RAM 采信槽 + STA 配置"之后
 * **再擦掉 NVS 里的 Wi-Fi 持久化配置**（`esp_wifi_stop()` + `esp_wifi_restore()`）⇒ 重新上电
 * 也不会再用旧 SSID 自动重连。`prov.reset` 的契约语义（只清 RAM、flash/NVS 保留）**一字未改**。
 *
 * 唯一合规调用者 = `wifi_prov_ble_pairing_reprovision()`（BOOT/SET 长按 ≥3 s 的"忘记并重配"）。
 * ⛔ 不动 creds 分区 / ⛔ 不动 nvs 其它命名空间 / ⛔ 日志不打印 SSID 或密码。
 * ⚠️ 会**阻塞**（`esp_wifi_stop()`）⇒ 只在任务上下文调用，⛔ 不在 sys_evt / esp_timer 回调里调用。
 *
 * @param reason 只进日志的成因标签（可 NULL）
 * @return `ESP_OK` = 已忘记（或本来就没初始化）；否则为 `esp_wifi_restore()` 的错误码（其余步骤只留痕不升级）。
 */
esp_err_t wifi_prov_forget_wifi(const char *reason);

/**
 * 联网就绪回调：获得 IP 时触发（含运行期改配后的重新入网）。
 *
 * ★ 这是「入网 ⇒ 上云」的**唯一收敛点**的触发边（ADR-0017 D2）★
 * 所有配网通道共用它。
 *
 * ⚠️ 本回调在**系统事件任务**上下文被调用（`on_wifi_event()` 的 GOT_IP 分支内）。回调体必须
 *    保持"有界"：可以同步做**有明确上限**的短活，⛔ 不要在这里再建"每通道一份"的常驻任务。
 *    本工程的做法（`korvo2_oneye_main.c:on_wifi_ready()`）= 一次性守卫 + 入网日志 + net_probe
 *    自检（≤3×3 s）+ SNTP 授时（≤2×10 s，**必须在建链之前**，严格 TLS 需要有效时钟）
 *    + 面板启动 + **只建一个** `cloud_start_task`（其余全部复用它的栈）。
 *    ⚠️ 2026-10-07 之前授时/自检挂在 C3/C5 专属的启动路径上 ⇒ BLE 通道会跳过它们；现在二者
 *    与面板/上云同在收敛点下游，**所有通道**必然经过（门禁 A11/A12 断言）。
 */
typedef void (*wifi_prov_ready_cb_t)(void);
void wifi_prov_set_ready_cb(wifi_prov_ready_cb_t cb);

/**
 * 🔴 2026-10-12〔本批〕**会话结束通知**：`WIFI_EVENT_STA_DISCONNECTED` 到达时被调用
 * （在系统事件任务上下文；本模块已经把 `s_connected=false`、`s_ip[0]='\0'` 落好之后才通知）。
 *
 * 为什么需要这条边（真机证据 + 逐字理由）：
 *   · `connected` 上报的幂等位（`wifi_prov_status.h` 的 ②）此前**跨会话永不复位** ⇒
 *     第二轮配网（App 走 `[prov-switch]` 断旧连新 → GOT_IP）**再也报不出** `connected`
 *     ⇒ App 卡在"配网中"（2026-10-12 15:36 真机第二轮就是这样失败的）；
 *   · 复位点是"**这一次配网会话结束**"，而它的唯一权威边就是本回调 ——
 *     Wi-Fi 生命周期归本模块持有（门禁 A15f），⛔ 应用层不得自己去注册 Wi-Fi 事件。
 *
 * ⚠️ 本模块**只通告"会话结束了"这一件事**，⛔ 不解释它在应用层的含义（要不要复位幂等位、
 *    要不要补发/清理别的状态，由注册方决定）；⛔ 本回调**不改**连接行为：
 *     `s_scan_only` / `s_reprovision` 两个抑制位与"是否自动重连"的既有语义**一字未改**。
 * ⚠️ 回调体必须"有界"：它在系统事件任务上执行（与 ready 回调同一类约束）。
 */
typedef void (*wifi_prov_disconnected_cb_t)(void);
void wifi_prov_set_disconnected_cb(wifi_prov_disconnected_cb_t cb);

bool wifi_prov_is_connected(void);
const char *wifi_prov_ip(void);        /* "" = 未连接 */
const char *wifi_prov_ssid(void);      /* 当前/最近使用的 SSID */
const char *wifi_prov_source(void);    /* "file:/sdcard/…" | "ble" | "kconfig" | "api" | "" */

#ifdef __cplusplus
}
#endif

#endif /* _WIFI_PROV_H_ */
