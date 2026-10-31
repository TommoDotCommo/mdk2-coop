#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "presence.h"
static unsigned int random_state = 4231;
static unsigned int random_word(void) { random_state ^= random_state << 13; random_state ^= random_state >> 17; random_state ^= random_state << 5; return random_state; }
int main(void)
{
    struct canopy_state a = {0}, b = {0}, unchanged;
    unsigned char wire[CANOPY_WIRE_MAX];
    struct presence_player players[3] = {{{50,0,0},1},{{2,0,0},1},{{3,0,0},1}};
    const float origin[3] = {0};
    struct join_flow flow = {0};
    int i, j;
    assert(canopy_encode(&a, wire, sizeof(wire)) == 1);
    assert(canopy_decode(wire, 1, &b) == 1 && !b.present);
    a.present = 1; a.cur = 66; a.prev = 67; a.flags = 0x22;
    a.anim[0][0] = 1;
    assert(canopy_encode(&a, wire, sizeof(wire)) == CANOPY_WIRE_MAX);
    assert(canopy_decode(wire, sizeof(wire), &b) == CANOPY_WIRE_MAX);
    assert(memcmp(&a, &b, sizeof(a)) == 0);
    unchanged = b;
    for (i = 0; i < CANOPY_WIRE_MAX; i++) {
        assert(!canopy_decode(wire, i, &b));
        assert(memcmp(&b, &unchanged, sizeof(b)) == 0);
        assert(!canopy_encode(&a, wire, i));
    }
    wire[0] = 2; assert(!canopy_decode(wire, sizeof(wire), &b));
    canopy_encode(&a, wire, sizeof(wire)); wire[1] = 0x80; assert(!canopy_decode(wire, sizeof(wire), &b));
    canopy_encode(&a, wire, sizeof(wire)); wire[2] = 160; assert(!canopy_decode(wire, sizeof(wire), &b));
    a.anim[0][3] = 0x7fc00000; assert(!canopy_encode(&a, wire, sizeof(wire)));
    a.anim[0][3] = 0; canopy_encode(&a, wire, sizeof(wire));
    wire[4+3*4+2] = 0x80; wire[4+3*4+3] = 0x7f;
    assert(!canopy_decode(wire, sizeof(wire), &b));
    /* Deploy, open, close, repeat with lost and reordered packets: each snapshot
     * is complete and the newest delivery restores the full controller. */
    for (i = 0; i < 10000; i++) {
        float phase = (random_word() % 10000) / 10000.0f;
        a.cur = i % 2 ? 66 : 67; a.prev = i % 2 ? 67 : 66;
        a.flags = i % 2 ? 0x20 : 2;
        memcpy(&a.anim[i%2][3], &phase, 4);
        assert(canopy_encode(&a, wire, sizeof(wire)) == CANOPY_WIRE_MAX);
        assert(canopy_decode(wire, sizeof(wire), &b) == CANOPY_WIRE_MAX);
        assert(memcmp(&a, &b, sizeof(a)) == 0);
        for (j = 0; j < CANOPY_WIRE_MAX; j++) wire[j] = random_word();
        b = unchanged;
        if (!canopy_decode(wire, random_word() % (CANOPY_WIRE_MAX+1), &b))
            assert(memcmp(&b, &unchanged, sizeof(b)) == 0);
    }
    /* Equipment visuals: the wire form of each kind, with lost, truncated and corrupt data */
    {
        struct gear_state g = {0}, r, kept;
        unsigned char gw[GEAR_KINDS * (5 + GEAR_ANIMS * 41)];
        int k, m, n, full;
        assert(gear_encode(&g, gw, sizeof(gw)) == GEAR_KINDS);              /* absent visuals are one byte each */
        assert(gear_decode(gw, GEAR_KINDS, &r) == GEAR_KINDS && !r.v[0].present && !r.v[GEAR_KINDS - 1].present);
        for (k = 0; k < GEAR_KINDS; k++) {
            struct gear_visual *v = &g.v[k];
            v->present = 1; v->hidden = k & 1; v->model = k + 1; v->cur = 77; v->prev = 62; v->nanims = GEAR_ANIMS;
            for (m = 0; m < GEAR_ANIMS; m++) {
                float phase = 0.125f * (m + 1);
                v->id[m] = (unsigned char)(60 + m * 9 + k);
                v->anim[m][0] = 1; v->anim[m][1] = 1 + m; v->anim[m][7] = 0x40800000u;
                memcpy(&v->anim[m][3], &phase, 4); memcpy(&v->anim[m][4], &phase, 4);
                v->anim[m][5] = 0x7f800000u;            /* a looping animation's end time is infinite */
                v->anim[m][6] = 0x3f800000u; v->anim[m][9] = 1;
            }
        }
        full = gear_encode(&g, gw, sizeof(gw));
        assert(full == GEAR_KINDS * (5 + GEAR_ANIMS * 41));
        assert(gear_decode(gw, full, &r) == full);
        for (k = 0; k < GEAR_KINDS; k++) {
            assert(r.v[k].present == 1 && r.v[k].hidden == (k & 1) && r.v[k].model == k + 1);
            assert(r.v[k].cur == 77 && r.v[k].prev == 62 && r.v[k].nanims == GEAR_ANIMS);
            for (m = 0; m < GEAR_ANIMS; m++) {
                assert(r.v[k].id[m] == g.v[k].id[m]);
                assert(memcmp(r.v[k].anim[m], g.v[k].anim[m], sizeof(r.v[k].anim[m])) == 0);
            }
        }
        memset(&kept, 0x5a, sizeof(kept));
        for (n = 0; n < full; n++) {
            r = kept;
            assert(!gear_decode(gw, n, &r) && memcmp(&r, &kept, sizeof(r)) == 0);
            assert(!gear_encode(&g, gw, n));
        }
        gear_encode(&g, gw, sizeof(gw));
        gw[0] = 2; assert(!gear_decode(gw, full, &r));
        gear_encode(&g, gw, sizeof(gw)); gw[1] = 0x0a; assert(!gear_decode(gw, full, &r));         /* model 5 */
        gear_encode(&g, gw, sizeof(gw)); gw[2] = 160; assert(!gear_decode(gw, full, &r));         /* current animation */
        gear_encode(&g, gw, sizeof(gw)); gw[3] = 160; assert(!gear_decode(gw, full, &r));         /* previous animation */
        gear_encode(&g, gw, sizeof(gw)); gw[4] = GEAR_ANIMS + 1; assert(!gear_decode(gw, full, &r));
        gear_encode(&g, gw, sizeof(gw)); gw[5] = 160; assert(!gear_decode(gw, full, &r));         /* animation id */
        gear_encode(&g, gw, sizeof(gw)); gw[5 + 1 + 3 * 4 + 2] = 0x80; gw[5 + 1 + 3 * 4 + 3] = 0x7f;
        assert(!gear_decode(gw, full, &r));                                                       /* infinite play position */
        g.v[0].anim[0][3] = 0x7fc00000u; assert(!gear_encode(&g, gw, sizeof(gw)));                /* NaN: never sent */
        g.v[0].anim[0][3] = 0;
        g.v[0].present = 2; assert(!gear_encode(&g, gw, sizeof(gw))); g.v[0].present = 1;
        g.v[0].nanims = GEAR_ANIMS + 1; assert(!gear_encode(&g, gw, sizeof(gw))); g.v[0].nanims = GEAR_ANIMS;
        g.v[0].id[0] = 160; assert(!gear_encode(&g, gw, sizeof(gw))); g.v[0].id[0] = 60;
        /* corrupt packets decode to nothing or to something valid, never into the destination half-written */
        for (n = 0; n < 20000; n++) {
            for (m = 0; m < (int)sizeof(gw); m++) gw[m] = (unsigned char)random_word();
            if (n % 2) for (k = 0; k < GEAR_KINDS; k++) gw[k * (5 + GEAR_ANIMS * 41)] &= 1;
            r = kept;
            if (!gear_decode(gw, (int)(random_word() % (sizeof(gw) + 1)), &r)) assert(memcmp(&r, &kept, sizeof(r)) == 0);
        }
    }
    assert(presence_nearest(origin, players, 3) == 1);
    players[1].eligible = 0; assert(presence_nearest(origin, players, 3) == 2);
    players[2].pos[0] = NAN; assert(presence_nearest(origin, players, 3) == 0);
    players[0].eligible = 0; assert(presence_nearest(origin, players, 3) == -1);
    assert(join_step(&flow, 0, 0, -1, 0, 0, 0, 0) == JOIN_CONNECTING);
    assert(join_step(&flow, 1, 1, 0, 0, 0, 0, 0) == JOIN_WAITING);
    assert(join_step(&flow, 2, 1, 1, 0, 1, 0, 0) == JOIN_LOADING);
    assert(join_step(&flow, 3, 1, 1, 1, 0, 0, 0) == JOIN_PLAYING);
    assert(join_step(&flow, 4, 0, -1, 1, 0, 0, 0) == JOIN_LOST);
    assert(join_step(&flow, 63, 0, -1, 1, 0, 0, 0) == JOIN_TIMEOUT);
    assert(join_step(&flow, 64, 1, 0, 1, 0, 0, 0) == JOIN_WAITING);
    assert(join_step(&flow, 65, 1, 1, 1, 0, 1, 0) == JOIN_MISMATCH);
    assert(join_step(&flow, 66, 1, 1, 1, 0, 0, 1) == JOIN_CANCELLED);
    assert(join_step(&flow, 67, 1, 1, 1, 0, 0, 0) == JOIN_CANCELLED);
    memset(&flow,0,sizeof(flow));
    assert(join_step(&flow,0,1,2,1,0,0,0)==JOIN_LOADING);
    assert(join_step(&flow,61,1,2,1,0,0,0)==JOIN_TIMEOUT);
    assert(join_step(&flow,62,1,2,1,0,0,0)==JOIN_TIMEOUT);
    assert(join_step(&flow,63,1,2,2,0,0,0)==JOIN_PLAYING);
    assert(strstr(join_text(JOIN_WAITING), "host"));
    puts("PASS presence: codec, equipment codec, 10000 fuzz deliveries, eligibility, join lifecycle");
    return 0;
}
