/*
 * Text chat and pings.
 *
 * Chat: T opens a line at the bottom left, Enter sends, Esc cancels. The typed characters come
 * from a thread message hook (WH_GETMESSAGE) that swallows the keyboard messages while the line
 * is open, and the DirectInput keyboard reads as empty (chat_blocks_keys, input.c), so the game
 * sees none of it. Messages are EV_CHAT events: a client's goes to the host, which stamps the
 * sender's slot and passes it on. Text is up to CHAT_MAX characters, carried in the event's
 * name and from fields.
 *
 * Pings: V or the middle mouse button marks what the camera is aimed at: the actor nearest
 * the crosshair if there is one, else a point PING_REACH ahead (the bridge cannot cast a ray
 * into the level). EV_PING carries the point; everyone sees a marker with the sender's name and
 * the distance for PING_LIFE ms. One ping per player.
 *
 * Both are drawn by hud.c from the accessors here. Lua can drive them for tests: coop_say,
 * coop_ping, coop_chat_dump.
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "bridge.h"

#define KEY_CHAT        20          /* T (scancode) */
#define VK_CHAT         'T'
#define KEY_PING        47          /* V */
#define VK_PING         'V'
#define KEY_PING_MOUSE  258         /* middle mouse button */

#define CHAT_MAX        62
#define CHAT_KEEP       16
#define PING_REACH      40.0f       /* game units ahead when nothing is aimed at */
#define PING_CONE       0.08f       /* aim tolerance, sideways distance over distance (about 4.5 degrees) */
#define PING_RANGE      300.0f
#define PING_LIFE       6000
#define PING_COOLDOWN   400
#define TYPING_LIFE     20000       /* a lost "stopped typing" must not leave the dots up */

static struct {
    int slot;
    char text[CHAT_MAX + 1];
    double at;
} lines[CHAT_KEEP];
static int nlines;                  /* total received; lines[(nlines - 1) % CHAT_KEEP] is the newest */

static struct {
    float pos[3];
    double at;
    int live;
} pings[MAX_PLAYERS];

static double typing_at[MAX_PLAYERS];     /* when each player last said they are typing; 0 not */
static int open_line;
static char input[CHAT_MAX + 1];
static int input_len;
static double opened_at, suppress_until, last_ping_at;
static int was_chat_down, was_ping_down;
static HHOOK msg_hook_handle;

/* ---- messages ---- */

static void add_line(int slot, const char *text)
{
    int i = nlines++ % CHAT_KEEP;

    lines[i].slot = slot;
    snprintf(lines[i].text, sizeof(lines[i].text), "%s", text);
    lines[i].at = now_ms();
    logmsg("chat: %s: %s", slot >= 0 && slot < MAX_PLAYERS ? player_name(slot) : "?", text);
}

/* is that player typing a message (they said so within TYPING_LIFE) */
int chat_typing(int slot)
{
    return slot >= 0 && slot < MAX_PLAYERS && typing_at[slot] && now_ms() - typing_at[slot] < TYPING_LIFE;
}

/* that player's latest message */
int chat_last(int slot, const char **text, double *age_ms)
{
    int i, s;

    for (i = 0; chat_line(i, &s, text, age_ms); i++)
        if (s == slot)
            return 1;
    return 0;
}

static void send_typing(int on)
{
    struct event e;

    if (!have_remote || my_slot < 0)
        return;
    event_init(&e, EV_TYPING);
    e.damage = (short)my_slot;
    e.part = (signed char)on;
    event_send(&e, TO_ALL);
    typing_at[my_slot] = on ? now_ms() : 0;
}

/* newest first; 0 when there is no i-th message */
int chat_line(int i, int *slot, const char **text, double *age_ms)
{
    int k;

    if (i < 0 || i >= nlines || i >= CHAT_KEEP)
        return 0;
    k = (nlines - 1 - i) % CHAT_KEEP;
    *slot = lines[k].slot;
    *text = lines[k].text;
    *age_ms = now_ms() - lines[k].at;
    return 1;
}

static void send_chat(const char *text)
{
    struct event e;
    size_t n = strlen(text);

    if (!n || !have_remote || my_slot < 0)
        return;
    event_init(&e, EV_CHAT);
    e.damage = (short)my_slot;
    memcpy(e.name, text, n < EV_NAME - 1 ? n : EV_NAME - 1);
    if (n > EV_NAME - 1)
        memcpy(e.from, text + EV_NAME - 1, n - (EV_NAME - 1) < EV_NAME - 1 ? n - (EV_NAME - 1) : EV_NAME - 1);
    event_send(&e, TO_ALL);
    add_line(my_slot, text);
}

/* ---- pings ---- */

