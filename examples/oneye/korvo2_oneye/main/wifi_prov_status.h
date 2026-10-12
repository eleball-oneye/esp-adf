/*
 * wifi_prov_status.h —— 「该不该上报 prov.status{state=connected}」的**唯一判据**（纯逻辑）
 *
 * ★ 为什么单独一个头（而不是把判据写在 korvo2_oneye_main.c 里）★
 *   与 `wifi_prov_policy.h` 同一个理由：**判据只有一份源** —— 固件
 *   （`korvo2_oneye_main.c` 的 `prov_status_report_connected_once()`）与主机侧夹具
 *   （`tools/test-prov-priority.c` 的[夹具④]）编译的是**同一份**判据，而不是各抄一份
 *   （抄件只能证明抄件）。本头**不 include 任何 ESP-IDF 头** ⇒ 宿主机上 cc 可直接跑。
 *
 * ★ 契约依据（⛔ 本文件不新增任何东西，只是把**既有**契约写成可判定的形状）★
 *   · 状态集合：`contracts/local/link-frames.json` 的 `prov_states`（`connected` 是**既有**项）；
 *   · 帧形状：同文件 `frames[]` 的 `prov.status` —— `required` 只有 `state`，
 *     `optional` 是 `ssid` / `ip` / `source` / `err`，`desc` 逐字写明「connected 时 ip 必填」；
 *   · App 侧判据（已按契约解析）：`isOnline = (state=="connected") && ip 非空`。
 *   ⇒ 本判据只回答两件事：**"有没有真 IP"** 与 **"是不是已经报过"**。
 *
 * ⚠️ 四处口径（**判据在这里**，落码点在 `korvo2_oneye_main.c`）：
 *   ① **非空 ip**：没拿到 IP ⇒ ⛔ 不报 `connected`（如实停在"配网中"）；
 *      ⛔ 不许拿空串 / 占位串 冒充 —— 那会让 App 把没联网的设备判成在线；
 *   ② **只报一次**（**同一次配网会话内**）：收敛点 `on_wifi_ready()` 会在**每次** GOT_IP
 *      （含掉线重连）被调用 ⇒ 没有幂等判据就会刷屏；"已报过"这件事由调用方的静态位承载
 *      （本头不含状态 ⇒ 可重入）；
 *   ③ **失败不置位**：只有**真的把帧交给 SDK 既有发送路径**（返回 OK）才算"报过"；
 *      链路尚未初始化 / 尚未启动时 ⛔ 不得置位 ⇒ 下一次 GOT_IP 仍会尝试补报；
 *   ④ **会话边界的复位**（🔴 2026-10-12 本批，真机证据见下）：Wi-Fi **断开** = 这一次配网
 *      会话结束 ⇒ "已报过"作废（下一段会话拿到 IP **可以再报一次**）。
 *      ⛔ 这不是放宽 ①（ip 非空）/ ②（本会话只报一次）—— 判据本体**一字未改**，
 *      复位的是**状态**（"是否已报过"），不是判据。
 *   ⛔ 本判据**不**参与任何"要不要重试 / 等多久"的决策：上报是**非阻塞的一次发送**，
 *      重试时机就是"下一次 GOT_IP" —— 没有定时器、没有阻塞等待、没有自建发送路径。
 */

#ifndef _WIFI_PROV_STATUS_H_
#define _WIFI_PROV_STATUS_H_

#include <stdbool.h>
#include <stddef.h>

/*
 * ① 「这一刻的状态值不值得发 connected」：必须**既**已入网、**又**有非空 IP。
 *    ⛔ 传 NULL 或空串一律判否（契约：connected 时 ip 必填）。
 */
static inline bool wifi_prov_connected_reportable(bool got_ip, const char *ip)
{
    if (!got_ip) {
        return false;                     /* 没拿到 IP：这不是 connected（可能是重连中的旧回调） */
    }
    if (ip == NULL || ip[0] == '\0') {
        return false;                     /* 契约硬判据：⛔ 空 ip 一律不许报 connected */
    }
    return true;
}

