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
 * ⚠️ 三处口径（**判据在这里**，落码点在 `korvo2_oneye_main.c`）：
 *   ① **非空 ip**：没拿到 IP ⇒ ⛔ 不报 `connected`（如实停在"配网中"）；
 *      ⛔ 不许拿空串 / 占位串 冒充 —— 那会让 App 把没联网的设备判成在线；
 *   ② **只报一次**：收敛点 `on_wifi_ready()` 会在**每次** GOT_IP（含掉线重连）被调用 ⇒
 *      没有幂等判据就会刷屏；"已报过"这件事由调用方的静态位承载（本头不含状态 ⇒ 可重入）；
 *   ③ **失败不置位**：只有**真的把帧交给 SDK 既有发送路径**（返回 OK）才算"报过"；
 *      链路尚未初始化 / 尚未启动时 ⛔ 不得置位 ⇒ 下一次 GOT_IP 仍会尝试补报。
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

#endif /* _WIFI_PROV_STATUS_H_ */