static void rotate(const float *q, const float *v, float *out)
{
    float w = q[0], x = q[1], y = q[2], z = q[3];
    float tx = 2.0f * (y * v[2] - z * v[1]), ty = 2.0f * (z * v[0] - x * v[2]), tz = 2.0f * (x * v[1] - y * v[0]);

    out[0] = v[0] + w * tx + (y * tz - z * ty);
    out[1] = v[1] + w * ty + (z * tx - x * tz);
    out[2] = v[2] + w * tz + (x * ty - y * tx);
}

/* What the camera looks at: the actor nearest the crosshair, else a point straight ahead */
static int aim(float *out)
{
    BYTE *cam = hud_camera();
    static const float fwd0[3] = { 0.0f, 1.0f, 0.0f };
    float fwd[3], *c, best = PING_CONE;
    int i, found = 0;

    if (!gob_alive(cam))
        return 0;
    c = (float *)(cam + GOB_POSITION);
    rotate((float *)(cam + GOB_ORIENTATION), fwd0, fwd);
    for (i = 0; i < GOB_SLOTS; i++) {
        BYTE *g = (BYTE *)GOB_TABLE[i * 2];
        float *p, d[3], t, perp[3], off;

        if (!g || !gob_is_actor(g) || gob_hp(g) <= 0)
            continue;
        p = (float *)(g + GOB_POSITION);
        d[0] = p[0] - c[0]; d[1] = p[1] - c[1]; d[2] = p[2] + 1.0f - c[2];
        t = d[0] * fwd[0] + d[1] * fwd[1] + d[2] * fwd[2];
        if (t < 2.0f || t > PING_RANGE)
            continue;
        perp[0] = d[0] - t * fwd[0]; perp[1] = d[1] - t * fwd[1]; perp[2] = d[2] - t * fwd[2];
        off = sqrtf(perp[0] * perp[0] + perp[1] * perp[1] + perp[2] * perp[2]) / t;
        if (off < best) {
            best = off;
            out[0] = p[0]; out[1] = p[1]; out[2] = p[2] + 1.0f;
            found = 1;
        }
    }
    if (!found)
        for (i = 0; i < 3; i++)
            out[i] = c[i] + fwd[i] * PING_REACH;
    return 1;
}

static void set_ping(int slot, const float *pos)
{
    memcpy(pings[slot].pos, pos, sizeof(pings[slot].pos));
    pings[slot].at = now_ms();
    pings[slot].live = 1;
    logmsg("ping: %s at (%.1f, %.1f, %.1f)", player_name(slot), pos[0], pos[1], pos[2]);
}

static void send_ping(const float *pos)
{
    struct event e;

    if (!have_remote || my_slot < 0 || now_ms() - last_ping_at < PING_COOLDOWN)
        return;
    last_ping_at = now_ms();
    event_init(&e, EV_PING);
    e.damage = (short)my_slot;
    memcpy(e.pos, pos, sizeof(e.pos));
    event_send(&e, TO_ALL);
    set_ping(my_slot, pos);
}

/* the slot's live ping: its point and age; 0 when it has none (or ran out) */
int ping_get(int slot, float *pos, double *age_ms)
{
    if (slot < 0 || slot >= MAX_PLAYERS || !pings[slot].live)
        return 0;
    *age_ms = now_ms() - pings[slot].at;
    if (*age_ms > PING_LIFE) {
        pings[slot].live = 0;
        return 0;
    }
    memcpy(pos, pings[slot].pos, sizeof(pings[slot].pos));
    return 1;
}

/* fraction of its life a ping has left, for fading */
float ping_life_left(double age_ms)
{
    return (float)(1.0 - age_ms / PING_LIFE);
}

/* An EV_CHAT / EV_PING from `from` (a link). On the host this stamps the sender's slot into e,
 * for the relay; elsewhere e->damage already holds it. */
void chat_event(struct event *e, int from)
{
    if (role_host)
        e->damage = (short)from;
    if (e->damage < 0 || e->damage >= MAX_PLAYERS)
        return;
    if (e->kind == EV_TYPING) {
        typing_at[e->damage] = e->part ? now_ms() : 0;
    } else if (e->kind == EV_CHAT) {
        char text[CHAT_MAX + 1];
        size_t n;

        e->name[EV_NAME - 1] = 0;
        e->from[EV_NAME - 1] = 0;
        n = strlen(e->name);
        memcpy(text, e->name, n);
        snprintf(text + n, sizeof(text) - n, "%s", n == EV_NAME - 1 ? e->from : "");
        add_line(e->damage, text);
        typing_at[e->damage] = 0;
    } else if (e->epoch == cur_epoch) {
        set_ping(e->damage, e->pos);
    }
}

/* ---- typing ---- */

static void close_line(void)
{
    if (open_line)
        send_typing(0);
    open_line = 0;
    suppress_until = now_ms() + 250;    /* the key that closed it must not reach the game either */
}

int chat_is_open(void)
{
    return open_line;
}

const char *chat_input(void)
{
    return input;
}

int chat_blocks_keys(void)
{
    return open_line || now_ms() < suppress_until;
}

