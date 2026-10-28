/* Pointer-free animation snapshots, shared with the portable simulator. */
#ifndef SNAPSHOT_H
#define SNAPSHOT_H
#include <stdint.h>
#include "presence.h"
#define ANIM_STATE_DWORDS   10      /* flags, mode, times, rate, weight: plain data, no pointers */
#define ANIM_TIME0          3       /* [3] and [4]: normalised play position, 0..1 */
#define ANIM_TIME1          4
#define ANIM_WEIGHT         6
#define SNAP_ANIMS          8
#define NO_ANIM             0xff

struct anim_entry {
    unsigned char id;
    uint32_t state[ANIM_STATE_DWORDS];
};

/* One sampled state of a gob: what a snapshot carries and what the puppet shows */
struct snap {
    struct canopy_state canopy;
    unsigned int time;          /* sender clock, ms */
    float pos[3];
    float quat[4];              /* w, x, y, z */
    unsigned char cur, prev, nanims;
    struct anim_entry anims[SNAP_ANIMS];
};

#define TRACK_SNAPS 16

struct track {
    struct snap snaps[TRACK_SNAPS];     /* ordered by sender time, oldest first */
    int n;
    struct snap view;                   /* interpolated state for this frame */
};

#endif
