/*
 * Gamepads for couch co-op ([couch] in coop.ini). Every local player is their own game window
 * (one process each, joined over localhost like any other co-op player); this gives each one a
 * gamepad. XInput is polled once a frame and turned into the keyboard and mouse the game already
 * understands (input.c feeds them to the game's DirectInput devices), so menus, revive (R) and the
 * co-op key bindings need nothing special: the keys are the Modern preset's, which a couch game
 * always uses (coop_dev.lua). It works without focus, which is how four windows can be played at once.
 *
 *   left stick / D-pad   the arrow keys (menus) and the move / strafe keys
 *   right stick          mouse look
 *   A                    jump, and Enter (menu select)
 *   B, Start             Esc (pause, back)
 *   X                    revive              Y   open the inventory / equip
 *   View (Back)          the co-op layer (pad.h): tap = ping, hold = player list, and with it the
 *                        D-pad, LB, RB and Y are co-op buttons (wheels, chat, watch, guide)
 *   right stick click    push to talk (input.c, [voice] pad_talk)
 *   right trigger        fire                left trigger   sniper mode
 *   right / left bumper  mouse wheel up / down (next / previous item)
 * The action keys follow the Controls page (pad_bind), so Original, Modern and Custom all work.
 * The same list is drawn on the Controls page's Gamepad tab (coop_dev.lua).
 */
#include <math.h>
#include <string.h>
#include "bridge.h"
#include "pad.h"

#define CODE_MOUSE_LEFT  256
#define CODE_MOUSE_RIGHT 257

/* the defaults are the Modern preset's keys; 256 + n is mouse button n */
static int bound[PA_COUNT] = { 17, 31, 30, 32, 57, CODE_MOUSE_LEFT, CODE_MOUSE_RIGHT, 18, 19, 47 };
static const int bound_default[PA_COUNT] = { 17, 31, 30, 32, 57, CODE_MOUSE_LEFT, CODE_MOUSE_RIGHT, 18, 19, 47 };

void pad_bind(int action, int code)
{
    if (action >= 0 && action < PA_COUNT)
        bound[action] = code >= 0 && code < 264 ? code : bound_default[action];
}

static void set(struct pad_out *o, int code)
{
    o->keys[code / 8] |= 1 << (code % 8);
}

/* Stick deflection after the dead zone, 0..1, keeping the sign */
static float stick(int v)
{
    float f = v / 32767.0f, a = fabsf(f);
    if (a < PAD_DEADZONE)
        return 0;
    a = (a - PAD_DEADZONE) / (1 - PAD_DEADZONE);
    if (a > 1)
        a = 1;
    return f < 0 ? -a : a;
}

/* One frame of pad state -> held codes and this frame's mouse movement (dt seconds, look =
 * counts per second at full deflection). prev_buttons: last frame's, for wheel clicks. */
void pad_map(const struct pad_raw *p, unsigned prev_buttons, float dt, float look, struct pad_out *o)
{
    float lx = stick(p->lx), ly = stick(p->ly), rx = stick(p->rx), ry = stick(p->ry);
    unsigned b = p->buttons, edge = b & ~prev_buttons;

    memset(o, 0, sizeof(*o));
    /* the arrow keys always (menus), the action keys on top of them (in game) */
    if (ly > 0.5f || (b & PAD_UP))      { set(o, 200); set(o, bound[PA_FWD]); }
    if (ly < -0.5f || (b & PAD_DOWN))   { set(o, 208); set(o, bound[PA_BACK]); }
    if (lx < -0.5f || (b & PAD_LEFT))   { set(o, 203); set(o, bound[PA_LEFT]); }
    if (lx > 0.5f || (b & PAD_RIGHT))   { set(o, 205); set(o, bound[PA_RIGHT]); }
    if (b & PAD_A)                      { set(o, bound[PA_JUMP]); set(o, 28); }
    if (b & (PAD_B | PAD_START))        set(o, 1);
    if (b & PAD_X)                      set(o, bound[PA_REVIVE]);
    if (b & PAD_Y)                      set(o, bound[PA_USE]);
    if (p->rt > PAD_TRIGGER)            set(o, bound[PA_FIRE]);
    if (p->lt > PAD_TRIGGER)            set(o, bound[PA_SNIPER]);
    /* squared response: fine aim near the centre, full speed at the edge */
    o->mouse[0] = (int)lroundf(rx * fabsf(rx) * look * dt);
    o->mouse[1] = (int)lroundf(-ry * fabsf(ry) * look * dt);
    if (edge & PAD_RB)                  o->mouse[2] += 120;
    if (edge & PAD_LB)                  o->mouse[2] -= 120;
}

/* ---- the co-op layer on View (pad.h) ---- */

const struct pad_line pad_layer_lines[] = {
    { "Tap View",                  "ping",   "Ping what you aim at: a marker for everyone" },
    { "Hold View",                 "list",   "Player list (right stick + RT: mute, volume, watch)" },
    { "View + D-pad up",           "emote",  "Emote wheel: right stick picks, let go of View plays" },
    { "View + D-pad down",         "chat",   "Chat: Steam's keyboard, or one on screen (Menu sends)" },
    { "View + D-pad left / right", "watch",  "Out of the fight: watch the previous / next teammate" },
    { "View + LB",                 "char",   "Character wheel: Kurt, Max or Doc" },
    { "View + RB",                 "quick",  "Quick chat: Ready, Wait for me, Over here, Thanks..." },
    { "View + Y",                  "guide",  "This quick guide (while held)" },
    { "Hold right stick click",    "voice",  "Push to talk (voice is open mic on a pad otherwise)" },
};
const int pad_layer_count = sizeof(pad_layer_lines) / sizeof(pad_layer_lines[0]);

