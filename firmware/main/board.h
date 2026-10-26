/* Pin map and I2C addresses for the Rapt Pill board.
 *
 * Values marked CONFIRMED were read out of the 2.0.0 image in Ghidra. TEARDOWN
 * comes from the 2021 community teardown (github.com/BestEnemies/pill) of an
 * older board. The firmware supports board rev >= 2.1 (see hardware_subtype).
 */
#pragma once

#define PILL_I2C_PORT        I2C_NUM_0
#define PILL_I2C_SDA_GPIO    25   /* CONFIRMED in 2.0.0: accel_init_lis3dh @0x400e0520
                                   * builds {port 0, sda 0x19, scl 0x1a} = GPIO25/26 */
#define PILL_I2C_SCL_GPIO    26
#define PILL_I2C_FREQ_HZ     100000

#define LIS3DH_ADDR          0x19 /* CONFIRMED: probe address, WHO_AM_I must be 0x33 */
#define LIS3DH_WHO_AM_I_VAL  0x33
#define TMP1075_ADDR         0x48 /* CONFIRMED: wire address 0x90, config 0x81, 1/16 °C */

#define PILL_LED_GPIO        18   /* TEARDOWN: output, status LED */
#define PILL_OUT5_GPIO       5    /* TEARDOWN: output, role unknown */
#define PILL_BTN_GPIO        23   /* TEARDOWN: input with pull-up; stock fw has click_events/click_threshold */
#define PILL_BATT_ADC_GPIO   34   /* TEARDOWN: ADC1_CH6, battery divider */
#define PILL_BATT_ADC_CHAN   ADC1_CHANNEL_6

/* Advertised interval, 0.625 ms units: 0x800 = 1.28 s. */
#define PILL_ADV_INTERVAL_UNITS 0x800

/* Stock AP passphrase, printed in the boot log. Keeping it means the SSID and
 * password a pill already has saved on someone's phone keep working. */
#define PILL_AP_PASS         "kegland1"

/* Sample period while running from USB. On battery, stock deep sleeps between
 * samples; see PILL_BATTERY_MODE in app_main.c. */
#define PILL_SAMPLE_SECONDS  900

/* Stock firmware publishes these as settings; keep the same names so a stock
 * NVS backup restores into this build. */
#define NVS_NS               "nvs"        /* U: confirm from the flash dump */
#define KEY_TELEM_METHOD     "telemetry_method"
#define KEY_INTERVAL_WIFI    "telemetry_interval_wifi"
#define KEY_INTERVAL_BLE     "telemetry_interval_ble"
#define KEY_GRAV_EXP         "gravity_expr"
#define KEY_CAL_ANGLE        "calibration_angle"
#define KEY_CAL_DENSITY      "calibration_density"
#define KEY_TEMP_OFFSET      "temp_offset"
#define KEY_ANGLE_OFFSET     "cal_offset"   /* CFW: calibration, milli-degrees */
#define KEY_SSID             "ssid"         /* the network the Rapt app saved */
#define KEY_PASS             "password"
