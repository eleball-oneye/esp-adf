/*
 * board_key.h —— Korvo-2（DU1906）板载 ADC 按键触发重新配网（见 board_key.c 头注释）
 */
#ifndef BOARD_KEY_H
#define BOARD_KEY_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 启动按键监测任务；无 ADC 的板型调用会返回错误但不影响其它功能。 */
esp_err_t board_key_start(void);

#ifdef __cplusplus
}
#endif

#endif /* BOARD_KEY_H */
