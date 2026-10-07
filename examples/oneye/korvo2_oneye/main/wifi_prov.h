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
 *    判据本体 = `wifi_prov_policy.h`（纯逻辑，与主机侧单测 `tools/test-prov-priority.c`
 *    编译**同一份源**），落码在 `wifi_prov_submit_credentials*()`。
 *
 * 4) **`ONEYE_FW_ENABLE_WIFI_FILE` 是「机型能力位」**（有卡槽机型 y / 无卡槽机型 n），
 *    ⛔ **不是**「量产 vs 台面」开关 —— 把它当量产开关的后果是纯生产件一个通道都不剩、
 *    `oneye_start()` 永不调用、设备上不了云（2026-10-07 真机实测）。构建期门禁
 *    （main/CMakeLists.txt）保证「一个可用通道都没有」的配置**编不过**。
 *
 * 5) ⚠️ 安全口径（未消除的残余风险，如实登记）：C3 在 SD 卡上是**明文** Wi-Fi 密码。
 *    解法是让有加密通道（C1/C2）的机型以 BLE 为首选，**不是**关掉唯一通道；
 *    剩余部分按 ADR-0017 D3 归属「配网凭据落盘形态」单独决策。
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
 * 为 **AP 扫描**准备好 Wi-Fi（幂等）：置扫描态后 `esp_wifi_start()`。
 * 扫描态下**不**自动 `esp_wifi_connect()`（否则拿不到凭据时会空转重连，
 * 在串口上刷屏且白耗电）。C1/C2 的 `prov.scan.req` 需要它在先。
 */
esp_err_t wifi_prov_prepare_scan(void);

/** 扫描周边 AP → JSON 数组（`[{"ssid":…,"rssi":…,"auth":…}]`，只含 SSID/RSSI/加密类型） */
esp_err_t wifi_prov_scan_json(char *out, size_t cap);

/* ------------------------------------------------------------------ 凭据来源与入网（唯一入口） */

/** 从 SD/SPIFFS 凭据文件读取（找到返回 ESP_OK 并回填；未找到返回 ESP_ERR_NOT_FOUND） */
esp_err_t wifi_prov_load_file(char *ssid, size_t ssid_cap, char *pass, size_t pass_cap,
                              char *src_path, size_t path_cap);

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

/** 清除当前凭据并回到未配网态（供通道的 `prov.reset` 用；**只清 RAM、不写 flash**）。
 *  同时清空采信槽 ⇒ 恢复"任一通道都可被重新采信"的状态。 */
void wifi_prov_forget(void);

/**
 * 联网就绪回调：获得 IP 时触发（含运行期改配后的重新入网）。
 *
 * ★ 这是「入网 ⇒ 上云」的**唯一收敛点**的触发边（ADR-0017 D2）★
 * 所有配网通道共用它。⚠️ 在系统事件任务上下文调用，回调内不要做重活——需要时自行建任务
 * （本工程的做法见 `korvo2_oneye_main.c:on_wifi_ready()`：只建一个任务，其余复用）。
 */
typedef void (*wifi_prov_ready_cb_t)(void);
void wifi_prov_set_ready_cb(wifi_prov_ready_cb_t cb);

bool wifi_prov_is_connected(void);
const char *wifi_prov_ip(void);        /* "" = 未连接 */
const char *wifi_prov_ssid(void);      /* 当前/最近使用的 SSID */
const char *wifi_prov_source(void);    /* "file:/sdcard/…" | "ble" | "kconfig" | "api" | "" */

#ifdef __cplusplus
}
#endif

#endif /* _WIFI_PROV_H_ */
