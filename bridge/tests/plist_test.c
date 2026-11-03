/* The real player list (plist.c) against a fake window, mouse, voice, revive state and player table. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../bridge.h"

#ifndef PLIST_SOURCE
#define PLIST_SOURCE "../plist.c"
#endif
#include PLIST_SOURCE

#undef lua_getparam
#undef lua_getnumber
#undef lua_isnumber
#undef lua_pushnumber
#undef tolua_function
struct build_addrs ADDR;
int role_host, have_remote = 1, my_slot = 1, test_background, cur_level = 1;
BYTE *local_player = (BYTE *)1;
static int ingame = 1, guide, ctrl, wheel, key, mouse_held_now, buttons_held_now, click, life_me = LIFE_ALIVE;
static int connected[MAX_PLAYERS], lives[MAX_PLAYERS], muted[MAX_PLAYERS], volume[MAX_PLAYERS], watched = -1, watch_calls;
static int in_level[MAX_PLAYERS], talking[MAX_PLAYERS];
static long dx, dy;
static int checks;
static char drawn[8192];
static int boxes;

#define CHECK(expr) do { checks++; if (!(expr)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); exit(1); } } while (0)

void logmsg(const char *fmt, ...) { }
int hud_ingame(void) { return ingame; }
int gamekey_down(int which) { return which == GK_LIST && key; }
int guide_on(void) { return guide; }
int ctrlview_on(void) { return ctrl; }
int emote_wheel_open(void) { return wheel; }
void input_mouse_hold(int on) { mouse_held_now = on; }
void input_buttons_hold(int on) { buttons_held_now = on; }
void input_mouse_take(long *x, long *y) { *x = dx; *y = dy; dx = dy = 0; }
int input_down(int code, int vk) { return code == 256 && click; }
int player_connected(int s) { return s >= 0 && s < MAX_PLAYERS && s != my_slot && connected[s]; }
int player_in_level(int s) { return in_level[s]; }
int player_life(int s) { return lives[s]; }
int player_ping(int s) { return 20 + s; }
int player_type(int s) { return 100 + s % 3; }
const char *player_name(int s) { static char n[16][16]; snprintf(n[s], 16, "P%d", s + 1); return n[s]; }
int revive_life(void) { return life_me; }
int revive_watching(void) { return watched; }
int revive_watch(int slot) { watch_calls++; if (life_me != LIFE_DEAD || lives[slot] != LIFE_ALIVE) return 0; watched = slot; return 1; }
int voice_talking(int s) { return talking[s < 0 ? my_slot : s]; }
int voice_muted(int s) { return muted[s]; }
void voice_set_muted(int s, int on) { muted[s] = on; }
int voice_player_volume(int s) { return volume[s] ? volume[s] : 100; }
void voice_set_player_volume(int s, int p) { volume[s] = p < 10 ? 10 : p > 200 ? 200 : p; }
void hud_slot_colour(int s, float *c) { c[0] = c[1] = c[2] = 1; }
const char *hud_hero_name(int t) { return "Kurt"; }
float hud_dist_to_slot(int s) { return 12.0f; }
const char *guide_list_key(void) { return "Tab"; }
int hud_font_h(void) { return 17; }
int hud_text_w(const char *s) { return (int)strlen(s) * 10; }
void hud_box(float x, float y, float w, float bh, int h, float r, float g, float b, float a) { boxes++; }
void hud_text(float x, float y, int h, const char *s, float r, float g, float b)
{ if (strlen(drawn) + strlen(s) + 2 < sizeof(drawn)) { strcat(drawn, s); strcat(drawn, "|"); } }
static unsigned int lua_param(int n) { return 0; }
static double lua_num(unsigned int o) { return 0; }
static int lua_isnum(unsigned int o) { return 0; }
static void lua_push(double d) { }
static void lua_reg(const char *m, const char *n, lua_CFunction f) { }

static void frame(void) { plist_frame(); }
static void draw(void) { drawn[0] = 0; boxes = 0; plist_draw(1280, 720); }
static int has(const char *s) { return strstr(drawn, s) != NULL; }
static int find(int kind, int slot)
{
    int i;
    for (i = 0; i < nhits; i++) if (hit[i].kind == kind && hit[i].slot == slot) return i;
    return -1;
}
static void point(int kind, int slot)
{
    int i = find(kind, slot);
    CHECK(i >= 0);
    px = hit[i].x + hit[i].w * 0.5f;
    py = hit[i].y + hit[i].h * 0.5f;
}
static void press(void) { click = 1; frame(); click = 0; frame(); }
static void reset(void)
{
    memset(connected, 0, sizeof(connected)); memset(lives, 0, sizeof(lives)); memset(muted, 0, sizeof(muted));
    memset(volume, 0, sizeof(volume)); memset(in_level, 0, sizeof(in_level)); memset(talking, 0, sizeof(talking));
    ingame = 1; guide = ctrl = wheel = key = click = 0; life_me = LIFE_ALIVE; watched = -1; watch_calls = 0;
    my_slot = 1; have_remote = 1;
    connected[0] = connected[2] = connected[3] = 1;
    in_level[0] = in_level[2] = in_level[3] = 1;
    key = 0; frame(); draw();
}

static void open_and_close(void)
{
    reset();
    CHECK(!plist_on() && !mouse_held_now && !buttons_held_now);
    key = 1; frame();
    CHECK(plist_on() && mouse_held_now && buttons_held_now);
    draw();
    CHECK(nrows == 4);                                  /* us and the three others, one row each */
    CHECK(has("P1") && has("P2") && has("P3") && has("P4") && has("(host)") && has("Mute") && has("Kurt"));
    CHECK(find(H_MUTE, my_slot) < 0 && find(H_WATCH, 0) < 0);   /* our own row has no buttons; none can Watch while alive */
    key = 0; frame();
    CHECK(!plist_on() && !mouse_held_now && !buttons_held_now);
    /* not at all outside a level, in the guide, the controls view or an emote wheel, or alone */
    ingame = 0; key = 1; frame(); CHECK(!plist_on());
    ingame = 1; guide = 1; frame(); CHECK(!plist_on());
    guide = 0; wheel = 1; frame(); CHECK(!plist_on());
    wheel = 0; ctrl = 1; frame(); CHECK(!plist_on());
    ctrl = 0; have_remote = 0; frame(); CHECK(!plist_on());
    have_remote = 1; frame(); CHECK(plist_on());
    guide = 1; frame(); CHECK(!plist_on() && !mouse_held_now);   /* opening the guide closes it */
    guide = 0; key = 0; frame();
}

