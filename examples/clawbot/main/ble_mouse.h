#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void (*on_connect)(bool connected);
} ble_mouse_callbacks_t;

void ble_mouse_init(ble_mouse_callbacks_t cb);
bool ble_mouse_connected(void);
void ble_mouse_send(uint8_t buttons, int16_t dx, int16_t dy, int8_t wheel);
void ble_mouse_set_battery(uint8_t level);
void ble_mouse_stop_advertising(void);
void ble_mouse_start_advertising(void);

#ifdef __cplusplus
}
#endif
