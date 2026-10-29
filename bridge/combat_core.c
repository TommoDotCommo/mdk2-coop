#include "combat_core.h"
#include <string.h>
#include <math.h>
void cb_reset(struct cb_state *s) { memset(s, 0, sizeof(*s)); }
int cb_validate(struct cb_guard *g, struct cb_event *e, int sender, int host,
                uint16_t epoch, uint64_t now)
{
    int i, slot = e->attacker;
    uint32_t delta;
    uint64_t bit, second = now / 1000;
    if (slot >= CB_PLAYERS || sender < 0 || sender >= CB_PLAYERS ||
        (host ? sender != slot : sender != 0)) return 0;
    if (e->epoch != epoch) return 0;
    if (!e->id || !e->target || e->kind < 200 || e->kind > 259 ||
        !e->amount || e->amount > 32767 || e->weapon > 4095 ||
        (e->flags & ~(CB_KILL | CB_CRIT)) || e->reserved) return 0;
    for (i = 0; i < 3; i++)
        if (!isfinite(e->pos[i]) || fabsf(e->pos[i]) > 1000000) return 0;
    if (host == CB_REPORT && e->amount > CB_MAX_DAMAGE) e->amount = CB_MAX_DAMAGE;
    /* A 64-event replay window admits reordered reliable deliveries, including wrap. */
    delta = e->id - g->newest[slot];
    if (!g->seen[slot] || (int32_t)delta > 0) {
        bit = 1;
    } else {
        delta = g->newest[slot] - e->id;
        if (delta >= 64 || (g->seen[slot] & (UINT64_C(1) << delta))) return 0;
        bit = UINT64_C(1) << delta;
    }
    if (g->second[slot] != second) { g->second[slot] = second; g->count[slot] = 0; }
    if (host && g->count[slot] >= CB_RATE) return 0;
    g->count[slot]++;
    if (bit == 1) {
        delta = e->id - g->newest[slot];
        g->seen[slot] = delta >= 64 ? 1 : (g->seen[slot] << delta) | 1;
        g->newest[slot] = e->id;
    } else g->seen[slot] |= bit;
    return 1;
}
int cb_attack_valid(int type, int part)
{
    return type > 0 && type <= 65535 && part >= -1 && part <= 64;
}
int cb_validate_hit(struct cb_guard *g, struct cb_event *e, int sender, uint16_t epoch,
                    uint32_t target, int kind, int hp, const float *pos, uint64_t now)
{
    int i;
    if (e->target != target || e->kind != kind || hp <= 0) return 0;
    for (i = 0; i < 3; i++)
        if (!isfinite(pos[i]) || !isfinite(e->pos[i]) || fabsf(e->pos[i] - pos[i]) > 64) return 0;
    return cb_validate(g, e, sender, CB_REPORT, epoch, now);
}
int cb_accept(struct cb_state *s, struct cb_event *e, int sender, int host,
              uint16_t epoch, uint64_t now)
{
    unsigned bucket = (unsigned)(now / 100 % 51);
    int p;
    if (!cb_validate(&s->guard, e, sender, host, epoch, now)) return 0;
    if (!s->started || now - s->last >= CB_IDLE) {
        memset(s->fight, 0, sizeof(s->fight));
        s->start = now; s->started = 1;
    }
    s->last = now;
    s->total[e->attacker] += e->amount;
    s->fight[e->attacker] += e->amount;
    if (s->ticks[bucket] != now / 100 + 1) {
        for (p = 0; p < CB_PLAYERS; p++) s->buckets[p][bucket] = 0;
        s->ticks[bucket] = now / 100 + 1;
    }
    s->buckets[e->attacker][bucket] += e->amount;
    s->lines[s->next].e = *e; s->lines[s->next].at = now;
    s->next = (s->next + 1) % CB_KEEP;
    if (s->size < CB_KEEP) s->size++;
    return 1;
}
double cb_rolling(const struct cb_state *s, int slot, uint64_t now)
{
    uint64_t sum = 0, tick = now / 100 + 1;
    int i;
    for (i = 0; i < 51; i++)
        if (s->ticks[i] && tick >= s->ticks[i] && tick - s->ticks[i] < 50)
            sum += s->buckets[slot][i];
    return sum / 5.0;
}
double cb_fight_dps(const struct cb_state *s, int slot, uint64_t now)
{
    uint64_t end = now - s->last >= CB_IDLE ? s->last : now;
    double seconds = s->started ? (end - s->start) / 1000.0 : 0;
    return s->fight[slot] / (seconds < 1 ? 1 : seconds);
}
