#include "qmi8658.h"

#include <string.h>
#include "esp_log.h"

static const char *TAG = "qmi8658";

/* QMI8658C register map (datasheet Rev B) */
#define REG_WHO_AM_I   0x00
#define REG_CTRL1      0x02
#define REG_CTRL2      0x03
#define REG_CTRL3      0x04
#define REG_CTRL7      0x08
#define REG_AX_L       0x35
#define WHO_AM_I_VAL   0x05

/* CTRL1: ADDR_AI=1, BE=0 (little endian) */
#define CTRL1_VAL      0x40
/* CTRL2: aFS=±4 g (001), aODR=125 Hz (0110) */
#define CTRL2_VAL      0x16
/* CTRL3: gFS=±512 dps (101), gODR=112.1 Hz (0110) */
#define CTRL3_VAL      0x56
/* CTRL7: gEN=1, aEN=1 */
#define CTRL7_VAL      0x03

#define ACC_LSB_PER_G   8192.0f
#define GYR_LSB_PER_DPS 64.0f

static esp_err_t write_reg(qmi8658_t *imu, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(imu->dev, buf, sizeof(buf), 100);
}

static esp_err_t read_regs(qmi8658_t *imu, uint8_t reg, uint8_t *out, size_t len)
{
    return i2c_master_transmit_receive(imu->dev, &reg, 1, out, len, 100);
}

static esp_err_t probe_addr(i2c_master_bus_handle_t bus, uint8_t addr, i2c_master_dev_handle_t *out)
{
    if (i2c_master_probe(bus, addr, 100) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 400000,
    };
    return i2c_master_bus_add_device(bus, &cfg, out);
}

esp_err_t qmi8658_init(qmi8658_t *imu, int sda_gpio, int scl_gpio)
{
    memset(imu, 0, sizeof(*imu));

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = sda_gpio,
        .scl_io_num = scl_gpio,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &imu->bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c bus init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* SA0 may be strapped either way on the dev board; probe both. */
    if (probe_addr(imu->bus, QMI8658_ADDR_SA0_HIGH, &imu->dev) == ESP_OK) {
        imu->addr = QMI8658_ADDR_SA0_HIGH;
    } else if (probe_addr(imu->bus, QMI8658_ADDR_SA0_LOW, &imu->dev) == ESP_OK) {
        imu->addr = QMI8658_ADDR_SA0_LOW;
    } else {
        ESP_LOGE(TAG, "QMI8658 not found on 0x6A/0x6B");
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t who = 0;
    err = read_regs(imu, REG_WHO_AM_I, &who, 1);
    if (err != ESP_OK || who != WHO_AM_I_VAL) {
        ESP_LOGE(TAG, "WHO_AM_I=0x%02x (expected 0x05)", who);
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* Configure: little endian + auto increment, accel ±4 g/125 Hz, gyro ±512 dps/112 Hz, enable both. */
    if ((err = write_reg(imu, REG_CTRL1, CTRL1_VAL)) != ESP_OK ||
        (err = write_reg(imu, REG_CTRL2, CTRL2_VAL)) != ESP_OK ||
        (err = write_reg(imu, REG_CTRL3, CTRL3_VAL)) != ESP_OK ||
        (err = write_reg(imu, REG_CTRL7, CTRL7_VAL)) != ESP_OK) {
        ESP_LOGE(TAG, "config failed: %s", esp_err_to_name(err));
        return err;
    }

    imu->ready = true;
    ESP_LOGI(TAG, "QMI8658C ready at 0x%02x", imu->addr);
    return ESP_OK;
}

esp_err_t qmi8658_read_raw(qmi8658_t *imu, qmi8658_raw_t *raw)
{
    if (!imu->ready) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t buf[12] = {0};
    esp_err_t err = read_regs(imu, REG_AX_L, buf, sizeof(buf));
    if (err != ESP_OK) {
        return err;
    }
    /* AX_L, AX_H, AY_L, AY_H, AZ_L, AZ_H, GX_L, GX_H, ... (little endian) */
    raw->ax = (int16_t)((buf[1] << 8) | buf[0]);
    raw->ay = (int16_t)((buf[3] << 8) | buf[2]);
    raw->az = (int16_t)((buf[5] << 8) | buf[4]);
    raw->gx = (int16_t)((buf[7] << 8) | buf[6]);
    raw->gy = (int16_t)((buf[9] << 8) | buf[8]);
    raw->gz = (int16_t)((buf[11] << 8) | buf[10]);
    return ESP_OK;
}

void qmi8658_to_sample(const qmi8658_raw_t *raw, qmi8658_sample_t *out)
{
    out->ax = raw->ax / ACC_LSB_PER_G;
    out->ay = raw->ay / ACC_LSB_PER_G;
    out->az = raw->az / ACC_LSB_PER_G;
    out->gx = raw->gx / GYR_LSB_PER_DPS;
    out->gy = raw->gy / GYR_LSB_PER_DPS;
    out->gz = raw->gz / GYR_LSB_PER_DPS;
}
