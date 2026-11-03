/*
 * buzzer_drv.h —— 无源蜂鸣器（2 脚）解锁提示
 *
 * 接线（推荐，NPN 低边驱动）：
 *   GPIO3 ── 1kΩ ── NPN 基极(B)
 *   NPN 发射极(E) ── GND
 *   NPN 集电极(C) ── 蜂鸣器负极
 *   蜂鸣器正极 ── 3V3
 *   电磁式蜂鸣器建议在蜂鸣器两端并联反向续流二极管（负极接 3V3）。
 *
 * 无源蜂鸣器必须用 PWM 驱动；本驱动固定 ~2.7 kHz、50% 占空比。
 */
#ifndef BUZZER_DRV_H
#define BUZZER_DRV_H

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化 LEDC PWM；gpio_num < 0 表示关闭。 */
esp_err_t buzzer_drv_init(int gpio_num);

/* 请求一次“两声短鸣”：120 ms 响、100 ms 停、120 ms 响。
 * 非阻塞；正在鸣叫时重复请求会被忽略。 */
void buzzer_drv_unlock_alert(void);

#ifdef __cplusplus
}
#endif

#endif /* BUZZER_DRV_H */
