/*
 * The player list: hold Tab (the key on the Controls page, GK_LIST) and a list opens over the picture, one row per
 * player: colour, number, name, host mark, speaking light, ping, hero, distance, state (loading, down, out,
 * watching). The mouse stays with the list while it is open (the camera does not turn, the left button does not
 * fire): a pointer you steer with it, and per row Mute, volume - / + and, when you are out of the fight, Watch.
 * Letting go of the key closes it. The always-on strip in the top left (hud.c) stays as the Interface page has it.
 *
 * TODO Kick (host only): the session has no way to put a player out and keep them out (no ban, no message to the
 * client), so the list has no Kick button yet.
 */
#include <stdio.h>
#include <string.h>
#include "bridge.h"

#define MAX_HITS    (MAX_PLAYERS * 4)
#define LIFE_SPECTATING_WIRE 3     /* revive_life(): dead and watching someone (revive.c LIFE_SPECTATING) */

enum { H_MUTE, H_VOLDOWN, H_VOLUP, H_WATCH };

static int is_on;
static float px = -1.0f, py = -1.0f;        /* pointer, window pixels */
static int win_w = 1280, win_h = 720;
static int click_was = 1, clicks;
static struct { int kind, slot; float x, y, w, h; } hit[MAX_HITS];
static int nhits, nrows;
static int last_row_slot[MAX_PLAYERS];

int plist_on(void)
{
    return is_on;
}

static void close_list(void)
{
    if (!is_on)
        return;
    is_on = 0;
    input_mouse_hold(0);
    input_buttons_hold(0);
    nhits = nrows = 0;
    logmsg("plist: closed (%d clicks)", clicks);
}

/* us and every other connected player */
static int listed(int s)
{
    return s == my_slot || player_connected(s);
}

static int inside(int i)
{
    return px >= hit[i].x && px < hit[i].x + hit[i].w && py >= hit[i].y && py < hit[i].y + hit[i].h;
}

static void do_click(void)
{
    int i;

    for (i = 0; i < nhits; i++) {
        if (!inside(i))
            continue;
        clicks++;
        switch (hit[i].kind) {
        case H_MUTE:
            voice_set_muted(hit[i].slot, !voice_muted(hit[i].slot));
            logmsg("plist: player %d %s", hit[i].slot + 1, voice_muted(hit[i].slot) ? "muted" : "unmuted");
            break;
        case H_VOLDOWN:
        case H_VOLUP: {
            int v = voice_player_volume(hit[i].slot) + (hit[i].kind == H_VOLUP ? 20 : -20);

            voice_set_player_volume(hit[i].slot, v);
            logmsg("plist: player %d volume %d%%", hit[i].slot + 1, voice_player_volume(hit[i].slot));
            break;
        }
        case H_WATCH:
            logmsg("plist: watch player %d: %s", hit[i].slot + 1, revive_watch(hit[i].slot) ? "yes" : "no");
            break;
        }
        return;
    }
}

/* every frame, before the revive state: open, steer and close */
void plist_frame(void)
{
    int want = hud_ingame() && have_remote && gamekey_down(GK_LIST) && !guide_on() && !ctrlview_on() &&
               !emote_wheel_open() && local_player;
    long dx, dy;
    int down;

    if (!want) {
        close_list();
        return;
    }
    if (!is_on) {
        is_on = 1;
        input_mouse_hold(1);
        input_buttons_hold(1);
        px = win_w * 0.5f;
        py = win_h * 0.5f;
        click_was = 1;                      /* a button already down is not a click */
        logmsg("plist: opened");
    }
    input_mouse_take(&dx, &dy);
    px += (float)dx;
    py += (float)dy;
    if (px < 0.0f) px = 0.0f;
    if (py < 0.0f) py = 0.0f;
    if (px > win_w - 1.0f) px = win_w - 1.0f;
    if (py > win_h - 1.0f) py = win_h - 1.0f;
    down = input_down(256, VK_LBUTTON);
    if (down && !click_was)
        do_click();
    click_was = down;
}

static const char *state_text(int s, int me)
{
    int life = s == me ? revive_life() : player_life(s);

    if (!player_in_level(s) && s != me)
        return "loading";
    if (life == LIFE_SPECTATING_WIRE)
        return "watching";
    if (life == LIFE_DEAD)
        return s == me && revive_watching() >= 0 ? "watching" : "out";
    if (life == LIFE_DOWNED)
        return "down";
    return "";
}

