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

/* ---- the wheels: hold a key, move the mouse toward a slot, let go to choose it. Two of them, never
 * both at once: the emote wheel (letting go in the middle stops the emote that is playing) and the
 * character wheel (letting go in the middle changes nothing; a pick is handed to Lua, coop_char_pick).
 * The mouse is held back from the game while one is open. ---- */
#define MAX_SLOTS   8
#define WHEEL_RANGE 90.0f           /* mouse counts from the middle to the rim */
#define WHEEL_DEAD  0.35f           /* inside this fraction of the range: no slot */
enum { W_EMOTE, W_CHAR, W_COUNT };
static struct wheel {
    int key;                        /* GK_ */
    struct { int id; char label[24]; } slots[MAX_SLOTS];
    int nslots, on, sel;
    float vx, vy;
} wheels[W_COUNT] = {
    { .key = GK_EMOTE },
    { .key = GK_CHAR },
};
static int char_pick = -1;          /* the character wheel's choice, until Lua takes it */
#define EMOTE_ALL   1000            /* an emote wheel slot that opens the list of every animation (Lua's) */
static int all_pick;                /* that slot was chosen, until Lua takes it */

static struct wheel *wheel_open_one(void)
{
    int i;

    for (i = 0; i < W_COUNT; i++)
        if (wheels[i].on)
            return &wheels[i];
    return NULL;
}

int emote_wheel_open(void)
{
    return wheel_open_one() != NULL;
}

/* for the HUD: the open wheel's slots, the selected one (or -1) and the pointer, -1..1; mid is what
 * letting go in the middle does */
int emote_wheel_info(int *n, int *sel, float *vx, float *vy, const char **labels, const char **mid)
{
    struct wheel *w = wheel_open_one();
    int i;

    if (!w)
        return 0;
    *n = w->nslots;
    *sel = w->sel;
    *vx = w->vx / WHEEL_RANGE;
    *vy = w->vy / WHEEL_RANGE;
    for (i = 0; i < w->nslots; i++)
        labels[i] = w->slots[i].label;
    *mid = w == &wheels[W_EMOTE] ? "Let go to stop" : "Let go to stay";
    return 1;
}

static void wheel_close(struct wheel *w, int play)
{
    int sel = w->sel;

    if (!w->on)
        return;
    w->on = 0;
    input_mouse_hold(0);
    w->sel = -1;
    if (!play)
        return;
    if (w == &wheels[W_CHAR]) {
        if (sel >= 0)
            char_pick = w->slots[sel].id;
    } else if (sel >= 0 && w->slots[sel].id == EMOTE_ALL) {
        all_pick = 1;
    } else if (sel >= 0) {
        emote_play(w->slots[sel].id, 1);
    } else if (active >= 0) {
        stop("wheel");
    }
}

/* Every frame: open, steer and close one wheel */
static void wheel_frame(struct wheel *w)
{
    int down = w->nslots > 0 && gamekey_down(w->key) && local_player && gob_alive(local_player) &&
               revive_life() == LIFE_ALIVE && (w->on || !wheel_open_one());
    long dx, dy;
    float len;

    if (!down) {
        wheel_close(w, 0);
        return;
    }
    if (!w->on) {
        w->on = 1;
        w->vx = w->vy = 0.0f;
        w->sel = -1;
        input_mouse_hold(1);
    }
    input_mouse_take(&dx, &dy);
    w->vx += (float)dx;
    w->vy += (float)dy;
    len = sqrtf(w->vx * w->vx + w->vy * w->vy);
    if (len > WHEEL_RANGE) {
        w->vx *= WHEEL_RANGE / len;
        w->vy *= WHEEL_RANGE / len;
        len = WHEEL_RANGE;
    }
    if (len < WHEEL_RANGE * WHEEL_DEAD) {
        w->sel = -1;
    } else {
        float a = atan2f(w->vx, -w->vy);        /* 0 straight up, clockwise */
        float sector = 6.2831853f / (float)w->nslots;

        if (a < 0.0f)
            a += 6.2831853f;
        w->sel = (int)((a + sector * 0.5f) / sector) % w->nslots;
    }
}

/* the key was let go: choose (checked next frame, before it is drawn again) */
static void wheel_release_check(struct wheel *w)
{
    if (w->on && !(w->nslots > 0 && gamekey_down(w->key)))
        wheel_close(w, 1);
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

    for (i = 0; i < W_COUNT; i++) {
        wheel_release_check(&wheels[i]);
        wheel_frame(&wheels[i]);
    }
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

/* the slots of wheel w from Lua's (id, label, ...) arguments, clockwise from the top (none clears) */
static void set_slots(struct wheel *w)
{
    int i;

    w->nslots = 0;
    for (i = 1; w->nslots < MAX_SLOTS; i += 2) {
        unsigned int o = lua_getparam(i), t = lua_getparam(i + 1);
        long len;

        if (!o || !lua_isnumber(o))
            break;
        w->slots[w->nslots].id = (int)lua_getnumber(o);
        snprintf(w->slots[w->nslots].label, sizeof(w->slots[0].label), "%s",
                 t ? luaL_check_lstr(i + 1, &len) : "");
        w->nslots++;
    }
    wheel_close(w, 0);
}

/* coop_emote_wheel(id1, label1, id2, label2, ...) */
static void l_coop_emote_wheel(void)
{
    set_slots(&wheels[W_EMOTE]);
}

/* coop_char_wheel(type1, label1, ...): the character wheel's slots (OBJ types; 0 is "the level's own") */
static void l_coop_char_wheel(void)
{
    set_slots(&wheels[W_CHAR]);
}

/* coop_char_pick() -> the type picked on the character wheel since the last call, or nil */
static void l_coop_char_pick(void)
{
    if (char_pick < 0) {
        lua_pushnil();
        return;
    }
    lua_pushnumber((double)char_pick);
    char_pick = -1;
}

/* coop_emote_all() -> 1 once after the wheel's "All" slot was chosen, else nil */
static void l_coop_emote_all(void)
{
    if (all_pick)
        lua_pushnumber(1);
    all_pick = 0;
}

/* coop_anim_has(id) -> 1 if the local player's character has animation id, else nil */
static void l_coop_anim_has(void)
{
    unsigned int o = lua_getparam(1);
    int id = o && lua_isnumber(o) ? (int)lua_getnumber(o) : -1;

    if (id >= 0 && id < ANIM_ID_MAX && local_player && gob_alive(local_player) && om_anim_state(local_player, id))
        lua_pushnumber(1);
}

void emote_lua_init(void)
{
    tolua_function(NULL, "coop_emote_all", l_coop_emote_all);
    tolua_function(NULL, "coop_anim_has", l_coop_anim_has);
    tolua_function(NULL, "coop_emote_wheel", l_coop_emote_wheel);
    tolua_function(NULL, "coop_char_wheel", l_coop_char_wheel);
    tolua_function(NULL, "coop_char_pick", l_coop_char_pick);
    tolua_function(NULL, "coop_emote_keys", l_coop_emote_keys);
}
