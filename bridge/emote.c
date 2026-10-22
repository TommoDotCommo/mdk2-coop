/*
 * Emotes: hold one of the player's animations while standing still.
 *
 * The same trick as the downed pose (revive.c): our player is marked mirrored, so its own think
 * stops putting the idle animation back, and the chosen animation's state is applied every frame.
 * The normal animation sync carries it to the other games. The emote ends when we move, jump,
 * fire, take damage or go down (revive.c takes over then).
 *
 * The animation's own state block supplies its play rate; the play position runs here and wraps
 * for a looping emote (the block is played once, ANIMSTYLE_PLAYONCE, and rewound).
 */
#include <string.h>
#include <math.h>
#include "bridge.h"

#define ANIMSTYLE_PLAYONCE  3
#define DEFAULT_RATE        1.0f

static void stop(const char *why);
static int active = -1;             /* animation id, or -1 */

/* the keys that end an emote (input.c codes; 256 and up are the mouse): moving, jumping, firing and
 * sniper mode as the Controls page has them bound (Lua: coop_emote_keys); the Modern keys until then */
#define MAX_CANCEL 24
static struct { int code, vk; } cancel_keys[MAX_CANCEL] = {
    { 17, 'W' }, { 30, 'A' }, { 31, 'S' }, { 32, 'D' }, { 200, VK_UP }, { 208, VK_DOWN },
    { 203, VK_LEFT }, { 205, VK_RIGHT }, { 57, VK_SPACE }, { 256, VK_LBUTTON }, { 257, VK_RBUTTON },
};
static int ncancel = 11;

static int loop = 1;
static float rate, phase;
static double last_ms;
static BYTE *on_gob;
static int hp_at_start;

/* ---- the wheel: hold the emote key, move the mouse toward a slot, let go to play it (letting go in
 * the middle stops the emote that is playing). The mouse is held back from the game while it is open. ---- */
#define MAX_SLOTS   8
#define WHEEL_RANGE 90.0f           /* mouse counts from the middle to the rim */
#define WHEEL_DEAD  0.35f           /* inside this fraction of the range: no slot */
static struct { int id; char label[24]; } slots[MAX_SLOTS];
static int nslots;
static int wheel_on, wheel_sel = -1;
static float wvx, wvy;

int emote_wheel_open(void)
{
    return wheel_on;
}

/* for the HUD: the slots, the selected one (or -1) and the pointer, -1..1 */
int emote_wheel_info(int *n, int *sel, float *vx, float *vy, const char **labels)
{
    int i;

    if (!wheel_on)
        return 0;
    *n = nslots;
    *sel = wheel_sel;
    *vx = wvx / WHEEL_RANGE;
    *vy = wvy / WHEEL_RANGE;
    for (i = 0; i < nslots; i++)
        labels[i] = slots[i].label;
    return 1;
}

static void wheel_close(int play)
{
    int sel = wheel_sel;

    if (!wheel_on)
        return;
    wheel_on = 0;
    input_mouse_hold(0);
    wheel_sel = -1;
    if (!play)
        return;
    if (sel >= 0)
        emote_play(slots[sel].id, 1);
    else if (active >= 0)
        stop("wheel");
}

/* Every frame: open, steer and close the wheel */
static void wheel_frame(void)
{
    int down = nslots > 0 && gamekey_down(GK_EMOTE) && local_player && gob_alive(local_player) &&
               revive_life() == LIFE_ALIVE;
    long dx, dy;
    float len;

    if (!down) {
        wheel_close(0);
        return;
    }
    if (!wheel_on) {
        wheel_on = 1;
        wvx = wvy = 0.0f;
        wheel_sel = -1;
        input_mouse_hold(1);
    }
    input_mouse_take(&dx, &dy);
    wvx += (float)dx;
    wvy += (float)dy;
    len = sqrtf(wvx * wvx + wvy * wvy);
    if (len > WHEEL_RANGE) {
        wvx *= WHEEL_RANGE / len;
        wvy *= WHEEL_RANGE / len;
        len = WHEEL_RANGE;
    }
    if (len < WHEEL_RANGE * WHEEL_DEAD) {
        wheel_sel = -1;
    } else {
        float a = atan2f(wvx, -wvy);            /* 0 straight up, clockwise */
        float sector = 6.2831853f / (float)nslots;
        int k;

        if (a < 0.0f)
            a += 6.2831853f;
        k = (int)((a + sector * 0.5f) / sector) % nslots;
        wheel_sel = k;
    }
}

/* the key was let go: play the chosen emote (checked next frame, before it is drawn again) */
static void wheel_release_check(void)
{
    if (wheel_on && !(nslots > 0 && gamekey_down(GK_EMOTE)))
        wheel_close(1);
}

