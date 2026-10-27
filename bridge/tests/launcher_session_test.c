#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include "launcher_session.h"
static uint32_t rng = 15;
static uint32_t next(void) { rng = rng * 1664525u + 1013904223u; return rng; }
int main(void)
{
    struct launcher_session s, old;
    uint32_t now;
    int i;
    ls_open(&s, 0, 100);
    assert(s.phase == LS_WAITING);
    old = s;
    assert(!ls_roster(&s, 200, 1, 0) && s.phase == old.phase);
    assert(!ls_roster(&s, 200, 3001, 1));
    assert(!ls_roster(&s, 200, -2, 1));
    assert(!ls_begin(&s, 200, 3000, 1));
    assert(ls_roster(&s, 200, -1, 1) && s.phase == LS_LOBBY);
    assert(!ls_begin(&s, 300, 3000, 1));
    ls_tick(&s, 4201); assert(s.phase == LS_LOST);
    assert(ls_roster(&s, 4300, -1, 1) && s.phase == LS_LOBBY);
    assert(ls_roster(&s, 4400, 3000, 1));
    assert(s.deadline == 7400);
    assert(ls_roster(&s, 4500, 3000, 1) && s.deadline == 7400);
    ls_tick(&s, 7400); assert(s.phase == LS_LOST); /* host disappeared during countdown */
    assert(ls_roster(&s, 8000, 3000, 1));
    assert(ls_roster(&s, 10500, 500, 1));
    ls_tick(&s, 10999); assert(s.phase == LS_COUNTDOWN);
    ls_tick(&s, 11000); assert(s.phase == LS_START);
    assert(!ls_roster(&s, 11001, -1, 1));
    ls_leave(&s); assert(s.phase == LS_OFF);
    assert(!ls_roster(&s, 12000, 0, 1));
    ls_tick(&s, 15000); assert(s.phase == LS_OFF);
    ls_open(&s, 1, 20);
    assert(!ls_roster(&s, 30, 0, 1));
    assert(!ls_begin(&s, 30, 3001, 1));
    assert(!ls_begin(&s, 30, -1, 1));
    assert(!ls_begin(&s, 30, 3000, 0));
    assert(ls_begin(&s, 40, 3000, 1));
    assert(!ls_begin(&s, 50, 3000, 1));
    ls_tick(&s, 3039); assert(s.phase == LS_COUNTDOWN);
    ls_tick(&s, 3040); assert(s.phase == LS_START);
    ls_open(&s, 0, UINT32_MAX - 100);
    assert(ls_roster(&s, UINT32_MAX - 50, 200, 1));
    ls_tick(&s, UINT32_MAX - 10); assert(s.phase == LS_COUNTDOWN);
    ls_tick(&s, 148); assert(s.phase == LS_COUNTDOWN);
    ls_tick(&s, 149); assert(s.phase == LS_START);
    ls_open(&s, 0, 0);
    assert(ls_roster(&s, 100, 3000, 1));
    assert(ls_roster(&s, 200, -1, 1) && s.phase == LS_LOBBY && !s.deadline);
    /* Deterministic random-loss/event simulator, including clock wrapping. */
    for (i = 0; i < 100000; i++) {
        int valid = next() & 1, delay = (int)(next() % 5000) - 1000;
        now = next();
        ls_open(&s, next() & 1, now);
        old = s;
        if (s.host) {
            int accepted = ls_begin(&s, now, delay, valid);
            assert(accepted == (valid && delay >= 0 && delay <= 3000));
            if (!accepted) assert(s.phase == old.phase);
        } else {
            int accepted = ls_roster(&s, now, delay, valid);
            assert(accepted == (valid && delay >= -1 && delay <= 3000));
            if (!accepted) assert(s.phase == old.phase);
        }
        now += next() % 8000;
        ls_tick(&s, now);
        if (s.phase == LS_START && !s.host) assert((uint32_t)(now - s.seen) <= 1000);
        ls_leave(&s); ls_tick(&s, now + 10000); assert(s.phase == LS_OFF);
    }
    puts("PASS: lifecycle transitions and 100000 simulated sessions");
    return 0;
}