static void button(float x, float y, float w, float bh, int h, const char *label, int on, int kind, int slot)
{
    float tw = (float)hud_text_w(label);

    hud_box(x, y, w, bh, h, on ? 0.45f : 0.12f, on ? 0.3f : 0.14f, on ? 0.1f : 0.2f, 0.9f);
    hud_text(x + (w - tw) * 0.5f, y + bh * 0.5f + hud_font_h() * 0.35f, h, label, 0.95f, 0.9f, 0.75f);
    if (nhits < MAX_HITS) {
        hit[nhits].kind = kind;
        hit[nhits].slot = slot;
        hit[nhits].x = x;
        hit[nhits].y = y;
        hit[nhits].w = w;
        hit[nhits].h = bh;
        nhits++;
    }
}

void plist_draw(int w, int h)
{
    float u, row, x0, y0, pw, ph, bh;
    int s, rows = 0, me = my_slot, compact, out_of_fight;
    char buf[96];
    float c[3];

    win_w = w;
    win_h = h;
    if (is_on && (!hud_ingame() || !have_remote))      /* a menu came up or the session ended with the key down */
        close_list();
    if (!is_on)
        return;
    u = (float)hud_font_h();
    row = u * 2.0f;
    bh = u * 1.5f;
    for (s = 0; s < MAX_PLAYERS; s++)
        rows += listed(s) ? 1 : 0;
    compact = u * 63.0f > w * 0.96f;
    pw = u * (compact ? 51.0f : 63.0f);
    ph = u * 3.6f + row * rows;
    x0 = (w - pw) * 0.5f;
    y0 = (h - ph) * 0.5f;
    if (y0 < u)
        y0 = u;
    out_of_fight = revive_life() == LIFE_SPECTATING_WIRE;   /* riding along with someone: the only time Watch can work */
    nhits = nrows = 0;

    hud_box(x0 - u, y0 - u, pw + u * 2, ph + u * 2, h, 0.01f, 0.015f, 0.03f, 0.82f);
    hud_box(x0 - u, y0 - u, pw + u * 2, 3, h, 0.95f, 0.65f, 0.15f, 0.9f);
    snprintf(buf, sizeof(buf), "PLAYERS   (hold %s)", guide_list_key());
    hud_text(x0, y0 + u * 0.8f, h, buf, 1.0f, 0.82f, 0.35f);
    for (s = 0; s < MAX_PLAYERS; s++) {
        float y, x, base;
        const char *name, *st;

        if (!listed(s))
            continue;
        y = y0 + u * 2.0f + nrows * row;
        base = y + row * 0.5f + u * 0.35f;
        last_row_slot[nrows++] = s;
        hud_slot_colour(s, c);
        hud_box(x0, y + u * 0.2f, u * 0.5f, row - u * 0.4f, h, c[0], c[1], c[2], 1.0f);
        if (s == me)
            hud_box(x0 + u * 0.7f, y, pw - u * 0.7f, row - 2, h, 1.0f, 1.0f, 1.0f, 0.07f);
        snprintf(buf, sizeof(buf), "%d", s + 1);
        hud_text(x0 + u * 1.0f, base, h, buf, c[0], c[1], c[2]);
        name = player_name(s);
        snprintf(buf, sizeof(buf), "%s%s", name, s == 0 ? "  (host)" : "");
        hud_text(x0 + u * 2.4f, base, h, buf, 0.92f, 0.95f, 1.0f);
        /* speaking light */
        if (voice_talking(s == me ? -1 : s))
            hud_box(x0 + u * 15.0f, y + row * 0.5f - u * 0.3f, u * 0.6f, u * 0.6f, h, 0.3f, 1.0f, 0.4f, 1.0f);
        else
            hud_box(x0 + u * 15.0f, y + row * 0.5f - u * 0.3f, u * 0.6f, u * 0.6f, h, 0.2f, 0.25f, 0.2f, 0.8f);
        x = x0 + u * 16.5f;
        if (s != me && player_ping(s) > 0)
            snprintf(buf, sizeof(buf), "%d ms", player_ping(s));
        else
            snprintf(buf, sizeof(buf), "-");
        hud_text(x, base, h, buf, 0.6f, 0.68f, 0.78f);
        x += u * 5.5f;
        if (!compact) {
            snprintf(buf, sizeof(buf), "%s", hud_hero_name(player_type(s)));
            hud_text(x, base, h, buf, 0.85f, 0.9f, 0.7f);
            x += u * 5.0f;
            if (s != me && player_in_level(s) && hud_dist_to_slot(s) >= 0.0f)
                snprintf(buf, sizeof(buf), "%.0f away", hud_dist_to_slot(s));
            else
                buf[0] = 0;
            hud_text(x, base, h, buf, 0.7f, 0.78f, 0.88f);
            x += u * 7.0f;
        }
        st = state_text(s, me);
        if (s == me && revive_watching() >= 0)
            snprintf(buf, sizeof(buf), "watching %s", player_name(revive_watching()));
        else
            snprintf(buf, sizeof(buf), "%s", st);
        hud_text(x, base, h, buf, 1.0f, st[0] ? 0.55f : 0.8f, st[0] ? 0.4f : 0.8f);
        x = x0 + pw - u * (compact ? 23.0f : 24.0f);
        if (s != me) {
            int muted = voice_muted(s);

            button(x, y + (row - bh) * 0.5f, u * 5.0f, bh, h, muted ? "Unmute" : "Mute", muted, H_MUTE, s);
            x += u * 5.4f;
            button(x, y + (row - bh) * 0.5f, u * 1.6f, bh, h, "-", 0, H_VOLDOWN, s);
            snprintf(buf, sizeof(buf), "%d%%", voice_player_volume(s));
            hud_text(x + u * 1.9f, base, h, buf, 0.9f, 0.9f, 0.9f);
            button(x + u * 5.2f, y + (row - bh) * 0.5f, u * 1.6f, bh, h, "+", 0, H_VOLUP, s);
            x += u * 7.4f;
            if (out_of_fight && player_in_level(s) && player_life(s) == LIFE_ALIVE)
                button(x, y + (row - bh) * 0.5f, u * 4.6f, bh, h, "Watch", revive_watching() == s, H_WATCH, s);
        }
    }
    snprintf(buf, sizeof(buf), "%s: Mute, volume, Watch (when you are out of the fight)   -   let go of %s to close",
             input_pad_last() ? "Right stick and RT" : "Mouse", guide_list_key());
    hud_text(x0, y0 + ph - u * 0.3f, h, buf, 0.7f, 0.6f, 0.4f);
    /* the pointer */
    hud_box(px - 1, py - 8, 3, 17, h, 1.0f, 1.0f, 1.0f, 0.95f);
    hud_box(px - 8, py - 1, 17, 3, h, 1.0f, 1.0f, 1.0f, 0.95f);
}

