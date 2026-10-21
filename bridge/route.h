/* Portable recorded-route format and playback oracle (no game or Windows dependencies). */
#ifndef ROUTE_H
#define ROUTE_H
#include <stdio.h>

#define ROUTE_KEYS 264
#define ROUTE_BYTES (ROUTE_KEYS / 8)
#define ROUTE_MAX_FRAMES 216000
#define ROUTE_MAX_MS 3600000

struct route_input { unsigned char keys[ROUTE_BYTES]; int mouse[3]; };
struct route_state { int level, cp, type; float pos[3], rot[4]; };
struct route_frame { unsigned int ms; struct route_input input; struct route_state state; };
struct route {
    struct route_state start;
    struct route_frame *frames;
    unsigned int count, duration;
};
struct route_player {
    const struct route *route;
    unsigned int next, milestone;
    int level, cp, progressed;
    double drift_since;
};

int route_write_header(FILE *f, const struct route_state *start);
int route_write_frame(FILE *f, const struct route_frame *frame);
int route_load(FILE *f, struct route *route, char *error, size_t size);
void route_free(struct route *route);
int route_begin(struct route_player *p, const struct route *route,
                const struct route_state *start, char *error, size_t size);
/* before a scene update: consume due input, retaining held keys and summing mouse motion */
void route_input_at(struct route_player *p, unsigned int ms, struct route_input *input);
/* after a scene update: -1 failed, 0 running, 1 reached the recorded progression and endpoint */
int route_check(struct route_player *p, unsigned int ms, const struct route_state *state,
                char *error, size_t size);
#endif
