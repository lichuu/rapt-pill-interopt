/* LAN side of the CFW: Wi-Fi (STA when it has credentials, open config AP when
 * it does not) and the subset of the stock HTTP API that Home Assistant and
 * raptpill2mqtt use. No AWS, no MQTT, no shadow, no OTA. */
#pragma once
#include "esp_err.h"
#include "gravity.h"
#include "pill_gv.h"
#include "pill_wire.h"

/* The gravity model in force, calibration offset included. The web handlers set
 * the offset and the sampler reads through this, so a calibration shows up on
 * the next sample with no reboot. */
const gravity_model_t *pill_net_model(void);

/* For the MQTT module, which reports the same numbers the API does. */
const char *pill_net_version(void);
const char *pill_net_ip(void);              /* STA address, or the AP's */
float pill_net_tilt(void);                  /* calibrated */
float pill_net_temp_c(void);
float pill_net_temp_f(void);

/* Publishes the latest sample to /sensors.json and the BLE advert. */
void pill_net_update(const pill_sample_t *s);

/* Starts Wi-Fi and the HTTP server. Returns ESP_OK with the AP up even when
 * there are no saved credentials — that is the provisioning state. */
esp_err_t pill_net_start(const char *version);

/* True once associated and holding an address. */
bool pill_net_is_connected(void);

/* Stock's gravity velocity: samples kept in uptime seconds (there is no SNTP,
 * and stock's /grav_vel.json reports now_time as uptime), reduced over the
 * grav_vel_time window. The ring itself is pill_gv.h, where it has a host test. */
void pill_gv_push(uint32_t uptime_s, float points);
pill_gv_t pill_gv_get(void);
