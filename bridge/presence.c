#include <math.h>
#include <string.h>
#include "presence.h"

static int canopy_valid(const struct canopy_state *s)
{
    int i, j;
    if (s->present > 1) return 0;
    if (!s->present) return 1;
    if ((s->flags & ~0x22) ||
        (s->cur != 66 && s->cur != 67 && s->cur != 255) ||
        (s->prev != 66 && s->prev != 67 && s->prev != 255)) return 0;
    for (i = 0; i < 2; i++)
        for (j = 3; j <= 6; j++) {
            float value;
            memcpy(&value, &s->anim[i][j], 4);
            if (!isfinite(value) || fabsf(value) > 1000000.0f) return 0;
        }
    return 1;
}

int canopy_encode(const struct canopy_state *s, unsigned char *out, int size)
{
    int i, j, k, n = s->present ? CANOPY_WIRE_MAX : 1;
    if (size < n || !canopy_valid(s)) return 0;
    out[0] = s->present;
    if (!s->present) return 1;
    out[1] = s->flags; out[2] = s->cur; out[3] = s->prev;
    for (i = 0; i < 2; i++) for (j = 0; j < 10; j++) for (k = 0; k < 4; k++)
        out[4 + i*40 + j*4 + k] = (unsigned char)(s->anim[i][j] >> (k*8));
    return n;
}

int canopy_decode(const unsigned char *in, int size, struct canopy_state *s)
{
    struct canopy_state next = {0};
    int i, j, k, n;
    if (size < 1) return 0;
    next.present = in[0];
    n = next.present ? CANOPY_WIRE_MAX : 1;
    if (size < n) return 0;
    if (next.present) {
        next.flags = in[1]; next.cur = in[2]; next.prev = in[3];
        for (i = 0; i < 2; i++) for (j = 0; j < 10; j++) for (k = 0; k < 4; k++)
            next.anim[i][j] |= (uint32_t)in[4 + i*40 + j*4 + k] << (k*8);
    }
    if (!canopy_valid(&next)) return 0;
    *s = next;
    return n;
}

int presence_nearest(const float *origin, const struct presence_player *players, int count)
{
    int i, k, best = -1;
    float distance = 0;
    for (i = 0; i < count; i++) {
        float d = 0;
        if (!players[i].eligible) continue;
        for (k = 0; k < 3; k++) {
            float delta = players[i].pos[k] - origin[k];
            d += delta * delta;
        }
        if (!isfinite(d)) continue;
        if (best < 0 || d < distance) { best = i; distance = d; }
    }
    return best;
}

int join_step(struct join_flow *f, double now, int connected, int host_level,
              int level, int pulling, int mismatch, int cancel)
{
    int next;
    if (cancel || f->phase == JOIN_CANCELLED) next = JOIN_CANCELLED;
    else if (mismatch) next = JOIN_MISMATCH;
    else if (connected && level > 0 && level == host_level && !pulling) next = JOIN_PLAYING;
    else if (connected && (pulling || host_level > 0)) {
        if (!f->loading) { f->loading = 1; f->loading_since = now; }
        next = now - f->loading_since >= 60 ? JOIN_TIMEOUT : JOIN_LOADING;
    }
    else if (connected) next = JOIN_WAITING;
    else if (now - f->since >= 60) next = JOIN_TIMEOUT;
    else next = f->seen ? JOIN_LOST : JOIN_CONNECTING;
    if (next != JOIN_LOADING && next != JOIN_TIMEOUT) f->loading = 0;
    if (connected) { f->seen = 1; f->since = now; }
    f->phase = next;
    return next;
}

const char *join_text(int phase)
{
    static const char *text[] = {
        "Connecting to host...", "Waiting for host to start the game",
        "Loading the host's game...", "Connected", "Host disconnected - reconnecting...",
        "Connection or loading timed out. Check the host and retry.",
        "Different mod versions. Everyone must update before joining.", "Join cancelled"
    };
    return phase >= 0 && phase <= JOIN_CANCELLED ? text[phase] : "Connection unavailable";
}
