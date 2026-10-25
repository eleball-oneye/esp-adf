/*
 * board_key.c —— Korvo-2（DU1906）板载 **ADC 按键**：长按任意键 ≥3 s = 忘记 WiFi + 重启（重新配网）
 *
 * 为什么需要它（2026-10-23 用户逐字）：
 *   「需要板子上有按键触发进入配网广播流程」「korvo2 的板子有 RESET 和 SET 键可用」
 *   —— 本工程原先只等 GPIO0(BOOT)，而 Korvo-2 没有 BOOT 键：它的 4 个键
 *   （VOLUP / VOLDOWN / MUTE / SET，`components/audio_board/esp32_korvo_du1906/board_def.h:84-116`）
 *   挂在 **ADC 电阻梯**上（`ADC_DETECT_GPIO = GPIO_NUM_2`，ADC1 通道 1）。
 *
 * 口径（刻意最简，⛔ 不引入 esp-adf 的 audio_board / input_key_service 依赖）：
 *   · 只判「**有没有键被按下**」：原始 ADC 读数低于阈值即视为按下（电阻梯按下 = 分压下降）；
 *   · 长按 ≥ `BOARD_KEY_LONG_PRESS_MS`（3 s）→ `wifi_prov_forget_wifi()` + `esp_restart()`；
 *   · 短按不做事；按键期间每 500 ms 打一次原始读数（便于换板/换电阻梯时校准阈值）。
 *   ⚠️ 本文件只服务「台面联调」；量产按键语义（哪个键做什么）不在本批范围。
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

#include "wifi_prov.h"
#include "board_key.h"

static const char *TAG = "boardkey";

/* Korvo-2 **v3**（ESP32-S3-Korvo-2 v3）的 ADC 按键脚：
 *   `components/audio_board/esp32_s3_korvo2_v3/board.c:157-163` 实测配置 =
 *   `ADC_UNIT_1 / ADC_CHANNEL_4`（电平档 `{190,600,1000,1375,1775,2195,3000}`，6 个键）。
 *   ESP32-S3 上 ADC1_CH4 = **GPIO4**。
 * ⚠️ 2026-10-23 首烧教训（旧值逐字留痕，⛔ 不抹）：旧值 = `ADC_CHANNEL_1`（GPIO2）——
 *   那是 LCD_DC，不是按键脚 ⇒ 读到的是恒定的 ~200 原始值，导致「永远判按下」自重启。 */
#define BOARD_KEY_ADC_UNIT       ADC_UNIT_1
#define BOARD_KEY_ADC_CHANNEL    ADC_CHANNEL_4

/* 原始读数**变化量**阈值：相对开机基线（前 20 次采样均值）偏移超过它 = 有键按下。
 * ⚠️ 2026-10-23 首烧实测：Korvo-2 本板 GPIO2 的**空闲原始读数 ≈200**（不是满量程 4095），
 *   所以「低于固定阈值」这种判据会**永远判按下** ⇒ 板子每次上电 3 s 就自我重启（死循环）。
 *   改成「相对基线偏移」判据 + 每次按下把当前 raw 打到串口，便于换板后重新定阈值。 */
#define BOARD_KEY_DELTA          400

#define BOARD_KEY_SAMPLE_MS      50
#define BOARD_KEY_BASELINE_N     20
#define BOARD_KEY_LONG_PRESS_MS  3000
#define BOARD_KEY_LOG_EVERY_MS   500

static adc_oneshot_unit_handle_t s_adc;
static TaskHandle_t s_task;

static int board_key_read_raw(void)
{
    int raw = 0;
    if (s_adc == NULL) {
        return -1;
    }
    if (adc_oneshot_read(s_adc, BOARD_KEY_ADC_CHANNEL, &raw) != ESP_OK) {
        return -1;
    }
    return raw;
}

