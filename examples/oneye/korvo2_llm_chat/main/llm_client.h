/*
 * llm_client.h —— 云端语音面客户端（契约：backend/contracts/api/ws/语音对话.md，协议 oneye.voice.v1）
 *
 * 面口径（与「本地面 link 帧」严格区分，不得混用）：
 *   - 承载：`wss://<host>:9091/v1/voice/ws`（dev 可 `ws://`），子协议 **oneye.voice.v1**；
 *   - 文本帧：统一信封 `{"v":1,"t":"<type>","p":{…}}`；15 类帧见契约 voice-frames.json；
 *   - 二进制帧：首字节 kind —— `0x01` 上行音频、`0x02` 下行音频，其后为 PCM
 *     （16 kHz / 16 bit / 单声道 / 60 ms = 1920 B）；
 *   - 会话：`session.start` → `session.ready` → `conv.state`（当前对话组）；
 *   - 对话组（契约 §12）：服务端按设备 SN 存**加密**对话历史，**每设备最多 10 段、每段不限轮数**；
 *     会话就绪后自动**接着最近活动的那段**继续；要开新段必须显式发 `conv.new`
 *     （设备侧 = **SET 键单击**，见 `llm_client_new_conversation()`）；
 *   - 轮次：上行音频 → `input.audio.commit` → asr.* → llm.delta → tts.start → 0x02… → tts.end → turn.end；
 *     说话中/思考中可 `input.cancel` 打断（服务端保证 ≤200 ms 停止下行音频）。
 *
 * 本模块**只管协议与连接**：音频采集/回放在 voice_io，按键在 key_talk。
 * 回调均在 WebSocket 任务上下文执行：**禁止阻塞**（播放写失败按丢弃处理，见 voice_io）。
 */

#ifndef _LLM_CLIENT_H_
#define _LLM_CLIENT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * URI scheme（wss 为生产形态，ws 仅台面/同网段）。
 * 注意：ESP-IDF 的 bool Kconfig 在**未置位时不会定义** `CONFIG_xxx` 宏，
 * 因此不能用 `CONFIG_ONEYE_LLM_USE_TLS ? "wss" : "ws"`（编译期即报未声明）——
 * 统一用下面的宏。
 */
#if CONFIG_ONEYE_LLM_USE_TLS
#define LLM_CLIENT_URI_SCHEME "wss"
#else
#define LLM_CLIENT_URI_SCHEME "ws"
#endif

/* 设备侧本地状态（服务端状态机 READY/LISTENING/THINKING/SPEAKING 的镜像 + 连接态） */
typedef enum {
    LLM_CLIENT_IDLE = 0,      /* 未连接/已断开 */
    LLM_CLIENT_CONNECTING,    /* 正在握手 */
    LLM_CLIENT_HANDSHAKING,   /* socket 已连，等待 session.ready */
    LLM_CLIENT_READY,         /* 可开始新一轮（idle，允许长按） */
    LLM_CLIENT_LISTENING,     /* 正在上行本轮音频 */
    LLM_CLIENT_THINKING,      /* 已 commit，等待/接收下行 */
    LLM_CLIENT_SPEAKING,      /* 正在播放下行音频 */
    LLM_CLIENT_ERROR,         /* 最近一次为不可恢复错误（等待重连） */
} llm_client_state_t;

typedef struct {
    void (*on_state)(llm_client_state_t st, const char *detail, void *ctx);
    void (*on_audio_down)(const void *pcm, size_t len, void *ctx);
    void (*on_tts_start)(void *ctx);
    void (*on_tts_end)(void *ctx);
    /* asr.partial / asr.final / llm.delta 的文本（frame_t 给出具体帧名） */
    void (*on_text)(const char *frame_t, const char *text, void *ctx);
    void (*on_turn_end)(int turn_seq, bool cancelled, int rtt_ms, void *ctx);
    void (*on_error)(const char *code, const char *msg, bool retryable, void *ctx);
    /* conv.state（契约 §12.1）：当前对话组——
     * reason = "ready"（会话就绪接上的段）/ "new"（conv.new 已建新段）/ "switched"（控制台换了模型）。
     * 设备无屏，此回调是"现在接着哪段聊、用的是哪个模型"的唯一可见入口。 */
    void (*on_conv_state)(const char *reason, int64_t conv_id, int turns,
                          const char *profile_id, void *ctx);
    void *ctx;
} llm_client_cbs_t;

typedef struct {
    llm_client_state_t state;
    bool     connected;        /* WebSocket 已连接 */
    bool     session_ready;    /* 收到过 session.ready */
    uint32_t up_frames;        /* 已发送的 0x01 帧数 */
    uint32_t up_bytes;         /* 上行 PCM 字节 */
    uint32_t down_bytes;       /* 下行 PCM 字节 */
    uint32_t turns;            /* 完成轮次数 */
    uint32_t cancels;          /* 打断次数 */
    uint32_t errors;           /* 收到的 error 帧数 */
    uint32_t reconnects;       /* 重连次数 */
    uint32_t new_convs;        /* 已请求的新对话段数（SET 键单击） */
    int      last_turn_seq;
    int      last_rtt_ms;
    int64_t  conv_id;          /* 当前对话段 id；0 = 未知/未绑定 */
    int      conv_turns;       /* 当前段已有轮数（服务端回执） */
    char     conv_profile[40]; /* 当前段生效的模型档位（服务端回执，可空） */
    char     session_id[40];
} llm_client_status_t;

/** 初始化（不连接）。uri 为 `ws://host:port/path` 或 `wss://…`；token 可为 ""。 */
esp_err_t llm_client_init(const llm_client_cbs_t *cbs, const char *uri, const char *device_id,
                          const char *token);

/** 启动并连接（连上后自动发 `session.start`）；幂等 */
esp_err_t llm_client_start(void);

/** 断开并释放；幂等 */
esp_err_t llm_client_stop(void);

bool llm_client_is_connected(void);
bool llm_client_is_session_ready(void);
llm_client_state_t llm_client_get_state(void);
void llm_client_get_status(llm_client_status_t *out);

/** 上行一段 PCM（内部按 1920 B 分帧，首字节 kind = 0x01）；未就绪返回 ESP_ERR_INVALID_STATE */
esp_err_t llm_client_audio_up(const void *pcm, size_t len);

/** 本轮说完（发 `input.audio.commit`，并冲刷不足一帧的尾包） */
esp_err_t llm_client_commit(void);

/** 打断/放弃本轮（发 `input.cancel`） */
esp_err_t llm_client_cancel(const char *reason);

/** 保活探测（发 `ping`，服务端回 `pong`） */
esp_err_t llm_client_ping(void);

/**
 * 起一段新对话（发 `conv.new`；设备侧 = SET 键单击）。
 *
 * 语义（契约 §12.1）：
 *   - 服务端建新段并回 `conv.state{reason:"new"}`；此后的轮次计入新段，旧段内容保留；
 *   - 本轮进行中（THINKING/SPEAKING）服务端回 `error{code:"conflict"}` —— 此时请先 `llm_client_cancel()`；
 *   - 新段**不继承**旧段上下文（"开新话题"就该从零开始）；
 *   - 未连上/未 session.ready 时返回 ESP_ERR_INVALID_STATE（不排队，避免"按键没反应却以为生效了"）。
 */
esp_err_t llm_client_new_conversation(void);

#ifdef __cplusplus
}
#endif

#endif /* _LLM_CLIENT_H_ */
