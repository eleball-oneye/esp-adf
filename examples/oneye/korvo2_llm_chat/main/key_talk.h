/*
 * key_talk.h —— 板载 6 键 ADC 键盘：REC「长按说话」+ SET「单击起新对话」
 *
 * 语义（用户需求：「按键长按触发 mic 收音来和云端 LLM 通信」+「用按键起新对话，在新对话中继续」）：
 *   REC  按下 → 长按达到 `CONFIG_ONEYE_LLM_TALK_MIN_PRESS_MS`（缺省 600 ms）→ **开始收音**（on_talk_start）
 *        松开 → 提交本轮（on_talk_stop，携带按住时长）→ 服务端 ASR/LLM/TTS
 *        短按（未达阈值）→ on_short_press（当前实现 = 打断正在播放的下行音频 / 忽略误触）
 *        回放中按下 → on_press_down（供应用立刻打断，满足契约「打断 ≤200 ms」）
 *   SET  **单击**（短按松开）→ on_new_conversation：请求服务端**开一段新对话**
 *        （契约 §12.1 的 `conv.new`；此后轮次计入新段，旧段内容保留、新段不继承旧上下文）
 *
 * 实现说明（**关键，勿臆改**）：
 *   - 板级 `audio_board_key_init()` 用的 `ADC_DEFAULT_ARR()` 里 `press_judge_time = 3000`，
 *     即 ADF 的长按判定要 3 s ⇒ 直接用它做「长按说话」体验极差；
 *     故本模块**自行**创建 ADC 按键外设，把 `press_judge_time` 覆写为 Kconfig 阈值
 *     （ADC 电平阶梯与通道照抄 board.c:158-162），再把同一个外设交给 `input_key_service`
 *     做 act_id → user_id 的映射（识别 REC 键 = BUTTON_REC_ID、SET 键 = BUTTON_SET_ID）。
 *   - 事件型号（`periph_service_event_t.type`）与 `input_key_service` 的动作枚举数值一致：
 *     PRESSED=1/CLICK、RELEASE=2/CLICK_RELEASE、LONG_PRESSED=3/PRESS、LONG_RELEASE=4/PRESS_RELEASE。
 *   - SET 键的长按（≥ 阈值）**不绑定任何动作**：将来若要复用（如恢复出厂/清空对话），
 *     在此处按 PRESS/PRESS_RELEASE 另加分支即可，不要改单击的语义（用户已知「SET 单击 = 新对话」）。
 */

#ifndef _KEY_TALK_H_
#define _KEY_TALK_H_

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_peripherals.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void (*on_press_down)(void *ctx);               /* REC 按下瞬间 */
    void (*on_talk_start)(void *ctx);               /* REC 长按达标：开始收音 */
    void (*on_talk_stop)(uint32_t held_ms, void *ctx); /* REC 松开：提交本轮 */
    void (*on_short_press)(void *ctx);              /* REC 短按（<阈值）：打断/忽略 */
    void (*on_new_conversation)(void *ctx);         /* SET 单击：请求开新对话（conv.new） */
    void *ctx;
} key_talk_cbs_t;

typedef struct {
    bool     pressed;       /* 当前是否按住（REC） */
    bool     talking;       /* 是否已达阈值、正在收音 */
    uint32_t last_held_ms;  /* 最近一次按住时长 */
    uint32_t talk_count;    /* REC 达标次数（累计） */
    uint32_t short_count;   /* REC 短按次数（累计） */
    uint32_t new_conv_count;/* SET 单击起新对话次数（累计） */
} key_talk_status_t;

/** 创建 ADC 按键外设 + input_key_service，并注册回调（可重复调用：已初始化则跳过） */
esp_err_t key_talk_init(esp_periph_set_handle_t set, const key_talk_cbs_t *cbs);

bool key_talk_is_talking(void);
void key_talk_get_status(key_talk_status_t *out);

#ifdef __cplusplus
}
#endif

#endif /* _KEY_TALK_H_ */
