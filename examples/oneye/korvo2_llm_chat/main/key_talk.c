/*
 * key_talk.c —— REC 键长按说话（ADC 按键 + input_key_service）
 *
 * 见 key_talk.h 头注：本模块用**自定义 press_judge_time** 的 ADC 按键外设，
 * 因此**不得**再调用 `audio_board_key_init()`（会在同一 ADC 通道上装第二份按键外设）。
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "board.h"
#include "esp_peripherals.h"
#include "periph_adc_button.h"
#include "input_key_service.h"
#include "audio_mem.h"

#include "key_talk.h"

static const char *TAG = "key_talk";

/* 与 input_key_service 的动作枚举**数值一致**的按键事件（见 periph_adc_button.h） */
#define KEY_EV_CLICK          1 /* = INPUT_KEY_SERVICE_ACTION_CLICK（按下） */
#define KEY_EV_CLICK_RELEASE  2 /* = …_CLICK_RELEASE（短按松开） */
#define KEY_EV_PRESS          3 /* = …_PRESS（长按达标） */
#define KEY_EV_PRESS_RELEASE  4 /* = …_PRESS_RELEASE（长按后松开） */

/* ADC 电平阶梯与通道：逐项照抄 components/audio_board/esp32_s3_korvo2_v3/board.c:158-162。
 * 用 static 保存：外设若保留指针，生命周期也安全。 */
static int s_key_levels[7] = { 190, 600, 1000, 1375, 1775, 2195, 3000 };
static adc_arr_t s_key_arr;
static periph_adc_button_cfg_t s_btn_cfg;

static key_talk_cbs_t    s_cbs;
static key_talk_status_t s_st;
static int64_t           s_press_us;
static periph_service_handle_t s_key_srv;
static esp_periph_handle_t     s_btn_periph;

static esp_err_t key_cb(periph_service_handle_t handle, periph_service_event_t *evt, void *ctx)
{
    (void)handle;
    (void)ctx;
    if (evt == NULL) {
        return ESP_OK;
    }
    int user_id = (int)(intptr_t)evt->data;
    int action = (int)evt->type;
    if (user_id != INPUT_KEY_USER_ID_REC) {
        return ESP_OK; /* 其他键（VOL/MUTE/PLAY/SET）本工程暂不处理 */
    }

    switch (action) {
    case KEY_EV_CLICK: /* 按下瞬间 */
        s_press_us = esp_timer_get_time();
        s_st.pressed = true;
        ESP_LOGI(TAG, "REC 按下");
        if (s_cbs.on_press_down) {
            s_cbs.on_press_down(s_cbs.ctx);
        }
        break;

    case KEY_EV_PRESS: { /* 长按达标 → 开始收音 */
        uint32_t held = (uint32_t)((esp_timer_get_time() - s_press_us) / 1000);
        s_st.talking = true;
        s_st.talk_count++;
        s_press_us = esp_timer_get_time(); /* 以达标时刻为本次说话起点计时 */
        /* 注意取值口径（真机取证 2026-09-17）：`held` 是**自本模块收到"按下"事件起算**的时间，
         * 而"长按达标"由 `input_key_service` 按其**自身计时**判定。我们的起点晚于真实按下时刻
         * （差一个 ADC 扫描周期 + 事件投递延迟），故这里打印的值可能**略小于**阈值
         * （实测 579 ms vs 阈值 600 ms），属正常误差，不代表阈值失效。 */
        ESP_LOGI(TAG, "长按达标（自按下事件起算 %u ms，阈值 %d ms；差值 = ADC 扫描/事件投递延迟）"
                      "→ 开始收音",
                 (unsigned)held, CONFIG_ONEYE_LLM_TALK_MIN_PRESS_MS);
        if (s_cbs.on_talk_start) {
            s_cbs.on_talk_start(s_cbs.ctx);
        }
        break;
    }

    case KEY_EV_PRESS_RELEASE: { /* 长按后松开 → 提交本轮 */
        uint32_t held = (uint32_t)((esp_timer_get_time() - s_press_us) / 1000);
        bool was_talking = s_st.talking;
        s_st.talking = false;
        s_st.pressed = false;
        s_st.last_held_ms = held;
        if (!was_talking) {
            return ESP_OK;
        }
        ESP_LOGI(TAG, "REC 松开（按住 %u ms）→ 提交本轮", (unsigned)held);
        if (s_cbs.on_talk_stop) {
            s_cbs.on_talk_stop(held, s_cbs.ctx);
        }
        break;
    }

    case KEY_EV_CLICK_RELEASE: { /* 未达阈值的短按 */
        s_st.pressed = false;
        s_st.talking = false;
        s_st.short_count++;
        ESP_LOGI(TAG, "短按（< %d ms）→ 视为误触/打断", CONFIG_ONEYE_LLM_TALK_MIN_PRESS_MS);
        if (s_cbs.on_short_press) {
            s_cbs.on_short_press(s_cbs.ctx);
        }
        break;
    }

    default:
        break;
    }
    return ESP_OK;
}