static void stop(const char *why)
{
    if (active < 0)
        return;
    logmsg("emote: anim %d stopped (%s)", active, why);
    if (gob_alive(on_gob))
        gob_set_mirrored(on_gob, 0);
    active = -1;
    on_gob = NULL;
}

int emote_active(void)
{
    return active;
}

/* id < 0 stops; otherwise start (or switch to) animation id. Only while alive and in a level.
 * Returns 1 if it plays. */
int emote_play(int id, int looping)
{
    DWORD *tmpl;
    float r;

    if (id < 0 || id >= ANIM_ID_MAX) {
        stop("asked");
        return 0;
    }
    if (!local_player || !gob_alive(local_player) || revive_life() != LIFE_ALIVE)
        return 0;
    tmpl = om_anim_state(local_player, id);
    if (!tmpl) {
        logmsg("emote: anim %d does not exist for this character", id);
        return 0;
    }
    memcpy(&r, &tmpl[5], 4);
    rate = (r > 0.01f && r < 20.0f) ? r : DEFAULT_RATE;
    if (active < 0) {
        gob_set_mirrored(local_player, 1);
        class_filter_think(gob_class(local_player));
    }
    active = id;
    loop = looping;
    phase = 0.0f;
    last_ms = now_ms();
    on_gob = local_player;
    hp_at_start = gob_hp(local_player);
    logmsg("emote: anim %d (rate %.3f, %s)", id, rate, looping ? "looping" : "once");
    return 1;
}

/* Every frame, before our state is captured */
void emote_frame(void)
{
    struct snap s;
    DWORD *st = s.anims[0].state;
    double now = now_ms();
    float one = 1.0f, four = 4.0f;
    unsigned i;

    wheel_release_check();
    wheel_frame();
    if (active < 0)
        return;
    if (!local_player || local_player != on_gob || !gob_alive(local_player)) {
        active = -1;
        on_gob = NULL;
        return;
    }
    if (revive_life() != LIFE_ALIVE) {
        stop("down");
        return;
    }
    if (gob_hp(local_player) < hp_at_start) {
        stop("hurt");
        return;
    }
    for (i = 0; i < (unsigned)ncancel && !chat_blocks_keys(); i++) {
        if (input_down(cancel_keys[i].code, cancel_keys[i].vk)) {
            stop("moved");
            return;
        }
    }
    phase += rate * (float)((now - last_ms) / 1000.0);
    last_ms = now;
    if (phase >= 1.0f)
        phase = loop ? phase - floorf(phase) : 0.98f;

    memset(&s, 0, sizeof(s));
    s.cur = (unsigned char)active;
    s.prev = NO_ANIM;
    s.nanims = 1;
    s.anims[0].id = (unsigned char)active;
    st[0] = 1;
    st[1] = 1;
    st[2] = ANIMSTYLE_PLAYONCE;
    memcpy(&st[ANIM_TIME0], &phase, 4);
    memcpy(&st[ANIM_TIME1], &phase, 4);
    memcpy(&st[5], &rate, 4);
    memcpy(&st[ANIM_WEIGHT], &one, 4);
    memcpy(&st[7], &four, 4);
    st[9] = 1;
    anim_apply(local_player, &s);
}

/* coop_emote_keys(k1, k2, ...): the keys that end an emote, numbered as Lua's (input_lua_key) */
static void l_coop_emote_keys(void)
{
    int n = 0, i, code, vk;
    unsigned int o;

    for (i = 1; n < MAX_CANCEL && (o = lua_getparam(i)) && lua_isnumber(o); i++)
        if (input_lua_key((int)lua_getnumber(o), &code, &vk)) {
            cancel_keys[n].code = code;
            cancel_keys[n].vk = vk;
            n++;
        }
    ncancel = n;
}

/* coop_emote_wheel(id1, label1, id2, label2, ...): the wheel's slots, clockwise from the top (none clears) */
static void l_coop_emote_wheel(void)
{
    int i;

    nslots = 0;
    for (i = 1; nslots < MAX_SLOTS; i += 2) {
        unsigned int o = lua_getparam(i), t = lua_getparam(i + 1);
        long len;

        if (!o || !lua_isnumber(o))
            break;
        slots[nslots].id = (int)lua_getnumber(o);
        snprintf(slots[nslots].label, sizeof(slots[nslots].label), "%s", t ? luaL_check_lstr(i + 1, &len) : "");
        nslots++;
    }
    wheel_close(0);
}

void emote_lua_init(void)
{
    tolua_function(NULL, "coop_emote_wheel", l_coop_emote_wheel);
    tolua_function(NULL, "coop_emote_keys", l_coop_emote_keys);
}
