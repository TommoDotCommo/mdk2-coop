#include "combat_hud.h"
#include <stdio.h>
#include <string.h>

void cbh_reset(struct cbh_state *s) { memset(s, 0, sizeof(*s)); }

const char *cbh_enemy_name(int kind)
{
    switch (kind) {
    case 200: return "Bif";
    case 201: return "Smite";
    case 202: return "Grunt";
    case 203: return "Conehead";
    case 205: return "Hoser";
    case 207: return "Doganboy";
    case 208: return "Birdbrain";
    case 209: return "Turret";
    case 210: return "Bad Max";
    case 211: return "BFB";
    case 214: return "Ultra Dog";
    case 215: return "Fire Smite";
    case 217: return "Poopsy";
    case 219: return "Cloak Grunt";
    case 230: return "Turret";
    case 240: return "Doom Sphere";
    case 250: case 251: case 252: case 253: case 254: return "Civilian";
    default: return "Enemy";
    }
}

static unsigned pop_life(const struct cbh_pop *p) { return p->kill ? CBH_KILL_MS : CBH_POP_MS; }

void cbh_note(struct cbh_state *s, const struct cb_event *e, int mine, uint64_t now)
{
    struct cbh_pop *p = NULL;
    unsigned i;
    int kill = (e->flags & CB_KILL) != 0;
    for (i = 0; i < CBH_POPS; i++) {
        struct cbh_pop *q = &s->pops[i];
        if (q->used && !q->kill && q->target == e->target && q->attacker == e->attacker &&
            now - q->last <= CBH_MERGE_MS && now - q->born <= CBH_MERGE_CAP_MS) { p = q; break; }
    }
    if (p) {
        p->amount += e->amount; p->hits++; p->last = now;
        if (kill) p->kill = 1;
    } else {
        struct cbh_pop *pick = &s->pops[0];
        for (i = 0; i < CBH_POPS; i++) {
            struct cbh_pop *q = &s->pops[i];
            if (!q->used || now - q->last >= pop_life(q)) { pick = q; break; }
            if (q->last < pick->last) pick = q;
        }
        memset(pick, 0, sizeof(*pick));
        pick->used = 1; pick->target = e->target; pick->attacker = e->attacker;
        pick->kind = e->kind; pick->amount = e->amount; pick->hits = 1;
        pick->kill = (uint8_t)kill; pick->born = pick->last = now;
        p = pick;
    }
    memcpy(p->pos, e->pos, sizeof(p->pos));
    if (mine) { s->hit_at = now; s->hit_kill = kill; s->hit_valid = 1; }
    if (kill) {
        struct cbh_tick *t = &s->ticks[0];
        if (s->nticks && t->attacker == e->attacker && t->kind == e->kind && now - t->at < CBH_TICK_MS) {
            if (t->count < 99) t->count++;
            t->at = now;
        } else {
            for (i = CBH_TICKS - 1; i > 0; i--) s->ticks[i] = s->ticks[i - 1];
            s->ticks[0].attacker = e->attacker; s->ticks[0].kind = e->kind;
            s->ticks[0].count = 1; s->ticks[0].at = now;
            if (s->nticks < CBH_TICKS) s->nticks++;
        }
    }
}

void cbh_peek(struct cbh_state *s, uint64_t now) { s->peek_until = now + CBH_PEEK_MS; }

int cbh_pop_style(const struct cbh_pop *p, uint64_t now, struct cbh_style *out)
{
    unsigned life = pop_life(p);
    uint64_t age = now >= p->last ? now - p->last : 0, total = now >= p->born ? now - p->born : 0;
    float t;
    if (!p->used || age >= life) return 0;
    t = (float)age / (float)life;
    out->alpha = t < 0.6f ? 1.0f : 1.0f - (t - 0.6f) / 0.4f;
    out->rise = total / 400.0f > 2.5f ? 2.5f : total / 400.0f;
    out->scale = age < 120 ? 1.0f + 0.35f * (1.0f - (float)age / 120.0f) : 1.0f;
    out->size = p->kill ? 2 : p->amount >= 25 ? 1 : 0;
    return 1;
}

int cbh_hit_style(const struct cbh_state *s, uint64_t now, float *alpha)
{
    uint64_t age;
    unsigned life = s->hit_kill ? CBH_KILLMARK_MS : CBH_HIT_MS;
    if (!s->hit_valid || now < s->hit_at) return 0;
    age = now - s->hit_at;
    if (age >= life) return 0;
    *alpha = 1.0f - (float)age / (float)life;
    return s->hit_kill ? 2 : 1;
}

int cbh_rank(const struct cb_state *s, int *order, int max)
{
    int all[CB_PLAYERS], p, n = 0, i;
    for (p = 0; p < CB_PLAYERS; p++) {
        if (!s->fight[p]) continue;
        for (i = n; i > 0 && s->fight[all[i - 1]] < s->fight[p]; i--) all[i] = all[i - 1];
        all[i] = p; n++;
    }
    if (n > max) n = max;
    for (i = 0; i < n; i++) order[i] = all[i];
    return n;
}

