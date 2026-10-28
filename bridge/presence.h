/* Portable player-presence policy; no engine pointers travel over the network. */
#ifndef PRESENCE_H
#define PRESENCE_H
#include <stdint.h>
#define CANOPY_WIRE_MAX 84
struct canopy_state {
    uint8_t present, flags, cur, prev;
    uint32_t anim[2][10]; /* native canopy animations 66 (open), 67 (close) */
};
int canopy_encode(const struct canopy_state *s, unsigned char *out, int size);
int canopy_decode(const unsigned char *in, int size, struct canopy_state *s);
struct presence_player { float pos[3]; int eligible; };
int presence_nearest(const float *origin, const struct presence_player *players, int count);
enum { JOIN_CONNECTING, JOIN_WAITING, JOIN_LOADING, JOIN_PLAYING, JOIN_LOST,
       JOIN_TIMEOUT, JOIN_MISMATCH, JOIN_CANCELLED };
struct join_flow { int phase, seen, loading; double since, loading_since; };
int join_step(struct join_flow *flow, double now, int connected, int host_level,
              int level, int pulling, int mismatch, int cancel);
const char *join_text(int phase);
#endif