/*
 * ② 幂等：**已经报过就不再报**（⛔ 不刷屏），否则回到 ① 的判据。
 */
static inline bool wifi_prov_connected_should_report(bool got_ip, const char *ip,
                                                     bool already_reported)
{
    if (already_reported) {
        return false;
    }
    return wifi_prov_connected_reportable(got_ip, ip);
}

/*
 * ③ 「已报过」这一位怎么推进：**只有发送成功才算报过**。
 *    ⛔ `send_ok == false` 时保持原值（false）⇒ 调用方下一次 GOT_IP 会再试；
 *    ⛔ 不许写成无条件 `return true`（那会把"发失败"写成"设备已上报"，App 永远等不到 connected）。
 */
static inline bool wifi_prov_connected_reported_after(bool already_reported, bool send_ok)
{
    return already_reported || send_ok;
}

/*
 * ④ 🔴 2026-10-12〔本批〕**会话边界的复位**：Wi-Fi 断开（会话结束）⇒ 「已报过」作废。
 *
 * 真机证据（⛔ 不重复采集，两轮均为 2026-10-12）：
 *   · 15:25 第一轮：`GOT_IP 192.168.110.90` ⇒ `★ 已上报 prov.status{state=connected, …}`（只报一次）；
 *   · 15:36 第二轮（栈修好后重跑）：`[prov-switch] 先断开现有连接 rc=0 ⇒ set_config + connect`
 *     → `GOT_IP`（1.3 s）→ `provider 返回 ESP_OK（耗时 1311 ms）` ⇒ **此后没有再上报 connected**
 *     ⇒ App 一直停在"配网中"直到超时。
 *   成因 = ② 的幂等位**跨会话不复位**：第二轮是**新的一次配网**，位却还停在第一轮的 true。
 *
 * 为什么复位边选"Wi-Fi 断开"（语义：**一次配网会话的结束**，逐字理由）：
 *   · "已报过"回答的是"**这一段连接会话**里报过没有"；Wi-Fi 断开之后这一段的 `connected`
 *     已经不成立（`wifi_prov_ip()` 被清空、`wifi_prov_is_connected()` 转 false）⇒ 位必须随之作废；
 *   · 三条"重新配网"的路径**都物理经过断开边**：App/权威通道的 `[prov-switch]` 先
 *     `esp_wifi_disconnect()`（`wifi_prov.c:73` 逐字：它会派发 `WIFI_EVENT_STA_DISCONNECTED`）、
 *     台面改配走 `wifi_prov_release_held()` + submit、`prov.reset` 走 `wifi_prov_forget()`
 *     （其内 `esp_wifi_disconnect()`）⇒ **一个复位边覆盖三者**，⛔ 不需要在三个地方各写一遍；
 *   · ⛔ 不会退化成刷屏：本固件的断开分支**不无条件重连**（`wifi_prov.c:137-142` 的
 *     `s_scan_only || s_reprovision` 分支"不自动重连"）⇒ 真正的"断开 → 再报"必然对应
 *     一次真实的状态变迁（掉线重连 / 操作者重新配网），而**不是**按 GOT_IP 频率刷屏；
 *   · ⛔ 退化的反例（本函数存在的意义）：第二次拿到 IP 时位仍为 true ⇒ 判据判否 ⇒
 *     App 卡在"配网中"（就是上面 15:36 的第二轮）。
 *
 * 语义：**与入参无关**，复位就是"清成未报过"。
 *   ⛔ 不许写成 `return already_reported;`（那就是"永不复位" = 本批要修掉的旧行为）；
 *   ⛔ 也不许把它塞进 `prov_status_report_connected_once()` 的判据之前 ——
 *      那等于每次 GOT_IP 都复位 ⇒ 退化成"每次都报"（刷屏，违背 ②）。
 */
static inline bool wifi_prov_connected_reported_after_disconnect(bool already_reported)
{
    (void)already_reported;   /* 复位与"之前报没报过"无关：断开之后一律回到"未报过" */
    return false;
}

#endif /* _WIFI_PROV_STATUS_H_ */
