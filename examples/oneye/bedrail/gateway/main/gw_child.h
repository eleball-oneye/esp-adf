/*
 * gw_child.h —— 网关的**子设备绑定**与 **sub.* 事件上报**（仅 CONFIG_ONEYE_T31_ROLE_GATEWAY）
 * =============================================================================
 * 用户 2026-10-23 裁定：子设备是**独立实体**（独立 node_id、用户设备列表里独立卡片），
 * 子设备**没有自己的 MQTT 连接**，遥测由网关代传。⇒ 本模块只做三件事：
 *   ① 处理云端下发（`command/down`）的 `child.pair` / `child.unpair`，并把绑定落 NVS；
 *   ② 用 oneye-link-sdk 的**运行时档案注册**（`oneye_link_profile_register_json`）把
 *      云端下发的 `linkProfile` 注册进去 —— 接新品类**不需要**重编固件；
 *   ③ 把 link-sdk 的子设备事件转成 `event/up` 上的 `sub.online|sub.offline|sub.telemetry|sub.error`。
 *
 * ★ 线程模型（硬约束，⛔ 不要打破）：
 *   SDK 回调（`oneye_dev_base` 的命令面 / link-sdk 的事件面）**只做解析 + 投递队列**，
 *   一句话都不写 NVS、不调上报 API —— 它们的上下文有"不得阻塞 20 ms"的硬要求。
 *   真正的动作（写 NVS / 注册档案 / event/up / 回执）全在本模块**唯一**的工作任务里。
 * =============================================================================
 */

#ifndef GW_CHILD_H
#define GW_CHILD_H

#include <stdbool.h>
#include <stddef.h>

#include "oneye_link_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 开机恢复：从 NVS 读出绑定表（**只读**）。
 * ⚠️ 本函数**不注册档案** —— 档案注册要等 `oneye_link_init()` 之后（见下）。
 * 在 app_main 里调用（早于任何上云动作），失败只 WARN，⛔ 不阻塞启动。
 */
void gw_child_boot(void);

/*
 * 把 NVS 里存的 `linkProfile` 逐个注册回 link-sdk。
 * 调用时机：`oneye_link_init()` 成功之后、`oneye_link_start()` **之前**
 * （link-sdk 要求"先有档案再 start"；本工程的模拟器档案也在这一段注册）。
 */
void gw_child_restore_profiles(void);

/*
 * ★ 2026-10-23 开机自检（用户要求：避免再"盲烧一轮"）：
 *   用**硬编码样本帧** `A6 01 54 0E 81 18 FC 6A`（t31_sim.c 真实帧）沿「SIM 完整档」的规则
 *   走一遍解码（复刻本仓已确认的 link-sdk 偏移语义，⛔ 不改 SDK），并打印：
 *     - SIM 档 `oneye_link_profile_validate()` 的返回码/说明；
 *     - 运行时注册表（`oneye_link_profile_count()` / `_model_at()`）；
 *     - `[自检] 样本帧 8 B → cmd=0x01 temp=3668(0.01℃) tlv=18 B`（目标值）。
 *   调用时机：main.c 注册完 SIM 档之后、`oneye_link_start()` **之前**。
 *   若自检失败 ⇒ 是 `frame{}` 参数错（不是真机帧错），按自检输出订正后再烧。
 */
void gw_child_profile_selftest(void);

/*
 * 起本模块的**唯一**工作任务（解析命令队列 + 排空子设备事件队列 + event/up）。
 * 幂等。时机：`oneye_dev_event_init()` 之后、`oneye_dev_base_start()` **之前**
 * （⛔ 早不了：event 库要先 init；⛔ 也晚不了：命令回调要靠它的队列接住）。
 * 返回 false = 队列/任务创建失败（内存不足）⇒ 命令与 sub.* 都不工作，调用方须记日志。
 */
bool gw_child_report_start(void);

/*
 * 云端命令（在 **oneye-dev-sdk 回调**里调用，payload = command JSON，**不套信封**）。
 * 只做 JSON 解析 + 投递队列；⛔ 不写 NVS、⛔ 不 ack、⛔ 不阻塞。
 */
void gw_child_on_command(const void *payload, size_t len);

/*
 * link-sdk 子设备事件（在 **link-sdk 任务**里调用）。
 * 只做定长拷贝 + 投递队列；原始帧指针在回调返回后即失效 ⇒ 必须在这里拷走。
 */
void gw_child_on_link_event(const oneye_link_event_t *evt);

/* 当前是否已有配对绑定（只读，供日志/自检使用）。 */
bool gw_child_any_bound(void);

/*
 * 2026-10-26〔允许丢帧 + 断线清队列〕云链路状态通知（由 main.c 的云事件回调调用）。
 *   ⚠️ 回调里**只置位**，不调 SDK（避免在 SDK 回调上下文重入）；
 *   真正的"清空 SDK 事件队列"在 gw_child 自己的任务里执行。
 *   up=false ⇒ 暂停 sub.telemetry 入队（丢弃并计数）+ 申请清队列；
 *   up=true  ⇒ 恢复上报并复位节流位（下一帧立刻补报）。
 */
void gw_child_on_cloud_state(bool up);

/*
 * ★ 2026-10-23：本工程的**完整自洽 T31 档案**（唯一事实源，main.c 与档案回退共用）。
 *
 * 为什么需要它（真机事故）：link-sdk 内置 `profiles/t31.json` 的 `frame.body_len=8` 且
 *   `cmd 0x01` 的 `fields` 为空 ⇒ 真 T31 帧体编码未解出前，**温度进不了 TLV 0x10**
 *   （transport 会因"无温度"丢弃该帧）。本仓 COM13 模拟器帧体已知（0x01 = temp i16le@0 +
 *   seq u16le@2，0xB1 = battery u8@0 + voltage u16le@1）⇒ 这份档案才是"能收到可观测量"的档。
 * 用途（两处，内容必须一致）：
 *   ① `main.c` 在 link_start 之前注册一次（覆盖 link-sdk 同名内置档）；
 *   ② `gw_child.c` 在「下发档案残缺 / NVS 档案不合法」时作为**回退档**注册（≥1 KB，结构齐全：
 *      match/gatt/frame/commands[]/passthrough 全部在场）。
 * ⚠️ 接**真 T31** 时：删掉这份覆盖，改用内置 t31.json（届时 cmd 0x01 编码解出后再补 fields）。
 * @return 只读的 JSON 文本（非 NULL；静态常量，⛔ 不要 free）。
 */
const char *gw_child_sim_profile_t31(void);

#ifdef __cplusplus
}
#endif

#endif /* GW_CHILD_H */
