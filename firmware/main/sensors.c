#include "sensors.h"
#include "board.h"

#include <math.h>
#include <string.h>

#include "driver/i2c.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "sensors";

/* Confirmed in the stock 2.0.0 image: accel_init_lis3dh @ 0x400e0520 calls
 * i2c_param_config(0, sda=0x19, scl=0x1a, clk_conf=0x101, ext_config=<global>)
 * then i2c_driver_install(0, MODE_MASTER, 0, 0, 0), probes address 0x19 twice
 * and requires WHO_AM_I (reg 0x0f) == 0x33. TMP1075 is addressed as 0x90, i.e.
 * 7-bit 0x48, and its temperature is (raw >> 4) * 0.0625 with a -256 sign fix.
 */

/* Read n bytes starting at reg. The LIS3DH wants bit 7 set for auto-increment;
 * TMP1075 has 8-bit register addresses, so bit 7 there is a register that does
 * not exist and the write NACKs. Callers pass the byte the device expects. */
static esp_err_t rd(uint8_t dev7, uint8_t reg, uint8_t *buf, size_t n)
{
    i2c_cmd_handle_t c = i2c_cmd_link_create();
    i2c_master_start(c);
    i2c_master_write_byte(c, dev7 << 1, true);
    i2c_master_write_byte(c, reg, true);
    i2c_master_start(c);
    i2c_master_write_byte(c, (dev7 << 1) | 1, true);
    for (size_t i = 0; i < n; i++)
        i2c_master_read_byte(c, buf + i, i + 1 == n ? I2C_MASTER_NACK : I2C_MASTER_ACK);
    i2c_master_stop(c);
    esp_err_t e = i2c_master_cmd_begin(PILL_I2C_PORT, c, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(c);
    return e;
}

static esp_err_t wr(uint8_t dev7, uint8_t reg, uint8_t val)
{
    i2c_cmd_handle_t c = i2c_cmd_link_create();
    i2c_master_start(c);
    i2c_master_write_byte(c, dev7 << 1, true);
    i2c_master_write_byte(c, reg, true);
    i2c_master_write_byte(c, val, true);
    i2c_master_stop(c);
    esp_err_t e = i2c_master_cmd_begin(PILL_I2C_PORT, c, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(c);
    return e;
}

esp_err_t pill_sensors_init(void)
{
    i2c_config_t cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = PILL_I2C_SDA_GPIO,
        .scl_io_num = PILL_I2C_SCL_GPIO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = PILL_I2C_FREQ_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_param_config(PILL_I2C_PORT, &cfg), TAG, "param");
    ESP_RETURN_ON_ERROR(i2c_driver_install(PILL_I2C_PORT, cfg.mode, 0, 0, 0), TAG, "install");

    uint8_t who = 0;
    ESP_RETURN_ON_ERROR(rd(LIS3DH_ADDR, 0x8f, &who, 1), TAG, "no LIS3DH");
    if (who != LIS3DH_WHO_AM_I_VAL) return ESP_ERR_NOT_FOUND;
    /* 0x20 CTRL1: ODR 10 Hz, normal mode, XYZ enable. 0x23 CTRL4: BDU +
     * continuous. 0x20 0x57 / 0x23 0x88 is the usual LIS3DH pairing; the stock
     * firmware additionally pokes reg 0x1e (aux/temp config). */
    ESP_RETURN_ON_ERROR(wr(LIS3DH_ADDR, 0x20, 0x57), TAG, "ctrl1");
    ESP_RETURN_ON_ERROR(wr(LIS3DH_ADDR, 0x23, 0x88), TAG, "ctrl4");

    /* TMP1075: pointer 0x00, config 0x81 (one-shot, 0.25 s conversion rate). */
    ESP_RETURN_ON_ERROR(wr(TMP1075_ADDR, 0x00, 0x00), TAG, "tmp ptr");
    ESP_RETURN_ON_ERROR(wr(TMP1075_ADDR, 0x01, 0x81), TAG, "tmp cfg");
    return ESP_OK;
}

esp_err_t pill_read_accel(float *ax, float *ay, float *az)
{
    uint8_t b[6];
    ESP_RETURN_ON_ERROR(rd(LIS3DH_ADDR, 0xa8, b, 6), TAG, "accel read");
    int16_t x = (int16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
    int16_t y = (int16_t)((uint16_t)b[2] | ((uint16_t)b[3] << 8));
    int16_t z = (int16_t)((uint16_t)b[4] | ((uint16_t)b[5] << 8));
    x >>= 4; y >>= 4; z >>= 4;              /* 12-bit, left aligned */
    /* CTRL4 0x88 leaves FS = 00, i.e. +/-2 g at 1 mg/LSB -> 1024 LSB/g. Measured
     * on the pill: dividing by 4096 gave |a| = 0.245 g lying flat. */
    *ax = x / 1024.0f;
    *ay = y / 1024.0f;
    *az = z / 1024.0f;
    return ESP_OK;
}

esp_err_t pill_read_temp(float *deg_c)
{
    uint8_t b[2];
    ESP_RETURN_ON_ERROR(rd(TMP1075_ADDR, 0x00, b, 2), TAG, "temp read");
    uint16_t raw = ((uint16_t)b[0] << 8) | b[1];
    float t = (float)(raw >> 4) * 0.0625f;
    if ((int16_t)raw < 0) t -= 256.0f;      /* stock firmware does the same */
    *deg_c = t;
    return ESP_OK;
}
