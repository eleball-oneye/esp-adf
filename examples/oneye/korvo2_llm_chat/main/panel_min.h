/*
 * panel_min.h —— 最小状态面板（**非契约**：只做串口状态呈现）
 *
 * 为什么不做 LCD/HTTP 状态面：
 *   - 本工程的目标是「长按说话 → 云端 LLM → 喇叭回放」最小闭环；
 *   - 设备本地 HTTP 端点属**契约增量**（contracts/local/lan-link.md §5 维护规则 2），
 *     不得以"验证面"名义夹带 ⇒ 本模块只输出串口日志，不新增任何 HTTP 路由。
 *
 * 上位机可用 `idf.py monitor` 观察；状态行前缀固定为 `[panel]`，便于脚本抓取。
 */

#ifndef _PANEL_MIN_H_
#define _PANEL_MIN_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化（打印启动横幅） */
void panel_min_init(void);

/** 上报一次会话/链路状态（state 建议用 LLM_CLIENT_* 的字符串口径） */
void panel_min_state(const char *state, const char *detail);

/** 通用备注（如识别文本、错误码） */
void panel_min_note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/** 一键状态 JSON（供调试打印；**不**经 HTTP 暴露） */
void panel_min_status_json(char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* _PANEL_MIN_H_ */