void padlayer_step(struct padlayer *s, unsigned b, double now, struct padlayer_out *o)
{
    unsigned edge = b & ~s->prev;

    memset(o, 0, sizeof(*o));
    if (b & PAD_BACK) {
        if (!s->down) {
            s->down = 1;
            s->since = now;
            s->used = 0;
            s->latch = PL_NONE;
        }
#ifndef COOP_MUTANT_PAD_NO_SWALLOW      /* bridge/Makefile test-pad: the old behaviour, the D-pad still walks */
        o->swallow = PL_SWALLOW;
#else
        o->swallow = PAD_BACK;
#endif
        if (edge & PAD_UP)    { s->latch = PL_EMOTE; s->used = 1; }
        if (edge & PAD_LB)    { s->latch = PL_CHAR; s->used = 1; }
        if (edge & PAD_RB)    { s->latch = PL_QUICK; s->used = 1; }
        if (edge & PAD_DOWN)  { o->chat = 1; s->used = 1; }
        if (b & PAD_LEFT)     { o->gk |= 1u << GK_WATCHP; s->used = 1; }
        if (b & PAD_RIGHT)    { o->gk |= 1u << GK_WATCHN; s->used = 1; }
        if (b & PAD_Y)        { o->guide = 1; s->used = 1; }
        if (s->latch == PL_EMOTE)
            o->gk |= 1u << GK_EMOTE;
        else if (s->latch == PL_CHAR)
            o->gk |= 1u << GK_CHAR;
        else if (s->latch == PL_QUICK)
            o->gk |= 1u << GK_QUICK;
        else if (!o->chat && (s->used || now - s->since >= PL_HOLD_MS))
            o->gk |= 1u << GK_LIST;
    } else if (s->down) {
        /* let go: a short press with nothing else is the ping; a latched wheel closes (and chooses) */
        if (!s->used && now - s->since < PL_HOLD_MS)
            o->gk |= 1u << GK_PING;
        s->down = 0;
        s->latch = PL_NONE;
    }
    s->prev = b;
}

void pad_frame(struct padlayer *s, const struct pad_raw *p, unsigned prev_buttons, float dt, float look, double now,
               struct pad_out *o, struct padlayer_out *lo)
{
    struct pad_raw game = *p;

    padlayer_step(s, p->buttons, now, lo);
    game.buttons &= ~lo->swallow;
    pad_map(&game, prev_buttons & ~lo->swallow, dt, look, o);
}

int pad_active(const struct pad_raw *p, unsigned prev_buttons)
{
    return (p->buttons & ~prev_buttons) || stick(p->lx) || stick(p->ly) || stick(p->rx) || stick(p->ry) ||
           p->lt > PAD_TRIGGER || p->rt > PAD_TRIGGER;
}

int pad_glyphs(int was, const struct pad_raw *p, unsigned prev_buttons, int key_pressed)
{
    if (key_pressed)
        return 0;
    if (p && pad_active(p, prev_buttons))
        return 1;
    return was;
}

const char *pad_glyph_text(int pad, const char *keys, const char *buttons)
{
    return pad && buttons && buttons[0] ? buttons : keys;
}

#ifndef PAD_NO_XINPUT

typedef struct { WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; } xi_gamepad;
typedef struct { DWORD packet; xi_gamepad pad; } xi_state;
typedef DWORD (WINAPI *xi_get_state_t)(DWORD, xi_state *);

static xi_get_state_t xi_get_state;
static int xi_tried;
static struct pad_raw fake;
static int fake_on;

/* Lua (background test mode only): stand in for a pad that is not there */
void pad_fake(const struct pad_raw *raw)
{
    if (raw) {
        fake = *raw;
        fake_on = 1;
    } else {
        fake_on = 0;
    }
}

/* Read pad `index`; 0 when none is plugged in */
int pad_read(int index, struct pad_raw *out)
{
    xi_state st;

    if (fake_on) {
        *out = fake;
        return 1;
    }
    if (!xi_tried) {
        static const char *dlls[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };
        int i;
        xi_tried = 1;
        for (i = 0; i < 3 && !xi_get_state; i++) {
            HMODULE m = LoadLibraryA(dlls[i]);
            if (m && (xi_get_state = (xi_get_state_t)GetProcAddress(m, "XInputGetState")))
                logmsg("pad: using %s", dlls[i]);
        }
        if (!xi_get_state)
            logmsg("pad: no XInput, gamepads unavailable");
    }
    if (!xi_get_state || xi_get_state(index, &st) != ERROR_SUCCESS)
        return 0;
    /* XInput's button bits are PAD_* */
    out->buttons = st.pad.buttons;
    out->lt = st.pad.lt;
    out->rt = st.pad.rt;
    out->lx = st.pad.lx;
    out->ly = st.pad.ly;
    out->rx = st.pad.rx;
    out->ry = st.pad.ry;
    return 1;
}

#endif
