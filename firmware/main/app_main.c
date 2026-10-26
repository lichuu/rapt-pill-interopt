/* Rapt Pill CFW: read the sensors, tilt-to-gravity with the stock quintic,
 * advertise the stock BLE payload, serve the stock-shaped LAN API. No AWS, no
 * MQTT, no shadow, no OTA — the pill stops phoning home and keeps working for
 * Home Assistant, raptpill2mqtt and any BLE scanner.
 *
 * Boot order matters: NVS, sensors, network, then BLE. The network comes up
 * APSTA so a pill that was provisioned by the Rapt app joins its saved network
 * on first boot (esp_wifi reads the credentials the stock firmware left in NVS)
 * and an unprovisioned one exposes RAPT-Pill-XXXXXX / kegland1 with a form at
 * http://172.217.28.1/. */
#include <math.h>
#include <stdio.h>

#include "board.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gravity.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "pill_ble.h"
#include "pill_net.h"
#include "pill_mqtt.h"
#include "pill_ota.h"
#include "pill_settings.h"
#include "pill_wire.h"
#include "sensors.h"

static const char *TAG = "pill";

/* Battery builds deep sleep between samples; the USB bench build stays awake so
 * the HTTP API is always live. */
#ifndef PILL_BATTERY_MODE
#define PILL_BATTERY_MODE 0
#endif

/* The gravity model, calibration and all, lives with the web layer that sets
 * it — see pill_net_model(). Sampling happens after pill_net_start(), so the
 * offset from NVS is in force by the first reading. */
static void sample(pill_sample_t *s)
{
    *s = (pill_sample_t){ .gravity = 1.0f };
    if (pill_read_accel(&s->ax, &s->ay, &s->az) != ESP_OK) return;
    if (pill_read_temp(&s->temp_c) != ESP_OK) s->temp_c = 0;
    float angle = pill_angle_deg(s->ax, s->ay);
    s->gravity = gravity_from_angle(angle, pill_net_model());
    /* Stock's gravity velocity: push into the ring, report the window slope.
     * The ring is RAM, which is fine awake; a battery build deep sleeps and
     * would have to persist it — see the pill_gv notes in pill_net.c. */
    pill_gv_push((uint32_t)(esp_timer_get_time() / 1000000), s->gravity * 1000.0f);
    pill_gv_t gv = pill_gv_get();
    s->gravity_velocity = gv.latest;
    s->gravity_velocity_valid = gv.valid;
    ESP_LOGI(TAG, "tilt %.2f°  gravity %.5f  temp %.4f  a(%.3f,%.3f,%.3f)g",
             angle, s->gravity, s->temp_c, s->ax, s->ay, s->az);
}

void app_main(void)
{
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    gpio_reset_pin(PILL_LED_GPIO);
    gpio_set_direction(PILL_LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(PILL_LED_GPIO, 1);

    if (pill_sensors_init() != ESP_OK)
        ESP_LOGE(TAG, "no sensors on the bus; reporting a flat 1.000");

    ESP_ERROR_CHECK(pill_net_start("cfw-0.1"));
    pill_mqtt_start();

    pill_sample_t s;
    sample(&s);
    pill_ble_start(&s);
    pill_net_update(&s);
    pill_mqtt_publish(&s);
    /* Reaching here — sensors read, advert out, server up — is the CFW's idea
     * of "this build works", so disarm rollback now rather than only on IP. */
    pill_ota_confirm_boot();
    gpio_set_level(PILL_LED_GPIO, 0);

#if PILL_BATTERY_MODE
    /* Non-connectable advertisements continue from the BLE controller while the
     * CPU sleeps; 1.28 s intervals for a 15-minute sample period is what stock
     * does, and it is what a $2 coin cell survives. */
    esp_sleep_enable_timer_wakeup((uint64_t)PILL_SAMPLE_SECONDS * 1000000ULL);
    /* esp-mqtt publishes from its own task: give it a moment to hand the QoS 1
     * values to the broker before the CPU goes away. */
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_deep_sleep_start();
#else
    for (;;) {
        /* interval_wifi is a stock setting and applies from the next pass. */
        vTaskDelay(pdMS_TO_TICKS(pill_settings()->interval_wifi * 1000ULL));
        sample(&s);
        pill_ble_update(&s);
        pill_net_update(&s);
        pill_mqtt_publish(&s);
    }
#endif
}
