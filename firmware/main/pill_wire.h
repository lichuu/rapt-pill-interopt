/* Rapt Pill BLE advertisement wire format.
 *
 * Recovered from the community parser (sairon/rapt-ble, MIT, vendored at
 * tests/rapt_ble_parser_reference.py) and cross-checked against the strings and
 * field names in the stock 2.0.0 image (gravity, gravity_velocity, tilt, temp,
 * battery). Home Assistant, SPINDELMATE and raptpill2mqtt parse this layout.
 *
 * Manufacturer-specific AD (type 0xFF): company id 0x4152 little-endian
 * on the wire (bytes 0x52 0x41 = "RA"), then:
 *
 *   char  tag[2]   "PT"          (so the whole AD reads "RAPT")
 *   u8    version  2
 *   u8    reserved 0
 *   u8    gv_valid
 *   f32   gravity_velocity      big-endian, points per day, negative = falling
 *   u16   temperature           big-endian, K*128      -> C = raw/128 - 273.15
 *   f32   gravity               big-endian, specific gravity * 1000
 *   i16   x, y, z               big-endian, acceleration in g * 16
 *   i16   battery               big-endian, volts * 256  (I: inferred, confirm
 *                               by sniffing a stock pill)
 *
 * Everything after the company id is 23 bytes.
 */
#pragma once
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define PILL_ADV_COMPANY 0x4152   /* on the wire: 0x52, 0x41 */
#define PILL_ADV_BODY_LEN 23

typedef struct {
    float gravity;            /* specific gravity, e.g. 1.048 */
    float gravity_velocity;   /* points/day */
    bool gravity_velocity_valid;
    float temp_c;
    float ax, ay, az;         /* g */
    float battery_v;
} pill_sample_t;

static void pill_put_be16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
static void pill_put_be32(uint8_t *p, uint32_t v) {
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}
static void pill_put_be_f32(uint8_t *p, float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    pill_put_be32(p, u);
}

/* Fill out[PILL_ADV_BODY_LEN] (the bytes after the company id). */
static inline void pill_adv_build(uint8_t *out, const pill_sample_t *s)
{
    memset(out, 0, PILL_ADV_BODY_LEN);
    out[0] = 'P';
    out[1] = 'T';
    out[2] = 2;                                   /* version */
    out[3] = 0;                                   /* reserved */
    out[4] = s->gravity_velocity_valid ? 1 : 0;
    pill_put_be_f32(out + 5, s->gravity_velocity);
    pill_put_be16(out + 9, (uint16_t)lroundf((s->temp_c + 273.15f) * 128.0f));
    pill_put_be_f32(out + 11, s->gravity * 1000.0f);
    pill_put_be16(out + 15, (int16_t)lroundf(s->ax * 16.0f));
    pill_put_be16(out + 17, (int16_t)lroundf(s->ay * 16.0f));
    pill_put_be16(out + 19, (int16_t)lroundf(s->az * 16.0f));
    pill_put_be16(out + 21, (int16_t)lroundf(s->battery_v * 256.0f));
}

/* Prefix bytes the NimBLE raw-advertising API wants for a manufacturer AD. */
static inline void pill_adv_prefix(uint8_t out[4])
{
    out[0] = PILL_ADV_COMPANY & 0xff;   /* 'R' */
    out[1] = PILL_ADV_COMPANY >> 8;     /* 'A' */
    out[2] = 'P';
    out[3] = 'T';
}
