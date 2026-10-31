/* Portable player-presence policy; no engine pointers travel over the network. */
#ifndef PRESENCE_H
#define PRESENCE_H
#include <stdint.h>
#define CANOPY_WIRE_MAX 84
struct canopy_state {
    uint8_t present, flags, cur, prev;
    uint32_t anim[2][10]; /* native canopy animations 66 (open), 67 (close) */
};
/* Visuals the game keeps as separate gobs on a player's body (kind 0 Max's jetpack, 1 Doc's leaf blower; both built when
 * the item is taken; its air stream is the host's push volume, not a visual; kind 2 Kurt's canopy, built with him:
 * only whether it is shown): present, shown or hidden, which model variant, and the play state of the
 * (at most GEAR_ANIMS) running animations. A mirror builds the same gob, with the game's own attach calls. */
#define GEAR_KINDS 3
#define GEAR_ANIMS 3
#define GEAR_ANIM_DWORDS 10
struct gear_visual {
    uint8_t present, hidden, model, cur, prev, nanims;
    uint8_t id[GEAR_ANIMS];
    uint32_t anim[GEAR_ANIMS][GEAR_ANIM_DWORDS];
};
struct gear_state { struct gear_visual v[GEAR_KINDS]; };
int gear_encode(const struct gear_state *s, unsigned char *out, int size);
int gear_decode(const unsigned char *in, int size, struct gear_state *s);
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
