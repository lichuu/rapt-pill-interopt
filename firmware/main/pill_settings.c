#include "pill_settings.h"
#include "board.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "settings";
#define KEY_SETT "cfw_sett"

static pill_settings_t s = {
    .interval_wifi = 3600,
    .interval_ble = 60,
    .telem_method = 1,
    .grav_vel_time = 86400,
    .temp_offset = 0.0f,
    .low_batt_shutdown = 0,
    .smart_mode = 0,
    .port = 1883,
};

void pill_settings_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t n = sizeof s;
    if (nvs_get_blob(h, KEY_SETT, &s, &n) == ESP_OK && n == sizeof s)
        ESP_LOGI(TAG, "interval_wifi %u s, interval_ble %u s, temp offset %+.2f C",
                 (unsigned)s.interval_wifi, (unsigned)s.interval_ble,
                 (double)s.temp_offset);
    nvs_close(h);
}

const pill_settings_t *pill_settings(void) { return &s; }
pill_settings_t *pill_settings_edit(void) { return &s; }

esp_err_t pill_settings_put(const pill_settings_t *patch)
{
    s = *patch;
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS, NVS_READWRITE, &h), TAG, "open");
    esp_err_t e = nvs_set_blob(h, KEY_SETT, &s, sizeof s);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}
