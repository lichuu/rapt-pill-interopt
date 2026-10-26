/* Host test for the gravity-velocity ring: ordering before the wrap, ordering
 * after the wrap, window filtering, oldest_time, the span floor and the slope.
 * Build: gcc -I ../main -lm -o /tmp/tgv test_gv.c && /tmp/tgv */
#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "pill_gv.h"

int main(void)
{
    pill_gv_ring_t r = { 0 };

    pill_gv_t g = pill_gv_reduce(&r, 100, 86400);
    assert(!g.valid && g.num == 0 && g.now_time == 100);

    /* before the wrap: the walk starts at the oldest, not at the empty tail */
    for (unsigned i = 0; i < 3; i++)
        pill_gv_add(&r, 1000 + i * 3600, 1000.0f - i * 5.0f);
    assert(pill_gv_at(&r, 0)->t == 1000);
    assert(pill_gv_at(&r, 2)->t == 8200);

    g = pill_gv_reduce(&r, 9000, 86400);
    assert(g.num == 3 && g.valid);
    assert(g.oldest_time == 1000 && g.latest_time == 8200);
    assert(fabsf(g.latest - (-120.0f)) < 0.01f);   /* -10 pts over 7200 s */

    /* the window drops what fell out; oldest_time is the oldest kept */
    g = pill_gv_reduce(&r, 9000, 5000);
    assert(g.oldest_time == 4600 && g.latest_time == 8200);
    assert(g.valid && fabsf(g.latest - (-120.0f)) < 0.01f);

    /* a span under an hour is a reboot, not a fermentation rate */
    pill_gv_ring_t s = { 0 };
    pill_gv_add(&s, 0, 1000.0f);
    pill_gv_add(&s, 3599, 990.0f);
    assert(!pill_gv_reduce(&s, 3600, 86400).valid);

    /* after the wrap: oldest is still n slots back from next */
    pill_gv_ring_t w = { 0 };
    for (unsigned i = 0; i < PILL_GV_MAX + 5; i++) pill_gv_add(&w, i * 3600u, 1000.0f);
    assert(w.n == PILL_GV_MAX && w.next == 5);
    assert(pill_gv_at(&w, 0)->t == 5 * 3600u);
    assert(pill_gv_at(&w, PILL_GV_MAX - 1)->t == (PILL_GV_MAX + 4) * 3600u);

    printf("gravity velocity ring OK\n");
    return 0;
}
