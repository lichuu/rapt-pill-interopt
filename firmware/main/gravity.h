/* Tilt/gravity model for the pill.

 * The stock firmware fits a quintic to the angle and stores it as an expression;
 * /get_calibration_state.json on a live pill returned:
 *
 *   (3.11683E-06*a^5 - 0.000786275*a^4 + 0.07871266*a^3
 *    - 3.916024112*a^2 + 98.73244*a - 8.09818) + -12.3354
 *
 * Output is specific gravity x 1000, monotonic over the usable band:
 *
 *     15° ->  808     30° ->  981     45° -> 1016     60° -> 1041
 *     20° ->  902     35° ->  997     50° -> 1025     70° -> 1061
 *
 * The pill tilts as buoyancy changes, so denser liquid means a larger angle;
 * water (1000) sits near 36°. Below ~12° the fit runs off to nonsense
 * (0° -> -20), which is why stock clamps.
 *
 * Verified against a live capture: extract/api/sensors.json has x=317.029
 * y=932.686, reported tilt 71.6819 and gravity 1.06634. atan2(y, x) = 71.2266,
 * the quintic at 71.6819 is 1078.6753, minus the factory constant 12.3354 is
 * 1066.3399. The pipeline is therefore
 *
 *   tilt    = atan2(y, x) + angle_offset
 *   gravity = (quintic(tilt) - 12.3354) / 1000
 *
 * and the -12.3354 is a factory constant compiled into the firmware, not
 * per-device data. The plane is XY; the pill rolls about Z.
 *
 * Calibration, from this pill's NVS: cal_a1 = 36.4477 (float, degrees),
 * cal_d1 = 1000.0 (float, points), cal_state = 1 (one point done). 36.4477 is
 * exactly the angle the quintic maps to 1000 points, so stock stores a point in
 * *model* space — the ideal angle for a density — and the measured angle is not
 * kept. So the offset a calibration produces is
 *
 *   angle_offset = invert(cal_d1) - angle_measured
 *
 * which for this pill is 36.4477 - 35.9924 = +0.4553, the offset the live
 * reading shows. See tests/test_gravity.c.
 */
#pragma once
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define GRAVITY_ANGLE_MIN  10.0f
#define GRAVITY_ANGLE_MAX  75.0f

typedef struct {
    float c5, c4, c3, c2, c1, c0;   /* c0 folds the trailing "+ -12.3354" */
    float angle_offset;             /* calibration, degrees */
} gravity_model_t;

static const gravity_model_t GRAVITY_MODEL_DEFAULT = {
    3.11683e-06f, -0.000786275f, 0.07871266f, -3.916024112f, 98.73244f,
    -8.09818f - 12.3354f, 0.0f,
};

/* The same model in the text stock prints, so /get_calibration_state.json can
 * quote the factory expression when none has been installed, and so the parser
 * below can be tested against it. */
#define GRAVITY_MODEL_EXPR \
    "(3.11683E-06 * angle^5 - 0.000786275 * angle^4 + 0.07871266 * angle^3" \
    " - 3.916024112 * angle^2 + 98.73244 * angle - 8.09818) + -12.3354"

/* Specific gravity x 1000 at a model-space angle, i.e. with the calibration
 * offset already applied. Monotonic across the band, nonsense outside it. */
static inline float gravity_points_at(float angle, const gravity_model_t *m)
{
    if (angle < GRAVITY_ANGLE_MIN) angle = GRAVITY_ANGLE_MIN;
    if (angle > GRAVITY_ANGLE_MAX) angle = GRAVITY_ANGLE_MAX;
    double p = m->c5;
    p = p * angle + m->c4;
    p = p * angle + m->c3;
    p = p * angle + m->c2;
    p = p * angle + m->c1;
    p = p * angle + m->c0;
    return (float)p;
}

/* Specific gravity, 1.000 = water, from a raw accelerometer angle. */
static inline float gravity_from_angle(float angle_deg, const gravity_model_t *m)
{
    return gravity_points_at(angle_deg + m->angle_offset, m) * 0.001f;
}

/* Stock lets the app install a fitted polynomial through X-Custom-expr, in
 * exactly the shape the model above is printed:
 *
 *   (3.11683E-06 * angle^5 - 0.000786275 * angle^4 + ... - 8.09818) + -12.3354
 *
 * Parse that so a CFW pill honours a model the Rapt server fitted for a unit.
 * The trailing constant folds into c0, the way the built-in model does it.
 * Returns false when the text is not that shape; m is untouched then. */
static inline bool gravity_parse_expr(const char *s, gravity_model_t *m)
{
    if (!s || !*s) return false;
    const char *p = strchr(s, '(');
    if (!p) return false;
    const char *close = strchr(++p, ')');
    if (!close) return false;

    double c[6] = { 0 };
    bool any = false;
    while (p < close) {
        while (*p == ' ') p++;
        int sign = 1;
        if (*p == '+') p++;
        else if (*p == '-') { sign = -1; p++; }
        while (*p == ' ') p++;
        char *end;
        double v = strtod(p, &end);
        if (end == p) return false;
        p = end;
        while (*p == ' ') p++;
        int pw;
        if (!strncmp(p, "* angle^", 8)) {
            pw = p[8] - '0';
            if (pw < 1 || pw > 5) return false;
            p += 9;
        } else if (!strncmp(p, "* angle", 7)) {
            pw = 1;
            p += 7;
        } else {
            pw = 0;                        /* bare constant term, "- 8.09818" */
            if (*p == '*') p += 1;         /* tolerate "* 8.09818" */
        }
        c[pw] += sign * v;
        any = true;
        while (*p == ' ') p++;
    }

    double tail = 0.0;
    p = close + 1;
    while (*p == ' ') p++;
    if (*p == '+' || *p == '-') {
        int sign = (*p == '-') ? -1 : 1;
        p++;                            /* "+ -12.3354": sign, space, number */
        while (*p == ' ') p++;
        char *end;
        tail = sign * strtod(p, &end);
        if (end == p) return false;
    }
    if (!any) return false;

    gravity_model_t out = *m;
    out.c5 = (float)c[5];
    out.c4 = (float)c[4];
    out.c3 = (float)c[3];
    out.c2 = (float)c[2];
    out.c1 = (float)c[1];
    out.c0 = (float)c[0] + (float)tail;   /* the trailing constant folds in here */
    *m = out;
    return true;
}

/* The model-space angle that reads `points`. The fit is monotonic over the
 * band, so bisection is enough; 60 halvings is far below one float ulp. */
static inline float gravity_angle_for(float points, const gravity_model_t *m)
{
    float lo = GRAVITY_ANGLE_MIN, hi = GRAVITY_ANGLE_MAX;
    for (int i = 0; i < 60; ++i) {
        float mid = (lo + hi) * 0.5f;
        if (gravity_points_at(mid, m) > points) hi = mid; else lo = mid;
    }
    return (lo + hi) * 0.5f;
}

/* The offset that makes the pill read `points` while sitting at `angle_deg`.
 * This is stock's /cal_1p.json: a density in, an offset out. */
static inline float gravity_offset_for(float angle_deg, float points,
                                       const gravity_model_t *m)
{
    return gravity_angle_for(points, m) - angle_deg;
}

/* Raw plane angle, offset NOT included. Stock's reported tilt carries the
 * calibration offset, so add angle_offset wherever you publish a tilt. */
static inline float pill_angle_deg(float ax, float ay)
{
    return atan2f(ay, ax) * (180.0f / (float)M_PI);
}
