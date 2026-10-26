/* Stock's gravity velocity: a ring of (uptime seconds, gravity points), reduced
 * over the grav_vel_time window. RAM only and uptime-based, exactly like stock
 * — /grav_vel.json reports now_time as uptime because there is no SNTP.
 *
 * Header-inline so the ring arithmetic gets a host test (tests/test_gv.c);
 * that arithmetic is the whole feature. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define PILL_GV_MAX       192     /* 8 days at a 3600 s sample period */
#define PILL_GV_MIN_SPAN  3600u   /* a rate over less is a reboot, not a ferment */

typedef struct {
    uint32_t t;
    float pts;
} pill_gv_pt_t;

typedef struct {
    pill_gv_pt_t pt[PILL_GV_MAX];
    unsigned n, next;
} pill_gv_ring_t;

typedef struct {
    bool valid;
    float latest;                        /* points per day over the window */
    uint32_t latest_time, oldest_time, now_time;
    unsigned num;
} pill_gv_t;

static inline void pill_gv_add(pill_gv_ring_t *r, uint32_t t, float pts)
{
    r->pt[r->next] = (pill_gv_pt_t){ .t = t, .pts = pts };
    r->next = (r->next + 1) % PILL_GV_MAX;
    if (r->n < PILL_GV_MAX) r->n++;
}

/* i = 0 is the oldest live sample. next points at the slot *after* the newest,
 * so the oldest lives n slots back — starting at next walks the empty tail. */
static inline const pill_gv_pt_t *pill_gv_at(const pill_gv_ring_t *r, unsigned i)
{
    return &r->pt[(r->next + PILL_GV_MAX - r->n + i) % PILL_GV_MAX];
}

static inline pill_gv_t pill_gv_reduce(const pill_gv_ring_t *r, uint32_t now,
                                       uint32_t win)
{
    pill_gv_t out = { .now_time = now, .num = r->n };
    const pill_gv_pt_t *first = NULL, *last = NULL;

    for (unsigned i = 0; i < r->n; i++) {
        const pill_gv_pt_t *p = pill_gv_at(r, i);
        if (p->t + win < now) continue;              /* fallen out of the window */
        if (!first) { first = p; out.oldest_time = p->t; }
        last = p;
    }
    if (last) out.latest_time = last->t;

    /* ponytail: slope from the ends of the window; least-squares if the
     * readout proves jumpy on a real fermentation. */
    if (first && last && last->t > first->t &&
        last->t - first->t >= PILL_GV_MIN_SPAN) {
        out.latest = (last->pts - first->pts) * 86400.0f /
                     (float)(last->t - first->t);
        out.valid = true;
    }
    return out;
}
