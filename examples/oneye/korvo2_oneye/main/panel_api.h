/*
 * panel_api.h —— 设备侧**本地验证面**（HTTP JSON API），供宿主验证面板与其他联调工具使用
 *
 * ⚠️ 边界（务必遵守）：本 API **不是云端设备面契约**，只是**台面/联调用的本地验证面**：
 *   - 不进 `backend/contracts`，不新增 topic、不新增 `esp.*` 影子键、不参与 `caps/up` 声明；
 *   - 只读为主（本轮全部为 GET）；不得成为产品功能路径的一部分；
 *   - 生产形态下应关闭（Kconfig `ONEYE_FW_ENABLE_PANEL_API`，缺省开，量产置 n）。
 *
 * 路由（本轮）：
 *   GET /api/ping      存活探针          → {"ok":true,...}
 *   GET /api/status    设备/链路/自检概况 → 见 panel_api.c 注释
 *   GET /api/selftest  板级参数核对明细   → 逐行 item/expect/actual/pass（与串口 [board-check] 同源）
 *   GET /api/keys      按键实时状态+历史  → current + history（含 event/up 上行与云端 ack 三态）
 *
 * 预留（下一轮：AEC 采集音频 / SD 卡录音回放）：
 *   GET /media/list          列出可播放文件（WAV/录像切片）
 *   GET /media/<path>        文件下载/流式播放（带 Range，便于浏览器拖动）
 *   POST /api/action         触发设备动作（开始/停止录音、播放指定文件）—— 须与面板一并冻结口径
 *   POST /api/simulate/key   按键事件注入（本地验证面专用）—— 见下方「本地注入」段
 */

#ifndef _PANEL_API_H_
#define _PANEL_API_H_

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 按键历史上限（含 ack 关联）；Kconfig 可调，缺省 64 */
#ifndef CONFIG_ONEYE_FW_KEY_HISTORY_MAX
#define PANEL_KEY_HISTORY_MAX 64
#else
#define PANEL_KEY_HISTORY_MAX CONFIG_ONEYE_FW_KEY_HISTORY_MAX
#endif

/* 板级自检明细行上限（与 board_selftest() 的检查项数一致，留余量） */
#define PANEL_CHECK_MAX 40

/* ------------------------------------------------------------ 数据登记（由固件其他部分调用） */

/** 启动 HTTP 服务（Wi-Fi 就绪后调用；port=0 取 CONFIG_ONEYE_FW_PANEL_PORT） */
esp_err_t panel_api_start(uint16_t port);
bool panel_api_is_running(void);

/** 设备标识（面板页眉显示） */
void panel_api_set_identity(const char *fw_version, const char *board_name);

/** 板级核对行登记（与串口 [board-check] 输出同源，供 /api/selftest 展示） */
void panel_api_record_check(const char *item, const char *expect, const char *actual, bool pass);

/** 按键事件登记：本地检测 → 生成 event/up 幂等 id → 记录 pending
 *  @param id  event/up 使用的幂等 id（调用方生成，用于与云端 ack 的 data.ref 关联） */
void panel_api_key_event(const char *key, const char *action, const char *id, uint64_t ts_ms);

/** 上行状态推进：queued | sent | failed（ack 另见 panel_api_key_ack） */
void panel_api_key_uplink(const char *id, const char *state);

/** 云端 ack 关联（台面手动 ack 路径）：在 ack 负载里查找本地事件 id 子串，命中即置 acked。
 *  @return true = 命中并已推进状态 */
bool panel_api_key_ack(const char *payload, size_t len);

/** 云端 ack 关联（**真链路**路径）：按本地事件 id 精确置位。
 *
 *  为什么需要它：契约 §4.3 的 ack `data.ref` 指向**上行信封 id**，而信封 id 由 SDK 生成
 *  （uuid，见 SDK `oneye_envelope_build(…, id=NULL, …)`），**不等于**本固件为 item 分配的
 *  `key-%05u` ⇒ 仅靠"负载里找子串"永远匹配不上，真机三态会一直停在 `sent`。
 *  因此固件维护"本端已上报 item id"的 FIFO（`oneye_dev_event_report()` 每调用一次即
 *  **强制成帧**，见 SDK `oneye_dev_event.c:1099` ⇒ 一帧一条，确认按 FIFO 精确对应），
 *  收到 `type=ack` 时弹出最旧一条并调用本函数。
 *  @param ok code=="ok" → acked；否则 failed
 *  @return true = 命中并已推进状态 */
bool panel_api_key_ack_item(const char *id, bool ok);

/** 链路/统计快照（由 oneye 回调或轮询处更新） */
void panel_api_set_link(bool cloud_link_up, const char *transport, uint32_t tx_frames, uint32_t rx_frames);

/** Wi-Fi 状态（ssid/source 供面板显示配网来源；source 形如 "file:/sdcard/oneye-wifi.txt"） */
void panel_api_set_wifi(bool connected, const char *ip, const char *ssid, const char *source);

/** 授时状态（契约 §7）：synced=true 后所有时间戳为 UTC 毫秒；offset_ms = cloud_ts − 本地运行时刻，
 *  source 形如 "caps/down.cloud_ts"。未授时时 synced=false、offset_ms=0（时间戳为运行时刻，并已告警）。 */
void panel_api_set_time(bool synced, uint64_t cloud_ts_ms, int64_t offset_ms, const char *source);

/* ------------------------------------------------------------ 本地注入（验证专用） */

/** 按键事件注入回调：由固件按键路径实现，走的是**与物理按键完全相同**的上报路径
 *  （同一函数、同一幂等 id 生成、同一 `event/up` 组装与 SDK 投递）。
 *
 *  ⚠️ 边界（必须如实表述）：触发源是 **HTTP 而非 ADC 按键**。用途 = 在没有手指可按
 *  （远程联调/自动化）时验证与演示「本地检测 → `event/up` → 云端 ack → 面板第三态」
 *  这条链路；它**不能**替代「物理按键可用」的验收证据（后者仍须人手按一次）。
 *  返回非 ESP_OK = key/action 非法（HTTP 层回 400）。 */
typedef esp_err_t (*panel_key_sim_fn_t)(const char *key, const char *action);

/** 注册按键注入回调（由 app_main 在按键服务初始化后调用；未注册时 `/api/simulate/key` 回 503） */
void panel_api_set_key_simulator(panel_key_sim_fn_t fn);

#ifdef __cplusplus
}
#endif

#endif /* _PANEL_API_H_ */
