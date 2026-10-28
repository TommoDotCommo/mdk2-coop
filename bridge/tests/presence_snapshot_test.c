#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "snapshot.h"
typedef unsigned char BYTE;
typedef uint32_t DWORD;
static int interp_ms = 100, extrap_ms = 150;
static double clock_now;
static double now_ms(void) { return clock_now; }
#include SNAPSHOT_SOURCE
int main(void)
{
    struct snap source = {0}, decoded = {0};
    struct track track = {0};
    unsigned char wire[512];
    unsigned int random = 71;
    int i, j, n;
    source.quat[0] = 1;
    source.cur = source.prev = NO_ANIM;
    source.canopy.present = 1; source.canopy.cur = 66; source.canopy.prev = 67;
    source.canopy.anim[0][0] = 1;
    for (i = 0; i < 10000; i++) {
        source.time = i * 33;
        source.canopy.cur = i%2 ? 66 : 67;
        source.canopy.flags = i%2 ? 0x20 : 2;
        n = snap_encode(&source, wire, sizeof(wire));
        assert(n > 0 && n <= (int)sizeof(wire));
        for(j = 0; j < n; j++) assert(!snap_decode(wire, j, &decoded));
        decoded.time = source.time;
        assert(snap_decode(wire, n, &decoded) == n);
        assert(memcmp(&decoded.canopy, &source.canopy, sizeof(source.canopy)) == 0);
        /* Drop one in three; add late duplicate deliveries on a full track. */
        if (i%3) track_add(&track, &decoded);
        if (i > 10 && i%7 == 0) { decoded.time -= 165; track_add(&track, &decoded); }
        clock_now = source.time;
        track_update(&track, 0);
        if(track.n) {
            assert(track.n <= TRACK_SNAPS);
            for(j=1;j<track.n;j++) assert(track.snaps[j-1].time <= track.snaps[j].time);
            assert(track.view.canopy.present && (track.view.canopy.cur == 66 || track.view.canopy.cur == 67));
        }
        for(j=0;j<(int)sizeof(wire);j++) { random = random*1664525u + 1013904223u; wire[j] = random >> 24; }
        snap_decode(wire, random % sizeof(wire), &decoded);
    }
    assert(track.n == TRACK_SNAPS);
    interp_ms = 0; track_update(&track, 0);
    assert(memcmp(&track.view.canopy,&track.snaps[track.n-1].canopy,sizeof(source.canopy))==0);
    source.canopy.present=0;
    n=snap_encode(&source,wire,sizeof(wire)); assert(n>0);
    assert(snap_decode(wire,n,&decoded)==n && !decoded.canopy.present);
    puts("PASS production snapshots: 10000 lossy/reordered deliveries, bounded decode fuzz, canopy reset");
    return 0;
}