uint64_t cbh_team_fight(const struct cb_state *s)
{
    uint64_t sum = 0;
    int p;
    for (p = 0; p < CB_PLAYERS; p++) sum += s->fight[p];
    return sum;
}

uint64_t cbh_fight_ms(const struct cb_state *s, uint64_t now)
{
    uint64_t end;
    if (!s->started) return 0;
    end = now >= s->last && now - s->last >= CB_IDLE ? s->last : now;
    return end > s->start ? end - s->start : 0;
}

double cbh_team_dps(const struct cb_state *s, uint64_t now)
{
    double sec = cbh_fight_ms(s, now) / 1000.0;
    return cbh_team_fight(s) / (sec < 1 ? 1 : sec);
}

float cbh_panel_alpha(const struct cb_state *s, const struct cbh_state *h, uint64_t now)
{
    float a = 0;
    if (h->peek_until > now) a = 1;
    if (s->started) {
        uint64_t end = s->last + CB_IDLE;
        if (now <= end) a = 1;
        else if (now - end < CBH_FADE_MS) {
            float f = 1.0f - (float)(now - end) / (float)CBH_FADE_MS;
            if (f > a) a = f;
        }
    }
    return a;
}

void cbh_fmt_count(char *out, unsigned size, uint64_t v)
{
    if (v < 1000) snprintf(out, size, "%u", (unsigned)v);
    else if (v < 10000) snprintf(out, size, "%u.%uk", (unsigned)(v / 1000), (unsigned)(v % 1000 / 100));
    else if (v < 1000000) snprintf(out, size, "%uk", (unsigned)(v / 1000));
    else if (v < 100000000) snprintf(out, size, "%u.%uM", (unsigned)(v / 1000000), (unsigned)(v % 1000000 / 100000));
    else snprintf(out, size, "99M+");
}

void cbh_fmt_time(char *out, unsigned size, uint64_t ms)
{
    unsigned sec = (unsigned)(ms / 1000);
    if (sec > 59 * 60 + 59) sec = 59 * 60 + 59;
    snprintf(out, size, "%u:%02u", sec / 60, sec % 60);
}

int cbh_text_w(int chars, int px) { return chars * px * 62 / 100; }

struct cbh_layout cbh_layout(int w, int h, int px_small, int px_big, int expanded, int rows, int ticks)
{
    struct cbh_layout l;
    int margin = w / 200 < 6 ? 6 : w / 200, maxh = h * 20 / 100, fit, top = h * 22 / 100;
    memset(&l, 0, sizeof(l));
    l.pad = px_small / 3 < 2 ? 2 : px_small / 3;
    l.line_h = px_small * 13 / 10;
    l.row_h = px_big * 12 / 10;
    l.header_h = l.line_h + l.pad;
    fit = (maxh - l.header_h - 2 * l.pad) / l.row_h;
    if (fit < 1) fit = 1;
    if (fit > 8) fit = 8;
    l.rows = rows < fit ? rows : fit;
    l.panel_max.w = l.panel.w = w * 15 / 100;
    l.panel_max.x = l.panel.x = w - margin - l.panel.w;
    l.panel_max.y = l.panel.y = top;
    l.panel_max.h = l.header_h + fit * l.row_h + 2 * l.pad;
    if (l.panel_max.h > maxh) l.panel_max.h = maxh;
    l.panel.h = expanded ? l.header_h + (l.rows ? l.rows : 1) * l.row_h + 2 * l.pad : l.line_h + l.pad;
    if (l.panel.h > l.panel_max.h) l.panel.h = l.panel_max.h;
    l.total_w = cbh_text_w(5, px_small);
    l.dps_w = cbh_text_w(5, px_big);
    l.name_x = l.pad;
    l.name_w = l.panel.w - 2 * l.pad - l.total_w - l.dps_w - 2 * l.pad;
    l.total_x = l.name_x + l.name_w + l.pad;
    l.dps_x = l.total_x + l.total_w + l.pad;
    l.ticker_max.x = l.ticker.x = l.panel.x;
    l.ticker_max.w = l.ticker.w = l.panel.w;
    l.ticker_max.y = top + maxh + l.pad;
    l.ticker_max.h = CBH_TICKS * l.line_h;
    l.ticker.y = l.panel.y + l.panel.h + l.pad;
    l.ticker.h = (ticks > CBH_TICKS ? CBH_TICKS : ticks) * l.line_h;
    l.hit.w = l.hit.h = h * 5 / 100 < 20 ? 20 : h * 5 / 100;
    l.kill_hit.w = l.kill_hit.h = h * 7 / 100 < 28 ? 28 : h * 7 / 100;
    l.hit.x = (w - l.hit.w) / 2; l.hit.y = (h - l.hit.h) / 2;
    l.kill_hit.x = (w - l.kill_hit.w) / 2; l.kill_hit.y = (h - l.kill_hit.h) / 2;
    return l;
}
