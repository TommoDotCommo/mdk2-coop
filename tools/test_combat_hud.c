/* Game-free tests of the combat presentation core: merge rules, ticker, ranking, fade and the
 * layout of every HUD element at five resolutions. Text is modelled with cbh_text_w(), a
 * deliberately wide per-glyph bound; the renderer still measures and clips with the real font. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "combat_hud.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); exit(1); } } while (0)
static struct cb_event hit(unsigned target, int slot, int amount, int flags, int kind)
{
    struct cb_event e = {0};
    e.id = 1; e.target = target; e.attacker = slot; e.amount = amount; e.flags = flags;
    e.kind = kind; e.epoch = 7; e.weapon = 2049; e.pos[0] = 10; e.pos[1] = 20; e.pos[2] = 5;
    return e;
}
static int used(const struct cbh_state *s)
{
    int i, n = 0;
    for (i = 0; i < CBH_POPS; i++) n += s->pops[i].used;
    return n;
}
static const struct cbh_pop *find(const struct cbh_state *s, unsigned target, int slot)
{
    int i;
    for (i = 0; i < CBH_POPS; i++)
        if (s->pops[i].used && s->pops[i].target == target && s->pops[i].attacker == slot) return &s->pops[i];
    return NULL;
}
static struct cbh_state st;

static void merging(void)
{
    struct cb_event e;
    const struct cbh_pop *p;
    uint64_t t;
    cbh_reset(&st);
    e = hit(42, 1, 20, 0, 207); cbh_note(&st, &e, 0, 1000);
    e = hit(42, 1, 15, 0, 207); cbh_note(&st, &e, 0, 1100);
    e = hit(42, 1, 5, 0, 207); cbh_note(&st, &e, 0, 1250);
    CHECK(used(&st) == 1);
    p = find(&st, 42, 1); CHECK(p && p->amount == 40 && p->hits == 3 && !p->kill);
    CHECK(p->last == 1250 && p->born == 1000);
    /* another player on the same enemy, and the same player on another enemy: separate numbers */
    e = hit(42, 2, 7, 0, 207); cbh_note(&st, &e, 0, 1300);
    e = hit(43, 1, 9, 0, 207); cbh_note(&st, &e, 0, 1300);
    CHECK(used(&st) == 3);
    CHECK(find(&st, 42, 2) && find(&st, 42, 2)->attacker == 2 && find(&st, 42, 2)->amount == 7);
    CHECK(find(&st, 43, 1) && find(&st, 43, 1)->amount == 9);
    CHECK(find(&st, 42, 1)->amount == 40);
    /* after the merge window a hit begins a new number */
    e = hit(42, 1, 11, 0, 207); cbh_note(&st, &e, 0, 1250 + CBH_MERGE_MS + 1);
    CHECK(used(&st) == 4);
    /* exactly at the window edge it still merges */
    cbh_reset(&st);
    e = hit(5, 3, 10, 0, 202); cbh_note(&st, &e, 0, 5000);
    e = hit(5, 3, 10, 0, 202); cbh_note(&st, &e, 0, 5000 + CBH_MERGE_MS);
    CHECK(used(&st) == 1 && find(&st, 5, 3)->amount == 20);
    /* sustained fire: a number may not grow for ever */
    cbh_reset(&st);
    for (t = 0; t <= 6000; t += 300) { e = hit(9, 0, 10, 0, 207); cbh_note(&st, &e, 0, 10000 + t); }
    CHECK(used(&st) >= 2 && used(&st) <= 4);
    /* a kill joins the growing number, marks it, and later hits start a new number */
    cbh_reset(&st);
    e = hit(8, 1, 30, 0, 207); cbh_note(&st, &e, 0, 100);
    e = hit(8, 1, 20, CB_KILL, 207); cbh_note(&st, &e, 0, 200);
    p = find(&st, 8, 1); CHECK(used(&st) == 1 && p->kill && p->amount == 50 && p->hits == 2);
    e = hit(8, 1, 5, 0, 207); cbh_note(&st, &e, 0, 300);
    CHECK(used(&st) == 2);
    /* the pool is bounded and keeps the newest */
    cbh_reset(&st);
    for (t = 0; t < 200; t++) { e = hit(1000 + (unsigned)t, t % 16, 10, 0, 207); cbh_note(&st, &e, 0, 20000 + t); }
    CHECK(used(&st) == CBH_POPS && find(&st, 1199, 199 % 16) != NULL);
    puts("PASS rapid hits merge per player and enemy, kills end a number, pool is bounded");
}

