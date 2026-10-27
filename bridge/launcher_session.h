/* Portable pregame lifecycle. No Windows, sockets, UI or game dependencies. */
#ifndef LAUNCHER_SESSION_H
#define LAUNCHER_SESSION_H
#include <stdint.h>
enum launcher_phase { LS_OFF, LS_WAITING, LS_LOBBY, LS_COUNTDOWN, LS_LOST, LS_START };
struct launcher_session { enum launcher_phase phase; int host; uint32_t seen, deadline; };
static void ls_open(struct launcher_session *s, int host, uint32_t now)
{
    s->host = host; s->seen = now; s->deadline = 0;
    s->phase = host ? LS_LOBBY : LS_WAITING;
}
static void ls_leave(struct launcher_session *s)
{
    s->phase = LS_OFF; s->deadline = 0;
}
/* The adapter validates endpoint, roster syntax, membership and versions first. */
static int ls_roster(struct launcher_session *s, uint32_t now, int left, int valid)
{
    if (s->host || s->phase == LS_OFF || s->phase == LS_START || !valid || left < -1 || left > 3000) return 0;
    s->seen = now;
    if (left < 0) { s->phase = LS_LOBBY; s->deadline = 0; }
    else {
        /* Do not extend a countdown on each heartbeat. */
        if (s->phase != LS_COUNTDOWN) s->deadline = now + (uint32_t)left;
        s->phase = LS_COUNTDOWN;
    }
    return 1;
}
static int ls_begin(struct launcher_session *s, uint32_t now, int delay, int compatible)
{
    if (!s->host || s->phase != LS_LOBBY || !compatible || delay < 0 || delay > 3000) return 0;
    s->phase = LS_COUNTDOWN; s->deadline = now + (uint32_t)delay;
    return 1;
}
static void ls_tick(struct launcher_session *s, uint32_t now)
{
    if (!s->host && (s->phase == LS_LOBBY || s->phase == LS_COUNTDOWN) && (uint32_t)(now - s->seen) > 4000) {
        s->phase = LS_LOST; s->deadline = 0;
    }
    if (s->phase == LS_COUNTDOWN && (int32_t)(now - s->deadline) >= 0) {
        if (!s->host && (uint32_t)(now - s->seen) > 1000) {
            s->phase = LS_LOST; s->deadline = 0;
        } else s->phase = LS_START;
    }
}
#endif
