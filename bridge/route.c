#include "route.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>

static int fail(char *error, size_t size, const char *why)
{
    snprintf(error, size, "%s", why);
    return 0;
}

static double distance(const struct route_state *a, const struct route_state *b)
{
    double x = a->pos[0] - b->pos[0], y = a->pos[1] - b->pos[1], z = a->pos[2] - b->pos[2];
    return sqrt(x*x + y*y + z*z);
}

static int same_progress(const struct route_state *a, const struct route_state *b)
{
    return a->level == b->level && a->cp == b->cp;
}

static int valid_state(const struct route_state *s)
{
    int i;
    double norm = 0;
    if (s->level < 1 || s->level > 12 || s->cp < 0 || s->cp > 100 || s->type < 0 || s->type > 1000)
        return 0;
    for (i = 0; i < 3; i++)
        if (!isfinite(s->pos[i]) || fabs(s->pos[i]) > 1000000) return 0;
    for (i = 0; i < 4; i++) {
        if (!isfinite(s->rot[i])) return 0;
        norm += s->rot[i] * s->rot[i];
    }
    return norm > 0.9 && norm < 1.1;
}

int route_write_header(FILE *f, const struct route_state *s)
{
    return fprintf(f, "MDKROUTE 1 3902\nS %d %d %d %.6g %.6g %.6g %.6g %.6g %.6g %.6g\n",
                   s->level, s->cp, s->type, s->pos[0], s->pos[1], s->pos[2],
                   s->rot[0], s->rot[1], s->rot[2], s->rot[3]) > 0;
}

int route_write_frame(FILE *f, const struct route_frame *v)
{
    int i;
    const struct route_state *s = &v->state;
    if (fprintf(f, "F %u ", v->ms) < 0) return 0;
    for (i = 0; i < ROUTE_BYTES; i++)
        if (fprintf(f, "%02x", v->input.keys[i]) < 0) return 0;
    return fprintf(f, " %d %d %d %d %d %d %.6g %.6g %.6g %.6g %.6g %.6g %.6g\n",
                   v->input.mouse[0], v->input.mouse[1], v->input.mouse[2], s->level, s->cp, s->type,
                   s->pos[0], s->pos[1], s->pos[2], s->rot[0], s->rot[1], s->rot[2], s->rot[3]) > 0;
}

void route_free(struct route *r)
{
    free(r->frames);
    memset(r, 0, sizeof(*r));
}

/* No executable Lua in recordings. Refuse partial files, junk, nonfinite values and dev keys. */
int route_load(FILE *f, struct route *r, char *error, size_t size)
{
    char line[512], keys[67], extra;
    unsigned int capacity = 0, end;
    int version, build, n, i, finished = 0;
    memset(r, 0, sizeof(*r));
    if (!fgets(line, sizeof(line), f) || sscanf(line, "MDKROUTE %d %d %c", &version, &build, &extra) != 2 ||
        version != 1 || build != 3902)
        return fail(error, size, "unsupported route version or game build");
    if (!fgets(line, sizeof(line), f) ||
        sscanf(line, "S %d %d %d %f %f %f %f %f %f %f %c", &r->start.level, &r->start.cp, &r->start.type,
               &r->start.pos[0], &r->start.pos[1], &r->start.pos[2], &r->start.rot[0], &r->start.rot[1],
               &r->start.rot[2], &r->start.rot[3], &extra) != 10 || !valid_state(&r->start))
        return fail(error, size, "invalid route start");
    while (fgets(line, sizeof(line), f)) {
        struct route_frame v;
        memset(&v, 0, sizeof(v));
        if (sscanf(line, "END %u %c", &end, &extra) == 1) {
            finished = r->count && end == r->frames[r->count - 1].ms;
            break;
        }
        n = sscanf(line, "F %u %66s %d %d %d %d %d %d %f %f %f %f %f %f %f %c", &v.ms, keys,
                   &v.input.mouse[0], &v.input.mouse[1], &v.input.mouse[2], &v.state.level, &v.state.cp,
                   &v.state.type, &v.state.pos[0], &v.state.pos[1], &v.state.pos[2], &v.state.rot[0],
                   &v.state.rot[1], &v.state.rot[2], &v.state.rot[3], &extra);
        if (n != 15 || strlen(keys) != ROUTE_BYTES * 2 || !valid_state(&v.state) ||
            v.ms > ROUTE_MAX_MS || (r->count && v.ms < r->frames[r->count - 1].ms) ||
            r->count >= ROUTE_MAX_FRAMES) goto invalid;
        for (i = 0; i < ROUTE_BYTES; i++) {
            char hex[3] = { keys[i*2], keys[i*2+1], 0 }, *tail;
            unsigned long byte = strtoul(hex, &tail, 16);
            if (*tail || byte > 255 || strspn(hex, "0123456789abcdefABCDEF") != 2) goto invalid;
            v.input.keys[i] = (unsigned char)byte;
        }
        /* Escape and developer/load/save keys would bypass the progression being tested. */
        for (i = 0; i < ROUTE_KEYS; i++)
            if ((i == 1 || (i >= 59 && i <= 68) || i == 87 || i == 88) &&
                (v.input.keys[i/8] & (1 << (i%8)))) goto invalid;
        for (i = 0; i < 3; i++)
            if (v.input.mouse[i] < -100000 || v.input.mouse[i] > 100000) goto invalid;
        if (r->count == capacity) {
            struct route_frame *frames;
            capacity = capacity ? capacity * 2 : 1024;
            frames = realloc(r->frames, capacity * sizeof(*frames));
            if (!frames) { route_free(r); return fail(error, size, "out of memory loading route"); }
            r->frames = frames;
        }
        r->frames[r->count++] = v;
    }
    if (!finished || ferror(f)) goto invalid;
    while ((n = fgetc(f)) != EOF)
        if (n != '\n' && n != '\r' && n != ' ' && n != '\t') goto invalid;
    r->duration = end;
    if (!r->duration || same_progress(&r->start, &r->frames[r->count - 1].state)) {
        route_free(r);
        return fail(error, size, "route must finish beyond its starting checkpoint");
    }
    return 1;
invalid:
    route_free(r);
    return fail(error, size, "invalid or incomplete route (timestamps, state, input or END)");
}

