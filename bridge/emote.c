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

/* the keys that end an emote (input.c codes; 256 and up are the mouse): moving, jumping, firing and
 * sniper mode as the Controls page has them bound (Lua: coop_emote_keys); the Modern keys until then */
#define MAX_CANCEL 24
static struct { int code, vk; } cancel_keys[MAX_CANCEL] = {
    { 17, 'W' }, { 30, 'A' }, { 31, 'S' }, { 32, 'D' }, { 200, VK_UP }, { 208, VK_DOWN },
    { 203, VK_LEFT }, { 205, VK_RIGHT }, { 57, VK_SPACE }, { 256, VK_LBUTTON }, { 257, VK_RBUTTON },
};
static int ncancel = 11;

static int active = -1;             /* animation id, or -1 */
static int loop = 1;
static float rate, phase;
static double last_ms;
static BYTE *on_gob;
static int hp_at_start;

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

void emote_lua_init(void)
{
    tolua_function(NULL, "coop_emote_keys", l_coop_emote_keys);
}