static void mute_and_volume(void)
{
    reset();
    key = 1; frame(); draw();
    point(H_MUTE, 2); press(); CHECK(muted[2] == 1 && !muted[0] && !muted[3]);
    draw(); CHECK(has("Unmute"));
    point(H_MUTE, 2); press(); CHECK(muted[2] == 0);
    point(H_VOLUP, 3); press(); CHECK(voice_player_volume(3) == 120 && voice_player_volume(0) == 100);
    press(); press(); press(); press(); press(); press(); press(); press(); press();
    CHECK(voice_player_volume(3) == 200);              /* clamped */
    point(H_VOLDOWN, 3); press(); CHECK(voice_player_volume(3) == 180);
    { int i; for (i = 0; i < 20; i++) press(); }
    CHECK(voice_player_volume(3) == 10);
    /* a button held down is one click */
    point(H_MUTE, 0); click = 1; frame(); frame(); CHECK(muted[0] == 1); click = 0; frame();
    /* a click on nothing does nothing; a click already held when the list opened is not one */
    { int c = clicks; px = 5; py = 5; press(); CHECK(clicks == c && muted[0] == 1 && muted[2] == 0); }
    key = 0; frame();
    click = 1; key = 1; frame(); { int c = clicks; frame(); CHECK(clicks == c); } click = 0; key = 0; frame();
}

static void pointer_and_watch(void)
{
    reset();
    key = 1; frame(); draw();
    { float x0 = px; dx = 40; dy = -30; frame(); CHECK(px == x0 + 40 && py < 360); }
    dx = 99999; dy = 99999; frame(); CHECK(px <= 1279 && py <= 719);
    dx = -99999; dy = -99999; frame(); CHECK(px >= 0 && py >= 0);
    key = 0; frame();
    /* out of the fight: Watch appears for teammates still in it, and works */
    reset();
    life_me = LIFE_DEAD; lives[3] = LIFE_DEAD;
    key = 1; frame(); draw();
    CHECK(find(H_WATCH, 0) >= 0 && find(H_WATCH, 2) >= 0 && find(H_WATCH, 3) < 0 && find(H_WATCH, my_slot) < 0);
    point(H_WATCH, 2); press(); CHECK(watched == 2 && watch_calls == 1);
    draw(); CHECK(has("watching P3"));
    lives[3] = LIFE_ALIVE; draw();
    CHECK(find(H_WATCH, 3) >= 0);
    key = 0; frame();
    /* states */
    reset();
    in_level[2] = 0; lives[3] = LIFE_DOWNED; lives[0] = LIFE_DEAD;
    key = 1; frame(); draw();
    CHECK(has("loading") && has("down") && has("out"));
    lives[0] = 3; draw(); CHECK(has("watching"));
    talking[2] = 1; boxes = 0; draw();
    key = 0; frame();
}

int main(void)
{
    ADDR.lua_getparam = (DWORD)lua_param; ADDR.lua_getnumber = (DWORD)lua_num; ADDR.lua_isnumber = (DWORD)lua_isnum;
    ADDR.lua_pushnumber = (DWORD)lua_push; ADDR.tolua_function = (DWORD)lua_reg;
    open_and_close(); mute_and_volume(); pointer_and_watch();
    printf("PASS: %d player list checks\n", checks);
    return 0;
}
