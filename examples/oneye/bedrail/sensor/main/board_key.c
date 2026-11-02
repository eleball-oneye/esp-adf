/*
 * board_key.c —— 板载按键：**长按 ≥3 s = 忘记 WiFi + 重启（重新进入 BLE 配网流程）**
 *
 * 用户口径（逐字留痕，⛔ 不删）：
 *   「需要板子上有按键触发进入配网广播流程」「korvo2 的板子有 RESET 和 SET 键可用」
 *   「C6-GEEK板上的按键需要支持长按进入蓝牙配网流程」（2026-10-25）
 *
 * 两块目标板的按键**形态完全不同**，所以本文件按 target 分两条实现（同一条语义）：
 *
 *   ① ESP32-S3（Korvo-2 v3，COM12 联调板）= **ADC 电阻梯**
 *      VOLUP / VOLDOWN / MUTE / SET 四键挂在 ADC 上（`ADC_DETECT_GPIO = GPIO_NUM_2`，
 *      `components/audio_board/esp32_korvo_du1906/board_def.h:84-116`）。
 *      Korvo-2 **没有 BOOT 键** ⇒ 不能等 GPIO0。
 *      ESP32-S3 上 ADC1_CH4 = **GPIO4**（`esp32_s3_korvo2_v3/board.c:157-163` 实测配置）。
 *      ⚠️ 2026-10-23 首烧教训（旧值逐字留痕，⛔ 不抹）：旧值 = `ADC_CHANNEL_1`（GPIO2）——
 *        那是 LCD_DC，不是按键脚 ⇒ 读到恒定 ~200，导致「永远判按下」自重启。
 *      ⚠️ 2026-10-23 首烧实测：Korvo-2 本板空闲原始读数 ≈200（不是满量程 4095）⇒ 判据改成
 *        「相对开机基线的偏移」而不是「低于固定阈值」（否则每次上电 3 s 就自我重启）。
 *
 *   ② ESP32-C6（微雪 ESP32-C6-GEEK，网关目标板）= **BOOT 键（GPIO9，低有效、10 kΩ 上拉）**
 *      🔴 为什么必须换脚（2026-10-25）：C6-GEEK 的 **GPIO4 是 LCD 的 RST**（官方 IDF demo
 *        `06_lvgl_example_v9/main/LCD_Driver/lcd_st7789.h` 实测引脚表），继续用 ADC1_CH4(GPIO4)
 *        会与屏幕复位脚打架 ⇒ 本 target 改**纯 GPIO 轮询**，不碰 ADC。
 *      GPIO9 同时是 strapping 脚（复位时低电平 = 进下载模式）⇒ **只在启动完成后读**。
 *
 * 口径（两块板一致，刻意最简，⛔ 不引入 esp-adf 的 audio_board / input_key_service 依赖）：
 *   · 只判「有没有键被按下」；
 *   · 长按 ≥ `BOARD_KEY_LONG_PRESS_MS`（3 s）→ `wifi_prov_forget_wifi()` + `esp_restart()`；
 *   · 短按不做事；按住期间每 500 ms 打一行日志（便于换板/换电阻梯时校准阈值）。
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

#include "wifi_prov.h"
#include "board_key.h"

static const char *TAG = "boardkey";

#define BOARD_KEY_SAMPLE_MS      50
#define BOARD_KEY_LONG_PRESS_MS  3000
#define BOARD_KEY_LOG_EVERY_MS   500

static TaskHandle_t s_task;

/* ==========================================================================
 * ① ESP32-C6 / C6-GEEK：BOOT 键（GPIO9）纯 GPIO 轮询
 * ========================================================================== */
#if CONFIG_IDF_TARGET_ESP32C6
#include "driver/gpio.h"

/* C6-GEEK 的 BOOT 键 = GPIO9（模组 strapping 脚；按下为低，外部 10 kΩ 上拉） */
#define BOARD_KEY_GPIO GPIO_NUM_9