static void styles(void)
{
    struct cb_event e;
    struct cbh_style a, b;
    const struct cbh_pop *p;
    float al;
    cbh_reset(&st);
    e = hit(1, 0, 20, 0, 207); cbh_note(&st, &e, 0, 1000);
    e = hit(2, 0, 30, CB_KILL, 207); cbh_note(&st, &e, 0, 1000);
    p = find(&st, 1, 0); CHECK(cbh_pop_style(p, 1000, &a) && a.size == 0 && a.alpha == 1.0f);
    CHECK(a.scale > 1.2f);                                  /* a fresh or merged number pops */
    CHECK(cbh_pop_style(p, 1000 + 200, &a) && a.scale == 1.0f);
    CHECK(cbh_pop_style(p, 1000 + CBH_POP_MS - 1, &a) && a.alpha > 0 && a.alpha < 0.1f);
    CHECK(!cbh_pop_style(p, 1000 + CBH_POP_MS, &a));        /* short fade, then gone */
    p = find(&st, 2, 0); CHECK(cbh_pop_style(p, 1000 + CBH_POP_MS, &b) && b.size == 2);
    CHECK(cbh_pop_style(p, 1000 + CBH_KILL_MS - 1, &b) && !cbh_pop_style(p, 1000 + CBH_KILL_MS, &b));
    CHECK(CBH_KILL_MS > CBH_POP_MS);
    CHECK(cbh_pop_style(p, 1700 - 300, &a) && a.rise > 0 && a.rise <= 2.5f);
    e = hit(3, 0, 150, 0, 207); cbh_note(&st, &e, 0, 5000);
    CHECK(cbh_pop_style(find(&st, 3, 0), 5000, &a) && a.size == 1);   /* a big or grown number is bigger */
    /* the local player's hit raises the hitmarker; a teammate's does not */
    cbh_reset(&st);
    e = hit(4, 1, 10, 0, 207); cbh_note(&st, &e, 0, 100);
    CHECK(!cbh_hit_style(&st, 100, &al));
    cbh_note(&st, &e, 1, 200);
    CHECK(cbh_hit_style(&st, 200, &al) == 1 && al == 1.0f);
    CHECK(!cbh_hit_style(&st, 200 + CBH_HIT_MS, &al));
    e = hit(4, 1, 10, CB_KILL, 207); cbh_note(&st, &e, 1, 300);
    CHECK(cbh_hit_style(&st, 300 + CBH_HIT_MS, &al) == 2);  /* kills flash longer */
    CHECK(!cbh_hit_style(&st, 300 + CBH_KILLMARK_MS, &al));
    puts("PASS number life, fade, kill style and hitmarker");
}

static void ticker(void)
{
    struct cb_event e;
    cbh_reset(&st);
    e = hit(1, 1, 10, CB_KILL, 207); cbh_note(&st, &e, 0, 100);
    e = hit(2, 1, 10, CB_KILL, 207); cbh_note(&st, &e, 0, 900);
    CHECK(st.nticks == 1 && st.ticks[0].count == 2);
    e = hit(3, 1, 10, 0, 207); cbh_note(&st, &e, 0, 950);        /* a plain hit is not a kill */
    CHECK(st.nticks == 1 && st.ticks[0].count == 2);
    e = hit(4, 2, 10, CB_KILL, 207); cbh_note(&st, &e, 0, 1000);
    e = hit(5, 2, 10, CB_KILL, 202); cbh_note(&st, &e, 0, 1100);
    e = hit(6, 3, 10, CB_KILL, 203); cbh_note(&st, &e, 0, 1200);
    e = hit(7, 3, 10, CB_KILL, 205); cbh_note(&st, &e, 0, 1300);
    CHECK(st.nticks == CBH_TICKS);                                 /* never more than three lines */
    CHECK(st.ticks[0].kind == 205 && st.ticks[1].kind == 203 && st.ticks[2].kind == 202);
    e = hit(8, 3, 10, CB_KILL, 205); cbh_note(&st, &e, 0, 1400);
    CHECK(st.nticks == CBH_TICKS && st.ticks[0].count == 2);
    e = hit(9, 3, 10, CB_KILL, 205); cbh_note(&st, &e, 0, 1400 + CBH_TICK_MS + 1);
    CHECK(st.ticks[0].count == 1 && st.ticks[1].count == 2);   /* an old streak is not extended */
    puts("PASS kill ticker: merged streaks, three lines, newest first");
}

