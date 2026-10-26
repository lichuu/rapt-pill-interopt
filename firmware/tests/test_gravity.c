/* The quintic, the plane and the calibration offset, checked against a live
 * capture (extract/api/sensors.json, a real pill on 1.5.0).
 *
 *   cc -I ../main -o /tmp/tg tests/test_gravity.c -lm && /tmp/tg
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "gravity.h"

int main(void)
{
    gravity_model_t m = GRAVITY_MODEL_DEFAULT;

    /* x=317.029 y=932.686 z=216.971 -> stock reported tilt 71.6819 and
     * gravity 1.06634. The raw XY angle is 71.2266, so the calibration offset
     * for that pill is +0.4553 deg. */
    float raw = pill_angle_deg(317.029f / 1000.0f, 932.686f / 1000.0f);
    assert(fabsf(raw - 71.2266f) < 0.001f);

    m.angle_offset = 0.4553f;
    float g = gravity_from_angle(raw, &m);
    printf("raw %.4f  tilt %.4f  gravity %.5f\n", raw, raw + m.angle_offset, g);
    assert(fabsf(g - 1.06634f) < 1e-5f);

    /* Water near 35 deg, monotonic across the floating band. */
    assert(fabsf(gravity_from_angle(35.0f, &m) - 0.99668f) < 2e-3f);
    float prev = 0;
    for (float a = 12.0f; a <= 73.0f; a += 1.0f) {
        float v = gravity_from_angle(a, &m);
        assert(v > prev);
        prev = v;
    }

    /* Outside the band the fit is nonsense (0 deg -> -20), so it clamps to the
     * floor of the band; 10 deg is 0.646, which is what stock reports for a
     * pill lying flat, and why a flat pill reads nonsense. */
    assert(gravity_from_angle(0.0f, &m) == gravity_from_angle(9.5447f, &m));
    assert(gravity_from_angle(90.0f, &m) == gravity_from_angle(75.0f - 0.4553f, &m));

    /* Stock's stored calibration point, read out of this pill's NVS:
     * cal_a1 = 36.4477 deg, cal_d1 = 1000.0 points, cal_state = 1. */
    float ideal = gravity_angle_for(1000.0f, &m);
    printf("invert(1000) = %.4f deg, pill's cal_a1 = 36.4477\n", ideal);
    assert(fabsf(ideal - 36.4477f) < 0.01f);

    /* The pill sat at 35.9924 deg raw in water; cal_a1 minus that is the
     * offset the live reading showed (+0.4553). */
    float off = gravity_offset_for(35.9924f, 1000.0f, &m);
    printf("offset from a water calibration: %+.4f deg\n", off);
    assert(fabsf(off - 0.4553f) < 0.002f);

    /* the printed expression, parsed back, must reproduce the built-in model */
    gravity_model_t pm = GRAVITY_MODEL_DEFAULT;
    pm.angle_offset = 0.0f;
    assert(gravity_parse_expr(GRAVITY_MODEL_EXPR, &pm));
    for (float a = 10.0f; a <= 75.0f; a += 2.5f)
        assert(fabsf(gravity_points_at(a, &pm)
                     - gravity_points_at(a, &GRAVITY_MODEL_DEFAULT)) < 0.05f);
    assert(!gravity_parse_expr("not a model", &pm));
    assert(!gravity_parse_expr("(1.0 * angle^9)", &pm));
    printf("expression parse: round-trips the factory model\n");

    printf("gravity model OK\n");
    return 0;
}