static void board_key_task(void *arg)
{
    (void)arg;
    bool pressed = false;
    int64_t press_start_us = 0;
    int64_t last_log_us = 0;
    int64_t last_state_log_us = 0;

    while (true) {
        int level = gpio_get_level(BOARD_KEY_GPIO);
        bool now_pressed = (level == 0);       /* 低有效 */
        int64_t now_us = esp_timer_get_time();

        if (now_pressed != pressed) {
            pressed = now_pressed;
            ESP_LOGW(TAG, "BOOT 键%s（GPIO%d level=%d）", pressed ? "按下" : "松开",
                     (int)BOARD_KEY_GPIO, level);
            if (pressed) {
                press_start_us = now_us;
                last_log_us = now_us;
            }
        }
        /* 每 2 s 一行「当前电平」，便于确认按键极性/焊接（⛔ 不刷屏） */
        if ((now_us - last_state_log_us) / 1000 >= 2000) {
            last_state_log_us = now_us;
            ESP_LOGI(TAG, "[diag] BOOT 键 level=%d（%s）", level, pressed ? "按住" : "未按");
        }
        if (pressed) {
            if ((now_us - last_log_us) / 1000 >= BOARD_KEY_LOG_EVERY_MS) {
                last_log_us = now_us;
                ESP_LOGW(TAG, "长按中：已按 %lld ms / 触发阈值 %d ms",
                         (long long)((now_us - press_start_us) / 1000), BOARD_KEY_LONG_PRESS_MS);
            }
            if ((now_us - press_start_us) / 1000 >= BOARD_KEY_LONG_PRESS_MS) {
                ESP_LOGW(TAG, "★ 长按 ≥ %d ms ⇒ 忘记 WiFi 并重启（下次启动进入 BLE 配网广播）",
                         BOARD_KEY_LONG_PRESS_MS);
                esp_err_t rc = wifi_prov_forget_wifi("board-key-long-press");
                ESP_LOGW(TAG, "wifi_prov_forget_wifi rc=%d(%s) ⇒ 1 s 后重启",
                         (int)rc, esp_err_to_name(rc));
                vTaskDelay(pdMS_TO_TICKS(1000));
                esp_restart();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(BOARD_KEY_SAMPLE_MS));
    }
}

esp_err_t board_key_start(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BOARD_KEY_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t rc = gpio_config(&cfg);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "BOOT 键 GPIO 配置失败 rc=%d(%s) ⇒ 按键触发不可用（其它配网入口照常）",
                 (int)rc, esp_err_to_name(rc));
        return rc;
    }
    ESP_LOGI(TAG, "板载 BOOT 按键已启动：GPIO%d（低有效），采样 %d ms，长按 %d ms = 忘记 WiFi + 重启"
                  "（重新进入 BLE 配网流程）；当前 level=%d",
             (int)BOARD_KEY_GPIO, BOARD_KEY_SAMPLE_MS, BOARD_KEY_LONG_PRESS_MS,
             gpio_get_level(BOARD_KEY_GPIO));
    if (xTaskCreate(board_key_task, "board_key", 3072, NULL, 3, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "按键任务创建失败（内存不足）");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* ==========================================================================
 * ② ESP32-S3 / Korvo-2：ADC 电阻梯（原实现，逐字保留）
 * ========================================================================== */
#else
#include "esp_adc/adc_oneshot.h"

/* Korvo-2 **v3**（ESP32-S3-Korvo-2 v3）的 ADC 按键脚：
 *   `components/audio_board/esp32_s3_korvo2_v3/board.c:157-163` 实测配置 =
 *   `ADC_UNIT_1 / ADC_CHANNEL_4`（电平档 `{190,600,1000,1375,1775,2195,3000}`，6 个键）。
 *   ESP32-S3 上 ADC1_CH4 = **GPIO4**。
 * ⚠️ 2026-10-23 首烧教训（旧值逐字留痕，⛔ 不抹）：旧值 = `ADC_CHANNEL_1`（GPIO2）——
 *   那是 LCD_DC，不是按键脚 ⇒ 读到的是恒定的 ~200 原始值，导致「永远判按下」自重启。 */
#define BOARD_KEY_ADC_UNIT       ADC_UNIT_1
#define BOARD_KEY_ADC_CHANNEL    ADC_CHANNEL_4

/* 原始读数**变化量**阈值：相对开机基线（前 20 次采样均值）偏移超过它 = 有键按下。
 * ⚠️ 2026-10-23 首烧实测：Korvo-2 本板 GPIO2（旧脚）空闲原始读数 ≈200（不是满量程 4095），
 *   所以「低于固定阈值」这种判据会**永远判按下** ⇒ 板子每次上电 3 s 就自我重启（死循环）。
 *   改成「相对基线偏移」判据 + 每次按下把当前 raw 打到串口，便于换板后重新定阈值。 */
#define BOARD_KEY_DELTA          400
#define BOARD_KEY_BASELINE_N     20

static adc_oneshot_unit_handle_t s_adc;

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
#endif /* CONFIG_IDF_TARGET_ESP32C6 */