static void names_and_format(void)
{
    char buf[16];
    int k, i;
    uint64_t values[] = {0, 1, 999, 1000, 1234, 9999, 10000, 54321, 999999, 1000000, 1234567, 99999999, 100000000, 4000000000u};
    CHECK(!strcmp(cbh_enemy_name(207), "Doganboy"));
    CHECK(!strcmp(cbh_enemy_name(0), "Enemy") && !strcmp(cbh_enemy_name(259), "Enemy") && !strcmp(cbh_enemy_name(204), "Enemy"));
    for (k = 0; k < 400; k++) {
        const char *n = cbh_enemy_name(k);
        CHECK(n && n[0] && (int)strlen(n) <= CBH_ENEMY_CHARS);
        for (i = 0; n[i]; i++) CHECK(n[i] < '0' || n[i] > '9');   /* never a raw id */
    }
    for (i = 0; i < (int)(sizeof(values) / sizeof(values[0])); i++) {
        cbh_fmt_count(buf, sizeof(buf), values[i]);
        CHECK(strlen(buf) <= 5 && buf[0]);
    }
    cbh_fmt_count(buf, sizeof(buf), 999); CHECK(!strcmp(buf, "999"));
    cbh_fmt_count(buf, sizeof(buf), 1234); CHECK(!strcmp(buf, "1.2k"));
    cbh_fmt_count(buf, sizeof(buf), 54321); CHECK(!strcmp(buf, "54k"));
    cbh_fmt_count(buf, sizeof(buf), 1234567); CHECK(!strcmp(buf, "1.2M"));
    cbh_fmt_time(buf, sizeof(buf), 0); CHECK(!strcmp(buf, "0:00"));
    cbh_fmt_time(buf, sizeof(buf), 75999); CHECK(!strcmp(buf, "1:15"));
    cbh_fmt_time(buf, sizeof(buf), 99999999); CHECK(!strcmp(buf, "59:59"));
    puts("PASS friendly enemy names (no ids, fallback) and compact number formats");
}

static void panel(void)
{
    static struct cb_state s;
    struct cb_event e;
    int order[8], n;
    float a;
    cb_reset(&s); cbh_reset(&st);
    CHECK(cbh_panel_alpha(&s, &st, 1000) == 0);                /* nothing before the first hit */
    e = hit(1, 2, 50, 0, 207); e.id = 1; CHECK(cb_accept(&s, &e, 2, CB_PUBLISH, 7, 1000));
    e = hit(1, 0, 90, 0, 207); e.id = 2; CHECK(cb_accept(&s, &e, 0, CB_PUBLISH, 7, 1500));
    e = hit(1, 5, 50, 0, 207); e.id = 3; CHECK(cb_accept(&s, &e, 5, CB_PUBLISH, 7, 2000));
    n = cbh_rank(&s, order, 8);
    CHECK(n == 3 && order[0] == 0 && order[1] == 2 && order[2] == 5);    /* by damage, ties by slot */
    n = cbh_rank(&s, order, 2); CHECK(n == 2 && order[0] == 0 && order[1] == 2);
    CHECK(cbh_team_fight(&s) == 190);
    CHECK(fabs(cbh_team_dps(&s, 3000) - 95.0) < 0.001);         /* 190 over 2 s */
    CHECK(cbh_fight_ms(&s, 3000) == 2000);
    CHECK(cbh_fight_ms(&s, 2000 + CB_IDLE + 50000) == 1000);   /* frozen at the last hit */
    CHECK(cbh_panel_alpha(&s, &st, 2000 + CB_IDLE) == 1.0f);   /* shown for the whole fight */
    a = cbh_panel_alpha(&s, &st, 2000 + CB_IDLE + CBH_FADE_MS / 2);
    CHECK(a > 0.4f && a < 0.6f);                               /* then fades ... */
    CHECK(cbh_panel_alpha(&s, &st, 2000 + CB_IDLE + CBH_FADE_MS) == 0);   /* ... and goes */
    cbh_peek(&st, 100000);                                     /* toggling shows it between fights */
    CHECK(cbh_panel_alpha(&s, &st, 100000 + CBH_PEEK_MS - 1) == 1.0f);
    CHECK(cbh_panel_alpha(&s, &st, 100000 + CBH_PEEK_MS) == 0);
    puts("PASS ranking by damage, fight timer, team DPS, fade after the fight and toggle peek");
}

