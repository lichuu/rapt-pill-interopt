/* MQTT with Home Assistant discovery, in FermentDial's shape: a retained
 * availability topic with an LWT, retained discovery configs, and one state
 * topic per reading under a prefix.
 *
 * Off unless pill_settings()->mqtt_enable with a broker set, because a device
 * that talks to nobody is the CFW's default.
 *
 * No exp_aft: the pill has no SNTP, so it cannot promise a wall-clock expiry.
 * Home Assistant marks the entity unavailable from the LWT instead.
 */
#pragma once
#include "pill_wire.h"

void pill_mqtt_start(void);                      /* from settings; no-op if off */
void pill_mqtt_stop(void);
void pill_mqtt_publish(const pill_sample_t *s);  /* one sample, after the API */
