#pragma once
#include <stddef.h>
#include "esp_err.h"

esp_err_t pill_sensors_init(void);
esp_err_t pill_read_accel(float *ax, float *ay, float *az);   /* in g */
esp_err_t pill_read_temp(float *deg_c);
