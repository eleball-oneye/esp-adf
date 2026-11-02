/*
 * hall_drv.c —— DRV5032FB 数字霍尔驱动实现
 * =============================================================================
 * 只做"配 GPIO + 读电平"，不做去抖/上报；去抖在 t31_sim.c 的周期任务里
 * （连续 3 次同电平才算稳定）。
 * =============================================================================
 */

#include "sdkconfig.h"

#include "hall_drv.h"

#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "hall";

static int s_gpio = -1;
static bool s_ready = false;

esp_err_t hall_drv_init(int gpio_num)
{
    if (gpio_num < 0 || gpio_num > 48) {
        ESP_LOGE(TAG, "GPIO%d 非法（期望 0..48）", gpio_num);
        return ESP_ERR_INVALID_ARG;
    }

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << (unsigned)gpio_num,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config(GPIO%d) 失败：%s", gpio_num, esp_err_to_name(err));
        return err;
    }

    s_gpio = gpio_num;
    s_ready = true;
    ESP_LOGI(TAG, "DRV5032FB 初始化完成：OUT=GPIO%d（推挽/全极/5 Hz，%s）",
             gpio_num,
             CONFIG_ONEYE_BEDRAIL_HALL_ACTIVE_LOW ? "低电平=有磁" : "高电平=有磁");
    return ESP_OK;
}

hall_state_t hall_drv_read(void)
{
    if (s_gpio < 0) {
        return HALL_STATE_OPEN;
    }

    int level = gpio_get_level((gpio_num_t)s_gpio);
#if CONFIG_ONEYE_BEDRAIL_HALL_ACTIVE_LOW
    bool magnet = (level == 0);
#else
    bool magnet = (level != 0);
#endif
    return magnet ? HALL_STATE_CLOSED : HALL_STATE_OPEN;
}

const char *hall_state_name(hall_state_t state)
{
    return (state == HALL_STATE_CLOSED) ? "闭合/有磁" : "打开/无磁";
}

bool hall_drv_ready(void)
{
    return s_ready;
}