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
    puts("PASS presence: codec, 10000 fuzz deliveries, eligibility, join lifecycle");
    return 0;
}