static void board_key_task(void *arg)
{
    (void)arg;
    bool pressed = false;
    int64_t press_start_us = 0;
    int64_t last_log_us = 0;
    int64_t last_raw_log_us = 0;
    int baseline = -1;

    /* 先取基线：上电时没有任何键被按下（前 N 次采样均值）。 */
    {
        int64_t sum = 0;
        int n = 0;
        for (int i = 0; i < BOARD_KEY_BASELINE_N; i++) {
            int raw = board_key_read_raw();
            if (raw >= 0) {
                sum += raw;
                n++;
            }
            vTaskDelay(pdMS_TO_TICKS(BOARD_KEY_SAMPLE_MS));
        }
        baseline = (n > 0) ? (int)(sum / n) : -1;
        ESP_LOGI(TAG, "按键基线 raw=%d（偏移 > %d 视为按下）", baseline, BOARD_KEY_DELTA);
    }

    while (true) {
        int raw = board_key_read_raw();
        int64_t now_us = esp_timer_get_time();

        /* 每 2 s 打一次原始读数（换板/换电阻梯时用它校准基线与 Δ 阈值）。 */
        if ((now_us - last_raw_log_us) / 1000 >= 2000) {
            last_raw_log_us = now_us;
            ESP_LOGI(TAG, "[diag] 按键 raw=%d（基线=%d，Δ=%+d）", raw, baseline,
                     (baseline >= 0 && raw >= 0) ? (raw - baseline) : 0);
        }

        if (raw >= 0) {
            int delta = (baseline >= 0) ? (raw - baseline) : 0;
            bool now_pressed = (baseline >= 0) && (delta > BOARD_KEY_DELTA || -delta > BOARD_KEY_DELTA);
            if (now_pressed != pressed) {
                pressed = now_pressed;
                ESP_LOGW(TAG, "按键%s（raw=%d，基线=%d，Δ=%+d）", pressed ? "按下" : "松开", raw,
                         baseline, delta);
                if (pressed) {
                    press_start_us = now_us;
                    last_log_us = now_us;
                }
            }
            if (pressed) {
                if ((now_us - last_log_us) / 1000 >= BOARD_KEY_LOG_EVERY_MS) {
                    last_log_us = now_us;
                    ESP_LOGW(TAG, "长按中：raw=%d Δ=%+d 已按 %lld ms / 触发阈值 %d ms", raw, delta,
                             (long long)((now_us - press_start_us) / 1000),
                             BOARD_KEY_LONG_PRESS_MS);
                }
                if ((now_us - press_start_us) / 1000 >= BOARD_KEY_LONG_PRESS_MS) {
                    ESP_LOGW(TAG, "★ 长按 ≥ %d ms ⇒ 忘记 WiFi 并重启（重新进入配网广播）",
                             BOARD_KEY_LONG_PRESS_MS);
                    esp_err_t rc = wifi_prov_forget_wifi("board-key-long-press");
                    ESP_LOGW(TAG, "wifi_prov_forget_wifi rc=%d(%s) ⇒ 1 s 后重启",
                             (int)rc, esp_err_to_name(rc));
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    esp_restart();
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(BOARD_KEY_SAMPLE_MS));
    }
}
esp_err_t board_key_start(void)
{
    adc_oneshot_unit_init_cfg_t unit_cfg = { .unit_id = BOARD_KEY_ADC_UNIT };
    esp_err_t rc = adc_oneshot_new_unit(&unit_cfg, &s_adc);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "ADC 单元创建失败 rc=%d(%s) ⇒ 按键触发不可用（其它配网入口照常）",
                 (int)rc, esp_err_to_name(rc));
        return rc;
    }
    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    rc = adc_oneshot_config_channel(s_adc, BOARD_KEY_ADC_CHANNEL, &chan_cfg);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "ADC 通道配置失败 rc=%d(%s)", (int)rc, esp_err_to_name(rc));
        adc_oneshot_del_unit(s_adc);
        s_adc = NULL;
        return rc;
    }
    int first = board_key_read_raw();
    ESP_LOGI(TAG, "板载 ADC 按键已启动：GPIO4/ADC1_CH4，采样 %d ms，长按 %d ms 触发重新配网；"
                  "当前 raw=%d（相对开机基线偏移 > %d 视为按下）",
             BOARD_KEY_SAMPLE_MS, BOARD_KEY_LONG_PRESS_MS, first, BOARD_KEY_DELTA);
    if (xTaskCreate(board_key_task, "board_key", 3072, NULL, 3, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "按键任务创建失败（内存不足）");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
