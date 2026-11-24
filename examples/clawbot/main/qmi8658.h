#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

#define QMI8658_ADDR_SA0_LOW   0x6B
#define QMI8658_ADDR_SA0_HIGH  0x6A

typedef struct {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t dev;
    uint8_t addr;
    bool ready;
} qmi8658_t;

typedef struct {
    int16_t ax, ay, az;   /* raw LSB, ±4 g full scale */
    int16_t gx, gy, gz;   /* raw LSB, ±512 dps full scale */
} qmi8658_raw_t;

typedef struct {
    float ax, ay, az;     /* g */
    float gx, gy, gz;     /* dps */
} qmi8658_sample_t;

esp_err_t qmi8658_init(qmi8658_t *imu, int sda_gpio, int scl_gpio);
esp_err_t qmi8658_read_raw(qmi8658_t *imu, qmi8658_raw_t *raw);
void      qmi8658_to_sample(const qmi8658_raw_t *raw, qmi8658_sample_t *out);

#ifdef __cplusplus
}
#endif