struct zone { const char *name; int x0, y0, x1, y1; };      /* percent of the screen */
static int overlaps(struct cbh_rect r, int w, int h, struct zone z)
{
    int zx0 = w * z.x0 / 100, zx1 = w * z.x1 / 100, zy0 = h * z.y0 / 100, zy1 = h * z.y1 / 100;
    return r.x < zx1 && r.x + r.w > zx0 && r.y < zy1 && r.y + r.h > zy0;
}
static int rects_overlap(struct cbh_rect a, struct cbh_rect b)
{
    return a.x < b.x + b.w && a.x + a.w > b.x && a.y < b.y + b.h && a.y + a.h > b.y;
}
static int inside(struct cbh_rect r, int w, int h)
{
    return r.x >= 0 && r.y >= 0 && r.x + r.w <= w && r.y + r.h <= h;
}
static void layouts(void)
{
    static const int sizes[][2] = {{1280, 720}, {1920, 1080}, {2560, 1440}, {3840, 2160}, {2560, 1080}};
    /* Reserved: the team panel (top left), chat (bottom left), the game's own health and ammo, the
     * mic and cutscene boxes (bottom strip), the centred status and banner lines, and the screen centre. */
    static const struct zone zones[] = {
        {"team panel", 0, 0, 50, 60}, {"chat", 0, 70, 45, 100}, {"bottom strip", 0, 86, 100, 100},
        {"status and banner", 30, 55, 70, 90}, {"centre", 35, 25, 65, 75},
        {"tutorial and dialog boxes", 40, 0, 100, 35}};
    unsigned i, z;
    int rows, expanded, ticks;
    for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        int w = sizes[i][0], h = sizes[i][1];
        int small = h / 64 < 10 ? 10 : h / 64, big = h / 42 < 13 ? 13 : h / 42;
        for (expanded = 0; expanded < 2; expanded++)
        for (rows = 0; rows <= 8; rows++)
        for (ticks = 0; ticks <= 3; ticks++) {
            struct cbh_layout l = cbh_layout(w, h, small, big, expanded, rows, ticks);
            CHECK(l.panel.w <= w * 15 / 100 && l.panel_max.w <= w * 15 / 100);
            CHECK(l.panel_max.h <= h * 20 / 100 && l.panel.h <= l.panel_max.h);
            CHECK(l.panel.x == l.panel_max.x && l.panel.y == l.panel_max.y);
            CHECK(inside(l.panel_max, w, h) && inside(l.panel, w, h) && inside(l.ticker_max, w, h));
            CHECK(inside(l.ticker, w, h) && inside(l.hit, w, h) && inside(l.kill_hit, w, h));
            CHECK(l.panel.x >= w / 2);                                /* a screen-edge panel */
            /* a safe margin: never flush with the edge, at least 12 px and 2 percent of the width */
            CHECK(w - (l.panel.x + l.panel.w) >= 12 && w - (l.panel.x + l.panel.w) >= w * 2 / 100);
            CHECK(w - (l.panel_max.x + l.panel_max.w) >= 12 && w - (l.ticker_max.x + l.ticker_max.w) >= 12);
            CHECK(l.panel.y >= h * 35 / 100);                         /* below the dialog zone */
            for (z = 0; z < sizeof(zones) / sizeof(zones[0]); z++) {
                CHECK(!overlaps(l.panel_max, w, h, zones[z]));
                CHECK(!overlaps(l.ticker_max, w, h, zones[z]));
                CHECK(!overlaps(l.ticker, w, h, zones[z]));
            }
            CHECK(!rects_overlap(l.panel, l.ticker) && !rects_overlap(l.panel_max, l.ticker_max));
            CHECK(l.ticker.y + l.ticker.h <= l.ticker_max.y + l.ticker_max.h);
            CHECK(!rects_overlap(l.hit, l.panel_max) && !rects_overlap(l.kill_hit, l.panel_max));
            CHECK(!rects_overlap(l.hit, l.ticker_max) && !rects_overlap(l.kill_hit, l.ticker_max));
            CHECK(l.hit.w <= h * 8 / 100 && l.kill_hit.w <= h * 8 / 100 && l.kill_hit.w > l.hit.w);
            CHECK(l.hit.x + l.hit.w / 2 == w / 2 || l.hit.x + l.hit.w / 2 == w / 2 - 1 || l.hit.x + l.hit.w / 2 == w / 2 + 1);
            /* collapsed is a single thin line */
            if (!expanded) CHECK(l.panel.h <= l.line_h + l.pad && l.panel.h < h * 5 / 100);
            /* rows, header and padding fit the panel; columns are disjoint and inside it */
            CHECK(l.rows <= 8 && l.rows <= rows && l.rows >= 0);
            if (expanded) CHECK(l.header_h + (l.rows ? l.rows : 1) * l.row_h + l.pad <= l.panel.h);
            CHECK(l.row_h >= big && l.line_h >= small && l.header_h > small);
            /* a title line and a column-label line ("Total", "DPS") fit the header above the rows */
            CHECK(l.cols_y >= l.line_h && l.cols_y + l.line_h <= l.header_h);
            if (expanded) CHECK(l.panel.h >= l.header_h + l.row_h);
            CHECK(cbh_text_w((int)strlen("Total"), small) <= l.total_w && cbh_text_w((int)strlen("DPS"), small) <= l.dps_w);
            CHECK(l.name_x >= 0 && l.name_x + l.name_w <= l.total_x);
            CHECK(l.total_x + l.total_w <= l.dps_x && l.dps_x + l.dps_w <= l.panel.w - l.pad + 1);
            CHECK(l.name_w >= cbh_text_w(CBH_PLAYER_CHARS, small));
            CHECK(l.total_w >= cbh_text_w(5, small) && l.dps_w >= cbh_text_w(5, big));
            /* text fits its boxes */
            CHECK(cbh_text_w((int)strlen("DPS  You 99.9k  Team 99.9k"), small) <= l.panel.w - 2 * l.pad);
            CHECK(cbh_text_w((int)strlen("Damage"), small) + cbh_text_w(5, small) + 2 * l.pad <= l.panel.w - 2 * l.pad);
            CHECK(cbh_text_w((int)strlen("Waiting for a fight"), small) <= l.panel.w - 2 * l.pad);
            CHECK(cbh_text_w(CBH_PLAYER_CHARS + 3 + CBH_ENEMY_CHARS + 4, small) <= l.ticker.w - 2 * l.pad);
            CHECK(l.line_h * CBH_TICKS <= l.ticker_max.h);
        }
    }
    puts("PASS layout at 1280x720, 1920x1080, 2560x1440, 3840x2160 and 21:9: sizes, reserved zones, overlaps, text fit");
}
static void avoidance(void)
{
    /* two name tags, one above the other; a number box overlapping them is lifted clear of both */
    float tags[2][4] = {{100, 200, 160, 220}, {100, 170, 160, 190}};
    float dy;
    dy = cbh_avoid_up(110, 195, 150, 215, tags, 2, 4); CHECK(dy < 0);
    CHECK(195 + dy < 170 - 4 + 1 - (215 - 195) + 20 && 215 + dy <= 170 - 4);   /* above the upper tag */
    dy = cbh_avoid_up(110, 205, 150, 225, tags, 1, 4); CHECK(dy < 0 && 225 + dy <= 200 - 4);
    /* clear of every tag: left alone; beside a tag: left alone; below it: left alone */
    CHECK(cbh_avoid_up(10, 195, 50, 215, tags, 2, 4) == 0);
    CHECK(cbh_avoid_up(110, 230, 150, 250, tags, 2, 4) == 0);
    CHECK(cbh_avoid_up(110, 100, 150, 120, tags, 2, 4) == 0);
    CHECK(cbh_avoid_up(110, 195, 150, 215, tags, 0, 4) == 0);
    /* the result never overlaps any tag */
    dy = cbh_avoid_up(110, 175, 150, 195, tags, 2, 4);
    CHECK(!(175 + dy < 190 && 195 + dy > 170) && !(175 + dy < 220 && 195 + dy > 200));
    {   /* stacked tags listed upper first: clearing the lower one lands on the upper one */
        float stack[2][4] = {{100, 170, 160, 190}, {100, 194, 160, 214}};
        dy = cbh_avoid_up(110, 200, 150, 220, stack, 2, 4);
        CHECK(220 + dy <= 170 - 4 + 0.001f);
    }
    puts("PASS floating numbers rise clear of name tags");
}
int main(void) { merging(); styles(); ticker(); names_and_format(); panel(); layouts(); avoidance(); return 0; }