esp_err_t key_talk_init(esp_periph_set_handle_t set, const key_talk_cbs_t *cbs)
{
    if (set == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
#if CONFIG_ONEYE_LLM_ENABLE_KEYS
    if (cbs) {
        s_cbs = *cbs;
    }
    if (s_btn_periph != NULL) {
        return ESP_OK; /* 幂等 */
    }

    s_key_arr = (adc_arr_t)ADC_DEFAULT_ARR();
    s_key_arr.total_steps = 6;
    s_key_arr.adc_ch = ADC_CHANNEL_4;          /* 板级口径（board.c:160） */
    s_key_arr.adc_level_step = s_key_levels;   /* 板级口径（board.c:161） */
    s_key_arr.press_judge_time = CONFIG_ONEYE_LLM_TALK_MIN_PRESS_MS; /* 覆写板级缺省 3000 ms */

    s_btn_cfg = (periph_adc_button_cfg_t)PERIPH_ADC_BUTTON_DEFAULT_CONFIG();
    s_btn_cfg.arr = &s_key_arr;
    s_btn_cfg.arr_size = 1;
    if (audio_mem_spiram_stack_is_enabled()) {
        s_btn_cfg.task_cfg.ext_stack = true;
    }

    s_btn_periph = periph_adc_button_init(&s_btn_cfg);
    if (s_btn_periph == NULL) {
        ESP_LOGE(TAG, "ADC 按键外设创建失败");
        return ESP_FAIL;
    }
    esp_err_t err = esp_periph_start(set, s_btn_periph);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADC 按键外设启动失败：%s", esp_err_to_name(err));
        s_btn_periph = NULL;
        return err;
    }

    /* 复用同一个外设：由 input_key_service 把 act_id 映射为 user_id 并分发动作 */
    input_key_service_info_t key_info[] = INPUT_KEY_DEFAULT_INFO();
    input_key_service_cfg_t key_cfg = INPUT_KEY_SERVICE_DEFAULT_CONFIG();
    key_cfg.handle = set;
    key_cfg.based_cfg.task_stack = 4 * 1024;
    s_key_srv = input_key_service_create(&key_cfg);
    if (s_key_srv == NULL) {
        ESP_LOGE(TAG, "input_key_service 创建失败");
        return ESP_FAIL;
    }
    (void)input_key_service_add_key(s_key_srv, key_info, INPUT_KEY_NUM);
    (void)periph_service_set_callback(s_key_srv, key_cb, NULL);

    ESP_LOGI(TAG, "REC 键就绪：长按 ≥ %d ms 开始收音，松开提交（短按 = 打断/忽略）",
             CONFIG_ONEYE_LLM_TALK_MIN_PRESS_MS);
    return ESP_OK;
#else
    (void)cbs;
    ESP_LOGW(TAG, "按键已由 Kconfig 关闭（CONFIG_ONEYE_LLM_ENABLE_KEYS=n）");
    return ESP_OK;
#endif
}

bool key_talk_is_talking(void)
{
    return s_st.talking;
}

void key_talk_get_status(key_talk_status_t *out)
{
    if (out) {
        *out = s_st;
    }
}