int route_begin(struct route_player *p, const struct route *r, const struct route_state *s,
                char *error, size_t size)
{
    int i;
    double dot = 0;
    if (!valid_state(s) || !same_progress(&r->start, s) || r->start.type != s->type || distance(&r->start, s) > 0.75)
        return fail(error, size, "wrong start level, checkpoint, character or position");
    for (i = 0; i < 4; i++) dot += r->start.rot[i] * s->rot[i];
    if (fabs(dot) < 0.995) return fail(error, size, "wrong start orientation");
    memset(p, 0, sizeof(*p));
    p->route = r;
    p->level = s->level;
    p->cp = s->cp;
    p->drift_since = -1;
    return 1;
}

void route_input_at(struct route_player *p, unsigned int ms, struct route_input *input)
{
    const struct route *r = p->route;
    int i;
    memset(input, 0, sizeof(*input));
    if (ms >= r->duration) return;  /* release every key, also during the final grace window */
    if (p->next) memcpy(input->keys, r->frames[p->next - 1].input.keys, ROUTE_BYTES);
    while (p->next < r->count && r->frames[p->next].ms <= ms) {
        const struct route_input *v = &r->frames[p->next++].input;
        memcpy(input->keys, v->keys, ROUTE_BYTES);
        for (i = 0; i < 3; i++) input->mouse[i] += v->mouse[i];
    }
}

int route_check(struct route_player *p, unsigned int ms, const struct route_state *s,
                char *error, size_t size)
{
    const struct route *r = p->route;
    const struct route_frame *expected;
    unsigned int next = p->milestone, remaining;
    int result;
    while (next < r->count && r->frames[next].state.level == p->level && r->frames[next].state.cp == p->cp) next++;
    if (s->level != p->level || s->cp != p->cp) {
        if (next == r->count || !same_progress(s, &r->frames[next].state)) {
            fail(error, size, "unexpected checkpoint or level transition"); return -1;
        }
        if (ms > r->frames[next].ms + 5000) {
            fail(error, size, "timed out waiting for recorded progression"); return -1;
        }
        p->level = s->level; p->cp = s->cp;
        p->milestone = next; p->progressed++;
    } else if (next < r->count && ms > r->frames[next].ms + 5000) {
        fail(error, size, "timed out waiting for recorded progression"); return -1;
    }
    expected = &r->frames[ms >= r->duration ? r->count - 1 : (p->next ? p->next - 1 : 0)];
    if (same_progress(s, &expected->state) && distance(s, &expected->state) > 12) {
        if (p->drift_since < 0) p->drift_since = ms;
        if (ms - p->drift_since > 2000) { fail(error, size, "position diverged from recorded route"); return -1; }
    } else p->drift_since = -1;
    expected = &r->frames[r->count - 1];
    remaining = p->milestone;
    while (remaining < r->count && r->frames[remaining].state.level == p->level &&
           r->frames[remaining].state.cp == p->cp) remaining++;
    result = ms >= r->duration && remaining == r->count && p->progressed && same_progress(s, &expected->state) &&
             s->type == expected->state.type && distance(s, &expected->state) <= 4;
    if (!result && ms > r->duration + 5000) { fail(error, size, "recorded endpoint not reached"); return -1; }
    return result;
}
