/*
 * buzzer_drv.c —— 无源蜂鸣器 PWM 驱动
 *
 * 用 LEDC 产生 ~2.7 kHz 方波；两声短鸣由独立任务完成，不阻塞 T31 上报任务。
 */
#include "sdkconfig.h"

#include "buzzer_drv.h"

#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "buzzer";

#define BUZZER_LEDC_TIMER     LEDC_TIMER_0
#define BUZZER_LEDC_CHANNEL   LEDC_CHANNEL_0
#define BUZZER_LEDC_RES       LEDC_TIMER_10_BIT
/* 10-bit 满量程 1023；直接 GPIO 驱动时 30% 占空比限制平均电流/发热。 */
#define BUZZER_DUTY_ON        ((1023u * CONFIG_ONEYE_BEDRAIL_BUZZER_DUTY_PCT) / 100u)

static bool s_ready;
static volatile bool s_busy;

static void buzzer_tone(bool on)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BUZZER_LEDC_CHANNEL, on ? BUZZER_DUTY_ON : 0u);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BUZZER_LEDC_CHANNEL);
}

static void buzzer_alert_task(void *arg)
{
    (void)arg;

    buzzer_tone(true);
    vTaskDelay(pdMS_TO_TICKS(120));
    buzzer_tone(false);
    vTaskDelay(pdMS_TO_TICKS(100));
    buzzer_tone(true);
    vTaskDelay(pdMS_TO_TICKS(120));
    buzzer_tone(false);

    s_busy = false;
    vTaskDelete(NULL);
}

esp_err_t buzzer_drv_init(int gpio_num)
{
    if (gpio_num < 0) {
        s_ready = false;
        return ESP_OK;
    }
    if (gpio_num > 48) {
        ESP_LOGE(TAG, "GPIO%d 非法（期望 0..48）", gpio_num);
        return ESP_ERR_INVALID_ARG;
    }

    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = BUZZER_LEDC_RES,
        .timer_num = BUZZER_LEDC_TIMER,
        .freq_hz = CONFIG_ONEYE_BEDRAIL_BUZZER_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LEDC timer 配置失败：%s", esp_err_to_name(err));
        return err;
    }

    ledc_channel_config_t ch = {
        .gpio_num = gpio_num,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = BUZZER_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = BUZZER_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
        .sleep_mode = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD,
        .flags.output_invert = 0,
    };
    err = ledc_channel_config(&ch);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LEDC channel(GPIO%d) 配置失败：%s", gpio_num, esp_err_to_name(err));
        return err;
    }

    s_ready = true;
    ESP_LOGI(TAG, "无源蜂鸣器初始化完成：GPIO%d，%d Hz，两声短鸣=解锁",
             gpio_num, CONFIG_ONEYE_BEDRAIL_BUZZER_FREQ_HZ);
    return ESP_OK;
}

void buzzer_drv_unlock_alert(void)
{
    if (!s_ready || s_busy) {
        return;
    }

    s_busy = true;
    if (xTaskCreate(buzzer_alert_task, "buzzer", 2048, NULL, 3, NULL) != pdPASS) {
        s_busy = false;
        ESP_LOGW(TAG, "蜂鸣器任务创建失败 ⇒ 本次不响");
    }
}