static void key_down(MSG *m)
{
    BYTE ks[256];
    WORD ch;
    int vk = (int)m->wParam, repeat = (m->lParam >> 30) & 1;

    if (vk == VK_RETURN) {
        if (!repeat) {
            send_chat(input);
            close_line();
        }
    } else if (vk == VK_ESCAPE) {
        close_line();
    } else if (vk == VK_BACK) {
        if (input_len)
            input[--input_len] = 0;
    } else if (!(vk == VK_CHAT && now_ms() - opened_at < 150) && input_len < CHAT_MAX &&
               GetKeyboardState(ks) && ToAscii(vk, (m->lParam >> 16) & 0xff, ks, &ch, 0) == 1 &&
               ch >= 32 && ch < 127) {
        input[input_len++] = (char)ch;
        input[input_len] = 0;
    }
}

static LRESULT CALLBACK msg_hook(int code, WPARAM wp, LPARAM lp)
{
    MSG *m = (MSG *)lp;

    if (code >= 0 && wp == PM_REMOVE && open_line) {
        switch (m->message) {
        case WM_KEYDOWN:
            key_down(m);
            /* fall through */
        case WM_KEYUP: case WM_CHAR: case WM_DEADCHAR: case WM_SYSKEYDOWN: case WM_SYSKEYUP: case WM_SYSCHAR:
            m->message = WM_NULL;       /* the game's own message loop never sees it */
            break;
        }
    }
    return CallNextHookEx(msg_hook_handle, code, wp, lp);
}

/* Per frame, from hud_frame, on the game's thread. `allowed`: in a level with the HUD up and
 * others connected. */
void chat_frame(int allowed)
{
    int chat_down, ping_down;

    if (!msg_hook_handle)
        msg_hook_handle = SetWindowsHookExA(WH_GETMESSAGE, msg_hook, NULL, GetCurrentThreadId());
    if (open_line && !allowed)
        close_line();
    chat_down = allowed && !open_line && input_down(KEY_CHAT, VK_CHAT);
    ping_down = allowed && !open_line && (input_down(KEY_PING, VK_PING) || input_down(KEY_PING_MOUSE, VK_MBUTTON));
    if (chat_down && !was_chat_down) {
        open_line = 1;
        input[0] = 0;
        input_len = 0;
        opened_at = now_ms();
        send_typing(1);
    }
    if (open_line && now_ms() - typing_at[my_slot < 0 ? 0 : my_slot] > TYPING_LIFE / 2)
        send_typing(1);                 /* still here: keep the dots up */
    if (ping_down && !was_ping_down) {
        float p[3];
        if (aim(p))
            send_ping(p);
    }
    was_chat_down = chat_down;
    was_ping_down = ping_down;
}

/* ---- Lua ---- */

/* coop_say(text): send a chat message as this player */
static void l_coop_say(void)
{
    long len;
    const char *text = luaL_check_lstr(1, &len);
    char buf[CHAT_MAX + 1];

    snprintf(buf, sizeof(buf), "%s", text);
    send_chat(buf);
}

/* coop_ping([x, y, z]): ping that point, or what the camera is aimed at */
static void l_coop_ping(void)
{
    unsigned int o = lua_getparam(1);
    float p[3];

    if (o && lua_isnumber(o)) {
        p[0] = (float)lua_getnumber(o);
        p[1] = (float)lua_getnumber(lua_getparam(2));
        p[2] = (float)lua_getnumber(lua_getparam(3));
        last_ping_at = 0;
        send_ping(p);
    } else if (aim(p)) {
        last_ping_at = 0;
        send_ping(p);
    }
}

/* coop_typing(on): say we are (not) typing, as the T key does */
static void l_coop_typing(void)
{
    send_typing((int)lua_getnumber(lua_getparam(1)));
}

/* coop_chat_dump(): log the messages and live pings */
static void l_coop_chat_dump(void)
{
    int i, slot;
    const char *text;
    double age;
    float p[3];

    for (i = 0; chat_line(i, &slot, &text, &age); i++)
        logmsg("chat line %d: %d %s | %s", i, slot + 1, player_name(slot), text);
    for (i = 0; i < MAX_PLAYERS; i++)
        if (chat_typing(i))
            logmsg("typing: %d %s", i + 1, player_name(i));
    for (i = 0; i < MAX_PLAYERS; i++)
        if (ping_get(i, p, &age))
            logmsg("ping live: %d %s (%.1f, %.1f, %.1f)", i + 1, player_name(i), p[0], p[1], p[2]);
}

void chat_lua_init(void)
{
    tolua_function(NULL, "coop_say", l_coop_say);
    tolua_function(NULL, "coop_ping", l_coop_ping);
    tolua_function(NULL, "coop_typing", l_coop_typing);
    tolua_function(NULL, "coop_chat_dump", l_coop_chat_dump);
}