/* ---- Lua (tests) ---- */

static double num_param(int n)
{
    unsigned int o = lua_getparam(n);
    return (o && lua_isnumber(o)) ? lua_getnumber(o) : 0.0;
}

/* coop_plist_info() -> open (0/1), rows listed, clicks handled, pointer x, y */
static void l_coop_plist_info(void)
{
    lua_pushnumber(is_on);
    lua_pushnumber(nrows);
    lua_pushnumber(clicks);
    lua_pushnumber(px);
    lua_pushnumber(py);
}

/* coop_plist_row(i) -> slot, muted, volume % for the i-th row drawn (1-based), or nothing */
static void l_coop_plist_row(void)
{
    int i = (int)num_param(1) - 1;

    if (i < 0 || i >= nrows)
        return;
    lua_pushnumber(last_row_slot[i]);
    lua_pushnumber(voice_muted(last_row_slot[i]));
    lua_pushnumber(voice_player_volume(last_row_slot[i]));
}

/* coop_plist_point(kind, slot): background test mode, put the pointer on that row's button (0 mute, 1 vol -,
 * 2 vol +, 3 watch); returns 1 if there is such a button on screen */
static void l_coop_plist_point(void)
{
    int i, kind = (int)num_param(1), slot = (int)num_param(2);

    if (!test_background)
        return;
    for (i = 0; i < nhits; i++)
        if (hit[i].kind == kind && hit[i].slot == slot) {
            px = hit[i].x + hit[i].w * 0.5f;
            py = hit[i].y + hit[i].h * 0.5f;
            lua_pushnumber(1);
            return;
        }
    lua_pushnumber(0);
}

void plist_lua_init(void)
{
    tolua_function(NULL, "coop_plist_info", l_coop_plist_info);
    tolua_function(NULL, "coop_plist_row", l_coop_plist_row);
    tolua_function(NULL, "coop_plist_point", l_coop_plist_point);
}
