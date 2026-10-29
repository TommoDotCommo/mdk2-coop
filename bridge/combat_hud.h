/* Portable combat presentation core: damage-number merging, kill ticker, panel ranking and
 * layout. No engine pointers or platform dependencies; the HUD only draws what this returns. */
#ifndef COMBAT_HUD_H
#define COMBAT_HUD_H
#include <stdint.h>
#include "combat_core.h"
#define CBH_POPS 24
#define CBH_TICKS 3
#define CBH_MERGE_MS 700        /* a further hit by the same player on the same enemy grows the number */
#define CBH_MERGE_CAP_MS 2500   /* ... but a number older than this starts afresh */
#define CBH_POP_MS 900          /* life after the last merged hit */
#define CBH_KILL_MS 1400
#define CBH_TICK_MS 6000
#define CBH_FADE_MS 2000        /* the panel fades this long after the fight ends */
#define CBH_PEEK_MS 3000        /* toggling the panel shows it this long even between fights */
#define CBH_HIT_MS 170
#define CBH_KILLMARK_MS 420
#define CBH_PLAYER_CHARS 9
#define CBH_ENEMY_CHARS 11
struct cbh_pop {
    uint32_t target, amount;
    uint16_t kind, hits;
    uint8_t attacker, kill, used;
    uint64_t born, last;
    float pos[3];
};
struct cbh_tick { uint8_t attacker; uint16_t kind, count; uint64_t at; };
struct cbh_state {
    struct cbh_pop pops[CBH_POPS];
    struct cbh_tick ticks[CBH_TICKS];     /* newest first */
    unsigned nticks;
    uint64_t hit_at, peek_until;
    int hit_kill, hit_valid;
};
struct cbh_style { float alpha, rise, scale; int size; };   /* size: 0 normal, 1 big, 2 kill */
struct cbh_rect { int x, y, w, h; };
struct cbh_layout {
    struct cbh_rect panel, panel_max, ticker, ticker_max, hit, kill_hit;
    int pad, row_h, line_h, header_h, rows;
    int name_x, name_w, total_x, total_w, dps_x, dps_w;   /* row columns, relative to the panel */
};
void cbh_reset(struct cbh_state *s);
/* mine: the local player dealt it (hitmarker). Called only for accepted, validated events. */
void cbh_note(struct cbh_state *s, const struct cb_event *e, int mine, uint64_t now);
void cbh_peek(struct cbh_state *s, uint64_t now);
const char *cbh_enemy_name(int kind);
int cbh_pop_style(const struct cbh_pop *p, uint64_t now, struct cbh_style *out);
/* 0 when no hitmarker is showing; else 1 hit, 2 kill, with its fade in *alpha */
int cbh_hit_style(const struct cbh_state *s, uint64_t now, float *alpha);
int cbh_rank(const struct cb_state *s, int *order, int max);
uint64_t cbh_team_fight(const struct cb_state *s);
double cbh_team_dps(const struct cb_state *s, uint64_t now);
uint64_t cbh_fight_ms(const struct cb_state *s, uint64_t now);
float cbh_panel_alpha(const struct cb_state *s, const struct cbh_state *h, uint64_t now);
void cbh_fmt_count(char *out, unsigned size, uint64_t v);   /* 987, 1.2k, 12k, 1.2M: at most 5 chars */
void cbh_fmt_time(char *out, unsigned size, uint64_t ms);   /* m:ss */
/* Conservative upper bound for a glyph run, used by the layout and its tests. */
int cbh_text_w(int chars, int px);
struct cbh_layout cbh_layout(int w, int h, int px_small, int px_big, int expanded, int rows, int ticks);
#endif
