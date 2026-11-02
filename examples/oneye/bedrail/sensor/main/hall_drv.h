/*
 * hall_drv.h —— DRV5032FB 数字霍尔开关驱动（ESP32-C6-ZERO）
 * =============================================================================
 * 器件：DRV5032FBLPGM（TI，LPG = 3-pin TO-92）
 *   · VCC = 1.65–5.5 V（本板 3V3）；GND；OUT = 推挽、5 Hz 采样、全极
 *   · 磁场 |B| > BOP(4.8 mT max) ⇒ OUT 低；|B| < BRP ⇒ OUT 高
 *   · TO-92 顶视图（数据手册 Figure 5-5）：pin1=VCC, pin2=GND, pin3=OUT
 *
 * 语义（本工程唯一口径）：
 *   hallState = 1  有磁（OUT 低）  —— 磁铁靠近，机械上定义为"护栏闭合/拉起"
 *   hallState = 0  无磁（OUT 高）  —— 磁铁离开，机械上定义为"护栏打开/放下"
 *   ⚠️ 机械安装若相反，改 Kconfig 的 ONEYE_BEDRAIL_HALL_ACTIVE_LOW 即可，
 *      ⛔ 不要改云端物模型语义。
 * =============================================================================
 */

#ifndef HALL_DRV_H
#define HALL_DRV_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HALL_STATE_OPEN = 0,   /* 无磁：护栏打开/放下 */
    HALL_STATE_CLOSED = 1, /* 有磁：护栏闭合/拉起 */
} hall_state_t;

/*
 * 初始化 GPIO 为输入（内部上拉；DRV5032FB 推挽驱动，上拉只用于
 * 传感器未接/断线时给一个确定电平，避免悬空读数）。
 * @return ESP_OK 成功；gpio 非法或配置失败返回错误。
 */
esp_err_t hall_drv_init(int gpio_num);

/* 读一次原始电平并按有效电平换算成语义状态（不做去抖）。 */
hall_state_t hall_drv_read(void);

/* 驱动是否初始化成功（false ⇒ 调用方⛔ 不要编 hallState，避免把"没接"报成 0）。 */
bool hall_drv_ready(void);

/* 语义状态的中文/日志名（只读静态串，⛔ 不要 free）。 */
const char *hall_state_name(hall_state_t state);

#ifdef __cplusplus
}
#endif

#endif /* HALL_DRV_H */