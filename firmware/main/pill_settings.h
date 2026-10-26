#pragma once
/* Stock's settings surface, ported: the same seven values, the same header
 * names, the same replies. Values live in our own NVS blob so a CFW pill owns
 * its configuration and nothing is shared with the stock record.
 *
 * The defaults are the ones a stock pill reports on first boot (captured from
 * /get_settings.json on 1.5.0): interval_wifi 3600, interval_ble 60,
 * telem_method 1, grav_vel_time 86400, temp_offset 0, low_batt_shutdown 0,
 * smart_mode 0. telem_method is a cloud field with no CFW behaviour; it is
 * stored and reported so stock tooling parsing /get_settings.json stays happy.
 */
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    uint32_t interval_wifi;       /* seconds between samples */
    uint32_t interval_ble;        /* seconds between advert refreshes */
    uint32_t telem_method;        /* stock: 0 = BLE, 1 = Wi-Fi (cloud, parity only) */
    uint32_t grav_vel_time;       /* gravity velocity window, seconds */
    float    temp_offset;         /* degrees C added to every temperature */
    uint32_t low_batt_shutdown;   /* stock: shut down below the brownout floor */
    uint32_t smart_mode;          /* stock: adaptive sampling around activity */
    uint8_t  dev_opts, beta, test_api;   /* /get_dev_buttons.json booleans */

    /* MQTT is off until a broker is set: a device that talks to nobody is the
     * CFW's default. */
    uint8_t  mqtt_enable;
    char     broker[64];
    uint16_t port;
    char     prefix[32];       /* topic prefix, default "rapt" */
    char     mqtt_user[32];
    char     mqtt_pass[64];
} pill_settings_t;

void pill_settings_load(void);
const pill_settings_t *pill_settings(void);
pill_settings_t *pill_settings_edit(void);                     /* live struct  */
esp_err_t pill_settings_put(const pill_settings_t *patch);   /* merge + commit */
