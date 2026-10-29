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
    /* A teleport (a step far beyond anything walked in one snapshot interval) is not walked through: the view is
     * at the old place or the new one, never in the rooms between, whichever frame samples it. The same holds when
     * the newest snapshot is older than the render time and the last step would be extrapolated. */
    {
        struct track tp = {0};
        struct snap a = {0}, b = {0};
        double c;

        a.quat[0] = b.quat[0] = 1; a.cur = a.prev = b.cur = b.prev = NO_ANIM;
        a.time = 1000; a.pos[0] = 100.0f;
        b.time = 1066; b.pos[0] = 5100.0f;
        track_add(&tp, &a); track_add(&tp, &b);
        interp_ms = 100;
        for (c = 1000; c < 1500; c += 1.0) {
            clock_now = c + interp_ms;
            track_update(&tp, 0);
            if (!(tp.view.pos[0] == 100.0f || tp.view.pos[0] == 5100.0f))
                fprintf(stderr, "teleport crossed: render %.0f ms after the first snapshot shows x=%.1f\n", c - 1000, tp.view.pos[0]);
            assert(tp.view.pos[0] == 100.0f || tp.view.pos[0] == 5100.0f);
        }
        /* walking is still smooth */
        b.pos[0] = 108.0f; tp.n = 0; track_add(&tp, &a); track_add(&tp, &b);
        clock_now = 1033 + interp_ms; track_update(&tp, 0);
        assert(tp.view.pos[0] > 100.5f && tp.view.pos[0] < 107.5f);
    }
    puts("PASS production snapshots: 10000 lossy/reordered deliveries, bounded decode fuzz, canopy reset");
    return 0;
}
