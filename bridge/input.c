/*
 * Background test mode ([test] background=1 in coop.ini): the game runs without focus and
 * without the real keyboard and mouse, so automated runs leave the desktop alone.
 *
 * - DirectInput: keyboard and mouse report only the presses scripts ask for (coop_press),
 *   never the real devices, and never take the cursor (no exclusive mode, no Acquire).
 * - Window: shown without activation at [test] x, y (tools/install.sh puts them on the second
 *   monitor), behind other windows, and told it is always active, so it keeps running there.
 * - Frames capped at [test] fps (60).
 * - Screenshots (coop_shot): read from the back buffer just before the game shows it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bridge.h"
#include "route.h"
#include "pad.h"
#include <mmsystem.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

int test_background;
int test_journal;
int couch_active;               /* [couch] in coop.ini: one of several local players on this PC */
static int synth;               /* devices are ours (scripted presses or gamepad), not the real ones */
static struct { int slot, count, pad, x, y, w, h; float look; } couch;

/* ---- synthetic presses ---- */

#define MOUSE_BUTTON0   256     /* coop_press codes: 0-255 keyboard scancodes, 256+ mouse buttons */
#define CODES           264

static double release_at[CODES];       /* held while now_ms() < release_at */
static struct route_input captured, replay_input;
static int replay_active;

void input_capture(struct route_input *out)
{
    *out = captured;
    memset(captured.mouse, 0, sizeof(captured.mouse));
}

void input_replay(const struct route_input *input)
{
    replay_active = input != NULL;
    if (input) {
        memcpy(replay_input.keys, input->keys, ROUTE_BYTES);
        for (int i = 0; i < 3; i++) replay_input.mouse[i] += input->mouse[i];
    } else {
        memset(&replay_input, 0, sizeof(replay_input));
        memset(release_at, 0, sizeof(release_at));
    }
}

/* the newest key, mouse button or pad button pressed (a pad presses key codes), for coop_last_press */
static BYTE press_state[(CODES + 7) / 8];
static int last_press_code = -1;
static DWORD last_press_tick;

static void capture_code(int code, int down)
{
    unsigned char mask = 1 << (code % 8);
    if (down) captured.keys[code / 8] |= mask;
    else captured.keys[code / 8] &= ~mask;
    if (code < 0 || code >= CODES)
        return;
    if (down && !(press_state[code / 8] & mask)) {
        last_press_code = code;
        last_press_tick = GetTickCount();
    }
    if (down) press_state[code / 8] |= mask;
    else press_state[code / 8] &= ~mask;
}

void input_press(int code, int hold_ms)
{
    if (code < 0 || code >= CODES)
        return;
    release_at[code] = now_ms() + hold_ms;
    logmsg("press %d for %d ms", code, hold_ms);
}

static BYTE pad_keys[33];
static int pad_enabled = 1;       /* [pad] enabled in coop.ini: a pad plays a normal game too */
static float pad_look = 700;      /* [pad] look: counts per second at full stick */
static long padm[3];
static int pad_index = -1;
static unsigned pad_buttons;

static int held(int code)
{
    if ((pad_keys[code / 8] >> (code % 8)) & 1)
        return 1;
    if (replay_active)
        return (replay_input.keys[code / 8] >> (code % 8)) & 1;
    return now_ms() < release_at[code];
}

/* The keys of the co-op features that read the keyboard themselves (chat, ping, revive, push to
 * talk): set by Lua from the Controls page (coop_game_keys), two each. code is this file's (scan
 * code, or 256 + mouse button), vk the Windows key for the focused-window read. */
static struct { int code[2], vk[2]; } gamekeys[GK_COUNT] = {
    { { 20, -1 }, { 'T', 0 } },
    { { 47, 258 }, { 'V', VK_MBUTTON } },
    { { 19, -1 }, { 'R', 0 } },
    { { 48, -1 }, { 'B', 0 } },
    { { 35, -1 }, { 'H', 0 } },
    { { 36, -1 }, { 'J', 0 } },
};

/* the emote wheel (emote.c) holds the mouse back from the game while it is open and steers with it */
static int mouse_held;
static long held_dx, held_dy;

void input_mouse_hold(int on)
{
    mouse_held = on;
    if (on)
        held_dx = held_dy = 0;
}

void input_mouse_take(long *dx, long *dy)
{
    *dx = held_dx;
    *dy = held_dy;
    held_dx = held_dy = 0;
}

/* none of them while a chat line is being typed (the letters are text then) */
int gamekey_down(int which)
{
    int i;

    if (chat_blocks_keys())
        return 0;
    for (i = 0; i < 2; i++)
        if (gamekeys[which].code[i] >= 0 && input_down(gamekeys[which].code[i], gamekeys[which].vk[i]))
            return 1;
    return 0;
}

/* the Windows key of the first slot that is set (the chat line ignores that letter right after opening) */
int gamekey_vk(int which)
{
    return gamekeys[which].code[0] >= 0 ? gamekeys[which].vk[0] : gamekeys[which].vk[1];
}

/* revive.c: is this key or mouse button down? Background test mode: the scripted presses;
 * otherwise the real one (vk), only while our game window has the focus */
int input_down(int code, int vk)
{
    DWORD pid = 0;

    if (code < 0 || code >= CODES)
        return 0;
    if (synth)
        return held(code);
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId() && (GetAsyncKeyState(vk) & 0x8000);
}

/* Not a couch game: the first connected pad plays this window, while it has the focus. The pad
 * is merged into the real keyboard and mouse (pad_merge_*), and its look movement waits in padm. */
static int own_window_focused(void)
{
    DWORD pid = 0;

    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

/* Once a frame, before the scene updates: the pad becomes held codes and mouse movement */
void input_pad_frame(void)
{
    static double last;
    struct pad_raw raw;
    struct pad_out out;
    double now = now_ms();
    float dt = last ? (float)((now - last) / 1000.0) : 0;
    int i, ok;

    last = now;
    if (dt > 0.1f)
        dt = 0.1f;
    if (couch_active) {
        ok = pad_read(couch.pad, &raw);
    } else if (!synth && !pad_enabled) {
        return;
    } else {
        /* 4 pads are looked at every ~second until one is found, then only that one */
        static double next_scan;

        ok = 0;
        if (synth || own_window_focused()) {
            if (pad_index >= 0) {
                ok = pad_read(pad_index, &raw);
                if (!ok)
                    pad_index = -1;
            } else if (now >= next_scan) {
                next_scan = now + 1000;
                for (i = 0; i < 4 && !ok; i++)
                    if ((ok = pad_read(i, &raw)))
                        pad_index = i;
                if (ok)
                    logmsg("pad: %d plays this game", pad_index);
            }
        }
    }
    if (!ok) {
        memset(pad_keys, 0, sizeof(pad_keys));
        pad_buttons = 0;
        return;
    }
    pad_map(&raw, pad_buttons, dt, couch_active ? couch.look : pad_look, &out);
    pad_buttons = raw.buttons;
    memcpy(pad_keys, out.keys, sizeof(pad_keys));
    for (i = 0; i < 3; i++) {
        if (synth)
            replay_input.mouse[i] += out.mouse[i];
        else
            padm[i] += out.mouse[i];
    }
}

static int pad_down(int code)
{
    return (pad_keys[code / 8] >> (code % 8)) & 1;
}

int input_couch(int *slot, int *count)
{
    *slot = couch.slot;
    *count = couch.count;
    return couch_active;
}

/* ---- devices ---- */

enum { DEV_OTHER, DEV_KEYBOARD, DEV_MOUSE };

struct dev {
    void *ptr;
    int kind;
    BYTE reported[CODES];   /* last state given out through GetDeviceData */
    BYTE padrep[CODES];     /* the same for the pad's codes, when the real device is read */
};

static struct dev devs[8];

static struct dev *dev_find(void *ptr)
{
    int i;
    for (i = 0; i < 8; i++)
        if (devs[i].ptr == ptr)
            return &devs[i];
    return NULL;
}

/* real devices (not synth): the pad's held codes and look movement added to what the device says */
static void pad_merge_state(struct dev *d, void *out, DWORD size)
{
    int i;

    if (d->kind == DEV_KEYBOARD) {
        for (i = 0; i < 256 && i < (int)size; i++)
            if (pad_down(i))
                ((BYTE *)out)[i] = 0x80;
    } else {
        for (i = 0; i < 3 && (i + 1) * 4 <= (int)size; i++) {
            ((LONG *)out)[i] += padm[i];
            padm[i] = 0;
        }
        for (i = 0; i < 8 && 12 + i < (int)size; i++)
            if (pad_down(256 + i))
                ((BYTE *)out)[12 + i] = 0x80;
    }
}

/* real devices, buffered: events for what the pad changed, after the device's own (room: the size
 * of the caller's buffer, *n how many are in it) */
static void pad_merge_events(struct dev *d, DIDEVICEOBJECTDATA *out, DWORD objsize, DWORD room, DWORD *n)
{
    int code, first = d->kind == DEV_KEYBOARD ? 0 : MOUSE_BUTTON0, last = d->kind == DEV_KEYBOARD ? 256 : CODES;

    if (d->kind == DEV_MOUSE) {
        int axis;
        for (axis = 0; axis < 3; axis++) {
            DIDEVICEOBJECTDATA *e;
            if (!padm[axis])
                continue;
            if (mouse_held && axis < 2) {
                if (axis == 0) held_dx += padm[0]; else held_dy += padm[1];
                padm[axis] = 0;
                continue;
            }
            if (*n >= room)
                break;
            e = (DIDEVICEOBJECTDATA *)((BYTE *)out + *n * objsize);
            memset(e, 0, objsize);
            e->dwOfs = axis * 4;
            e->dwData = (DWORD)padm[axis];
            e->dwTimeStamp = GetTickCount();
            padm[axis] = 0;
            (*n)++;
        }
    }
    for (code = first; code < last && *n < room; code++) {
        BYTE s = pad_down(code) ? 0x80 : 0;
        DIDEVICEOBJECTDATA *e;
        if (s == d->padrep[code])
            continue;
        e = (DIDEVICEOBJECTDATA *)((BYTE *)out + *n * objsize);
        memset(e, 0, objsize);
        e->dwOfs = d->kind == DEV_KEYBOARD ? (DWORD)code : (DWORD)(12 + code - MOUSE_BUTTON0);
        e->dwData = s;
        e->dwTimeStamp = GetTickCount();
        d->padrep[code] = s;
        (*n)++;
    }
}

/* vtable slots, IDirectInput8A and IDirectInputDevice8A */
#define DI_CREATEDEVICE     3
#define DEV_ACQUIRE         7
#define DEV_UNACQUIRE       8
#define DEV_GETDEVICESTATE  9
#define DEV_GETDEVICEDATA   10
#define DEV_SETCOOPLEVEL    13
#define DEV_POLL            25

typedef HRESULT (WINAPI *create_device_t)(void *, REFGUID, void **, void *);
typedef HRESULT (WINAPI *dev_fn_t)(void *);
typedef HRESULT (WINAPI *get_state_t)(void *, DWORD, void *);
typedef HRESULT (WINAPI *get_data_t)(void *, DWORD, DIDEVICEOBJECTDATA *, DWORD *, DWORD);
typedef HRESULT (WINAPI *set_coop_t)(void *, HWND, DWORD);

static create_device_t create_device_orig;
static dev_fn_t acquire_orig, unacquire_orig, poll_orig;
static get_state_t get_state_orig;
static get_data_t get_data_orig;
static set_coop_t set_coop_orig;

static void *patch_slot(void *obj, int slot, void *fn)
{
    void **vt = *(void ***)obj, *old = vt[slot];
    DWORD prot;
    if (old == fn)
        return NULL;
    VirtualProtect(&vt[slot], 4, PAGE_READWRITE, &prot);
    vt[slot] = fn;
    VirtualProtect(&vt[slot], 4, prot, &prot);
    return old;
}

static HRESULT WINAPI dev_acquire(void *self)
{
    return synth && dev_find(self) ? DI_OK : acquire_orig(self);
}

static HRESULT WINAPI dev_unacquire(void *self)
{
    return synth && dev_find(self) ? DI_OK : unacquire_orig(self);
}

static HRESULT WINAPI dev_poll(void *self)
{
    return synth && dev_find(self) ? DI_OK : poll_orig(self);
}

static HRESULT WINAPI dev_set_coop(void *self, HWND wnd, DWORD flags)
{
    return synth && dev_find(self) ? DI_OK : set_coop_orig(self, wnd, flags);
}

/* Current synthetic state of one code, as the device reports it (0x80 = down) */
static BYTE code_state(struct dev *d, int code)
{
    if (d->kind == DEV_KEYBOARD)
        return code < 256 && held(code) ? 0x80 : 0;
    return code >= MOUSE_BUTTON0 && held(code) ? 0x80 : 0;
}

static HRESULT WINAPI dev_get_state(void *self, DWORD size, void *out)
{
    struct dev *d = dev_find(self);
    int i;
    HRESULT hr;

    if (!d)
        return get_state_orig(self, size, out);
    if (!synth) {
        hr = get_state_orig(self, size, out);
        if (FAILED(hr)) return hr;
        pad_merge_state(d, out, size);
    } else {
        memset(out, 0, size);
        if (d->kind == DEV_KEYBOARD) {
            for (i = 0; i < 256 && i < (int)size; i++)
                ((BYTE *)out)[i] = code_state(d, i);
        } else {
            /* DIMOUSESTATE(2): lX, lY, lZ, then 4 (8) buttons */
            for (i = 0; i < 3 && (i + 1) * 4 <= (int)size; i++) {
                ((LONG *)out)[i] = replay_input.mouse[i];
                replay_input.mouse[i] = 0;
            }
            for (i = 0; i < 8 && 12 + i < (int)size; i++)
                ((BYTE *)out)[12 + i] = code_state(d, MOUSE_BUTTON0 + i);
        }
    }
    if (d->kind == DEV_KEYBOARD && chat_blocks_keys())
        memset(out, 0, size);               /* typing a chat line: the game sees no keys */
    if (d->kind == DEV_MOUSE && mouse_held && size >= 8) {
        held_dx += ((LONG *)out)[0];        /* emote wheel open: the movement steers it, not the camera */
        held_dy += ((LONG *)out)[1];
        ((LONG *)out)[0] = ((LONG *)out)[1] = 0;
    }
    if (d->kind == DEV_KEYBOARD) {
        for (i = 0; i < 256 && i < (int)size; i++) capture_code(i, ((BYTE *)out)[i] & 0x80);
    } else {
        for (i = 0; i < 3 && (i + 1) * 4 <= (int)size; i++) captured.mouse[i] += ((LONG *)out)[i];
        for (i = 0; i < 8 && 12 + i < (int)size; i++) capture_code(256 + i, ((BYTE *)out)[12 + i] & 0x80);
    }
    return DI_OK;
}

/* Buffered reads: one event per code whose state changed since the last read */
static HRESULT WINAPI dev_get_data(void *self, DWORD objsize, DIDEVICEOBJECTDATA *out, DWORD *inout, DWORD flags)
{
    struct dev *d = dev_find(self);
    int code, first, last;
    DWORD n = 0, room;

    if (!d)
        return get_data_orig(self, objsize, out, inout, flags);
    if (!synth) {
        DWORD room0 = *inout;
        HRESULT hr = get_data_orig(self, objsize, out, inout, flags);
        if (SUCCEEDED(hr) && out && !(flags & DIGDD_PEEK) && *inout <= room0)
            pad_merge_events(d, out, objsize, room0, inout);
        if (SUCCEEDED(hr) && d->kind == DEV_KEYBOARD && chat_blocks_keys()) {
            *inout = 0;                     /* typing a chat line: read and dropped */
            return hr;
        }
        if (SUCCEEDED(hr) && out && !(flags & DIGDD_PEEK)) {
            for (DWORD i = 0; i < *inout; i++) {
                DIDEVICEOBJECTDATA *e = (void *)((BYTE *)out + i * objsize);
                if (d->kind == DEV_KEYBOARD && e->dwOfs < 256) capture_code(e->dwOfs, e->dwData & 0x80);
                else if (d->kind == DEV_MOUSE && e->dwOfs < 12 && e->dwOfs % 4 == 0) {
                    captured.mouse[e->dwOfs / 4] += (LONG)e->dwData;
                    if (mouse_held && e->dwOfs < 8) {
                        if (e->dwOfs == 0) held_dx += (LONG)e->dwData;
                        else held_dy += (LONG)e->dwData;
                        e->dwData = 0;
                    }
                }
                else if (d->kind == DEV_MOUSE && e->dwOfs >= 12 && e->dwOfs < 20)
                    capture_code(256 + e->dwOfs - 12, e->dwData & 0x80);
            }
        }
        return hr;
    }
    room = *inout;
    if (d->kind == DEV_MOUSE) {
        for (int axis = 0; axis < 3; axis++) {
            if (!replay_input.mouse[axis] || (out && n >= room)) continue;
            if (out) {
                DIDEVICEOBJECTDATA *e = (void *)((BYTE *)out + n * objsize);
                memset(e, 0, objsize);
                e->dwOfs = axis * 4;
                e->dwData = replay_input.mouse[axis];
                if (mouse_held && axis < 2) {
                    if (!(flags & DIGDD_PEEK)) {
                        if (axis == 0) held_dx += replay_input.mouse[0]; else held_dy += replay_input.mouse[1];
                    }
                    e->dwData = 0;
                }
                e->dwTimeStamp = GetTickCount();
            }
            if (!(flags & DIGDD_PEEK)) {
                captured.mouse[axis] += replay_input.mouse[axis];
                replay_input.mouse[axis] = 0;
            }
            n++;
        }
    }
    first = d->kind == DEV_KEYBOARD ? 0 : MOUSE_BUTTON0;
    last = d->kind == DEV_KEYBOARD ? 256 : CODES;
    for (code = first; code < last; code++) {
        BYTE s = code_state(d, code);
        if (s == d->reported[code])
            continue;
        if (out && n < room) {
            DIDEVICEOBJECTDATA *e = (DIDEVICEOBJECTDATA *)((BYTE *)out + n * objsize);
            memset(e, 0, objsize);
            e->dwOfs = d->kind == DEV_KEYBOARD ? (DWORD)code : (DWORD)(12 + code - MOUSE_BUTTON0);
            e->dwData = s;
            e->dwTimeStamp = GetTickCount();
        } else if (out) {
            break;
        }
        if (!(flags & DIGDD_PEEK)) {
            d->reported[code] = s;
            capture_code(code, s & 0x80);
        }
        n++;
    }
    *inout = out ? n : 0;
    return DI_OK;
}

static HRESULT WINAPI create_device(void *self, REFGUID guid, void **out, void *outer)
{
    HRESULT hr = create_device_orig(self, guid, out, outer);
    int i, kind;
    void *old;

    if (FAILED(hr) || !*out)
        return hr;
    kind = IsEqualGUID(guid, &GUID_SysKeyboard) ? DEV_KEYBOARD :
           IsEqualGUID(guid, &GUID_SysMouse) ? DEV_MOUSE : DEV_OTHER;
    logmsg("input: dinput device %p kind %d", *out, kind);
    if (kind == DEV_OTHER)
        return hr;
    for (i = 0; i < 8 && devs[i].ptr; i++)
        ;
    if (i == 8)
        return hr;
    devs[i].ptr = *out;
    devs[i].kind = kind;
    /* all devices share one vtable: hook it once, pass through for devices we do not own */
    if ((old = patch_slot(*out, DEV_ACQUIRE, (void *)dev_acquire))) acquire_orig = (dev_fn_t)old;
    if ((old = patch_slot(*out, DEV_UNACQUIRE, (void *)dev_unacquire))) unacquire_orig = (dev_fn_t)old;
    if ((old = patch_slot(*out, DEV_POLL, (void *)dev_poll))) poll_orig = (dev_fn_t)old;
    if ((old = patch_slot(*out, DEV_GETDEVICESTATE, (void *)dev_get_state))) get_state_orig = (get_state_t)old;
    if ((old = patch_slot(*out, DEV_GETDEVICEDATA, (void *)dev_get_data))) get_data_orig = (get_data_t)old;
    if ((old = patch_slot(*out, DEV_SETCOOPLEVEL, (void *)dev_set_coop))) set_coop_orig = (set_coop_t)old;
    return hr;
}

/* Called with every IDirectInput8 the game creates */
void input_wrap_dinput(void *di)
{
    void *old;
    if (!di)
        return;
    if ((old = patch_slot(di, DI_CREATEDEVICE, (void *)create_device)))
        create_device_orig = (create_device_t)old;
}

/* ---- window ---- */

static WNDPROC wndproc_orig;
static HWND game_wnd;
static int place_x, place_y;    /* window position, physical pixels */

/* The game only ever sees itself as the active, focused window */
static LRESULT CALLBACK wndproc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ACTIVATEAPP:
        wp = TRUE;
        break;
    case WM_ACTIVATE:
        wp = WA_ACTIVE;
        break;
    case WM_NCACTIVATE:
        wp = TRUE;
        break;
    case WM_KILLFOCUS:
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    }
    return CallWindowProcA(wndproc_orig, wnd, msg, wp, lp);
}

/* The game's windows: the launcher runs before the first one; in background and couch mode they are
 * placed and never activated */
static HWND WINAPI hook_CreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR name, DWORD style, int x, int y,
                                        int w, int h, HWND parent, HMENU menu, HINSTANCE inst, LPVOID param)
{
    static int launched;
    HWND wnd;
    if (!parent && !launched) {
        launched = 1;
        launcher_run();
    }
    if (!synth)
        return CreateWindowExA(ex, cls, name, style, x, y, w, h, parent, menu, inst, param);
    if (!parent) {
        x = place_x;
        y = place_y;
        if (couch_active) {         /* borderless, exactly this player's share of the screen */
            style = WS_POPUP | (style & WS_VISIBLE);
            w = couch.w;
            h = couch.h;
        }
    }
    wnd = CreateWindowExA(ex | WS_EX_NOACTIVATE, cls, name, style, x, y, w, h, parent, menu, inst, param);
    if (wnd && !parent && !game_wnd) {
        game_wnd = wnd;
        wndproc_orig = (WNDPROC)SetWindowLongA(wnd, GWL_WNDPROC, (LONG)wndproc);
        PostMessageA(wnd, WM_ACTIVATEAPP, TRUE, 0);
        PostMessageA(wnd, WM_ACTIVATE, WA_ACTIVE, 0);
        PostMessageA(wnd, WM_SETFOCUS, 0, 0);
        logmsg("test: window %p subclassed, never activated", (void *)wnd);
    }
    return wnd;
}

static LONG WINAPI hook_SetWindowLongA(HWND wnd, int index, LONG value)
{
    /* the game setting its own window procedure later goes under ours */
    if (wnd == game_wnd && index == GWL_WNDPROC) {
        LONG old = (LONG)wndproc_orig;
        wndproc_orig = (WNDPROC)value;
        return old;
    }
    if (couch_active && wnd == game_wnd && index == GWL_STYLE)
        value = WS_POPUP | (value & WS_VISIBLE);
    return SetWindowLongA(wnd, index, value);
}

/* Shown without activation: behind every other window in test mode, in front for couch play */
static BOOL WINAPI hook_ShowWindow(HWND wnd, int cmd)
{
    BOOL r;
    if (cmd == SW_SHOW || cmd == SW_SHOWNORMAL || cmd == SW_RESTORE || cmd == SW_SHOWDEFAULT)
        cmd = SW_SHOWNOACTIVATE;
    else if (cmd == SW_SHOWMINIMIZED)
        cmd = SW_SHOWMINNOACTIVE;
    r = ShowWindow(wnd, cmd);
    SetWindowPos(wnd, test_background ? HWND_BOTTOM : HWND_TOP, place_x, place_y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
    return r;
}

static BOOL WINAPI hook_SetWindowPos(HWND wnd, HWND after, int x, int y, int w, int h, UINT flags)
{
    /* keep the z-order (HWND_TOP / HWND_TOPMOST would put the game over the desktop) and
     * our position */
    if (wnd == game_wnd) {
        x = place_x;
        y = place_y;
        if (couch_active) {
            w = couch.w;
            h = couch.h;
            flags &= ~SWP_NOSIZE;
        }
    }
    return SetWindowPos(wnd, after, x, y, w, h, flags | SWP_NOACTIVATE | SWP_NOZORDER);
}

static BOOL WINAPI hook_SetWindowPlacement(HWND wnd, const WINDOWPLACEMENT *wp)
{
    WINDOWPLACEMENT p = *wp;
    if (wnd == game_wnd) {
        OffsetRect(&p.rcNormalPosition, place_x - p.rcNormalPosition.left, place_y - p.rcNormalPosition.top);
        if (couch_active) {
            p.rcNormalPosition.right = p.rcNormalPosition.left + couch.w;
            p.rcNormalPosition.bottom = p.rcNormalPosition.top + couch.h;
        }
        if (p.showCmd == SW_SHOW || p.showCmd == SW_SHOWNORMAL || p.showCmd == SW_RESTORE)
            p.showCmd = SW_SHOWNOACTIVATE;
    }
    return SetWindowPlacement(wnd, &p);
}

/* ---- screenshots ---- */

#define GL_BACK                 0x0405
#define GL_PACK_ALIGNMENT       0x0D05
#define GL_BGR                  0x80E0
#define GL_UNSIGNED_BYTE        0x1401

static char shot_path[MAX_PATH];

/* coop_shot(name): <game dir>\coop\<name>.tga from the next frame shown */
void input_shot(const char *name)
{
    snprintf(shot_path, MAX_PATH, "%s\\%s.tga", coop_dir, name);
}

static void take_shot(HDC dc)
{
    static void (WINAPI *read_buffer)(unsigned);
    static void (WINAPI *pixel_store)(unsigned, int);
    static void (WINAPI *read_pixels)(int, int, int, int, unsigned, unsigned, void *);
    HMODULE gl = GetModuleHandleA("opengl32.dll");
    RECT rc;
    int w, h;
    BYTE *buf, hdr[18] = { 0 };
    FILE *f;

    if (!read_pixels && gl) {
        read_buffer = (void *)GetProcAddress(gl, "glReadBuffer");
        pixel_store = (void *)GetProcAddress(gl, "glPixelStorei");
        read_pixels = (void *)GetProcAddress(gl, "glReadPixels");
    }
    GetClientRect(WindowFromDC(dc), &rc);
    w = rc.right;
    h = rc.bottom;
    if (!read_pixels || w <= 0 || h <= 0 || !(buf = malloc(w * h * 3))) {
        logmsg("shot: cannot read the frame");
        return;
    }
    read_buffer(GL_BACK);
    pixel_store(GL_PACK_ALIGNMENT, 1);
    read_pixels(0, 0, w, h, GL_BGR, GL_UNSIGNED_BYTE, buf);
    hdr[2] = 2;                         /* uncompressed true colour, origin bottom left */
    hdr[12] = w & 0xff; hdr[13] = w >> 8;
    hdr[14] = h & 0xff; hdr[15] = h >> 8;
    hdr[16] = 24;
    if ((f = fopen(shot_path, "wb"))) {
        fwrite(hdr, 1, 18, f);
        fwrite(buf, 1, w * h * 3, f);
        fclose(f);
        logmsg("shot: %s (%dx%d)", shot_path, w, h);
    }
    free(buf);
}

/* Frame cap ([test] fps, default 60): background games leave the GPU to whatever the user runs */
static int fps_cap;

static void cap_frame(void)
{
    static double last;
    double wait = 1000.0 / fps_cap - (now_ms() - last);
    if (wait > 1.0)
        Sleep((DWORD)wait);
    last = now_ms();
}

/* Every frame shown: the HUD goes into the back buffer first; in test mode, the frame cap
 * and screenshots */
static BOOL WINAPI hook_SwapBuffers(HDC dc)
{
    hud_frame(dc);
    if (fps_cap > 0)
        cap_frame();
    if (shot_path[0]) {
        take_shot(dc);
        shot_path[0] = 0;
    }
    return SwapBuffers(dc);
}

/* Repoint one import of mdk2hd.exe */
static void patch_import(const char *dll, const char *name, void *fn)
{
    BYTE *base = (BYTE *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY *dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    IMAGE_IMPORT_DESCRIPTOR *imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir->VirtualAddress);

    for (; imp->Name; imp++) {
        IMAGE_THUNK_DATA *names, *addrs;
        if (_stricmp((char *)(base + imp->Name), dll) != 0)
            continue;
        names = (IMAGE_THUNK_DATA *)(base + imp->OriginalFirstThunk);
        addrs = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; names++, addrs++) {
            DWORD old;
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal) ||
                strcmp((char *)((IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData))->Name, name) != 0)
                continue;
            VirtualProtect(&addrs->u1.Function, 4, PAGE_READWRITE, &old);
            addrs->u1.Function = (DWORD)fn;
            VirtualProtect(&addrs->u1.Function, 4, old, &old);
            return;
        }
    }
    logmsg("test: no import %s", name);
}

/* [test] timescale k (background test games only, for the campaign bot): the game's clocks run k
 * times as fast from the moment it starts, and the frame cap rises k times, so each frame advances the
 * world by the same step and a level plays k times as fast. The bridge keeps the real clock (now_ms):
 * only the game's own imports are repointed. */
static double time_scale = 1;
static LARGE_INTEGER qpc_anchor;
static DWORD tick_anchor;
static BOOL WINAPI hook_QueryPerformanceCounter(LARGE_INTEGER *c)
{
    LARGE_INTEGER real;
    BOOL ok = QueryPerformanceCounter(&real);
    c->QuadPart = qpc_anchor.QuadPart + (LONGLONG)((double)(real.QuadPart - qpc_anchor.QuadPart) * time_scale);
    return ok;
}
static DWORD WINAPI hook_GetTickCount(void)
{
    return tick_anchor + (DWORD)((double)(GetTickCount() - tick_anchor) * time_scale);
}

/* DllMain: read the switches and, when on, take over the window calls before WinMain runs */
void input_init(void)
{
    char ini[MAX_PATH];

    snprintf(ini, MAX_PATH, "%s\\coop.ini", coop_dir);
    patch_import("gdi32.dll", "SwapBuffers", (void *)hook_SwapBuffers);
    patch_import("user32.dll", "CreateWindowExA", (void *)hook_CreateWindowExA);
    test_background = GetPrivateProfileIntA("test", "background", 0, ini);
    test_journal = GetPrivateProfileIntA("test", "journal", 0, ini);
    /* [couch]: this game is player slot+1 of count on one screen, window at x, y of w x h, played
     * with gamepad `pad` (default slot); see pad.c and dist/mdk2-couch.ps1 */
    couch.slot = GetPrivateProfileIntA("couch", "slot", -1, ini);
    couch.count = GetPrivateProfileIntA("couch", "count", 0, ini);
    couch_active = couch.slot >= 0 && couch.count >= 2;
    if (couch_active) {
        char look[32];
        couch.pad = GetPrivateProfileIntA("couch", "pad", couch.slot, ini);
        couch.x = GetPrivateProfileIntA("couch", "x", 0, ini);
        couch.y = GetPrivateProfileIntA("couch", "y", 0, ini);
        couch.w = GetPrivateProfileIntA("couch", "w", 960, ini);
        couch.h = GetPrivateProfileIntA("couch", "h", 540, ini);
        GetPrivateProfileStringA("couch", "look", "700", look, sizeof(look), ini);
        couch.look = (float)atof(look);
    }
    pad_enabled = GetPrivateProfileIntA("pad", "enabled", 1, ini) != 0;
    {
        char look[32];
        GetPrivateProfileStringA("pad", "look", "700", look, sizeof(look), ini);
        pad_look = (float)atof(look);
    }
    synth = test_background || couch_active;
    if (!synth)
        return;
    if (couch_active && !test_background) {
        place_x = couch.x;
        place_y = couch.y;
        fps_cap = GetPrivateProfileIntA("couch", "fps", 60, ini);
    } else {
        place_x = GetPrivateProfileIntA("test", "x", 0, ini);
        place_y = GetPrivateProfileIntA("test", "y", 0, ini);
        fps_cap = GetPrivateProfileIntA("test", "fps", 60, ini);
        {
            char scale[32];
            GetPrivateProfileStringA("test", "timescale", "1", scale, sizeof(scale), ini);
            time_scale = atof(scale);
            if (time_scale > 1 && time_scale <= 8) {
                QueryPerformanceCounter(&qpc_anchor);
                tick_anchor = GetTickCount();
                patch_import("kernel32.dll", "QueryPerformanceCounter", (void *)hook_QueryPerformanceCounter);
                patch_import("kernel32.dll", "GetTickCount", (void *)hook_GetTickCount);
                fps_cap = (int)(fps_cap * time_scale);
                logmsg("test: game clock x%.2f, frame cap %d", time_scale, fps_cap);
            } else {
                time_scale = 1;
            }
        }
    }
    timeBeginPeriod(1);     /* Sleep(1) granularity for the cap */
    patch_import("user32.dll", "SetWindowLongA", (void *)hook_SetWindowLongA);
    patch_import("user32.dll", "ShowWindow", (void *)hook_ShowWindow);
    patch_import("user32.dll", "SetWindowPos", (void *)hook_SetWindowPos);
    patch_import("user32.dll", "SetWindowPlacement", (void *)hook_SetWindowPlacement);
    if (couch_active)
        logmsg("couch: player %d of %d, pad %d, window %dx%d at %d, %d, %d fps", couch.slot + 1, couch.count,
               couch.pad, couch.w, couch.h, place_x, place_y, fps_cap);
    else
        logmsg("test: background mode at %d, %d, %d fps (scripted input only, window never activated)",
               place_x, place_y, fps_cap);
}

/* A key as Lua numbers it (DirectInput scan code, 1000 + mouse button, -1 none) -> this file's code
 * and the Windows key; 0 when it cannot be read here (none, or the wheel) */
int input_lua_key(int k, int *code, int *vk)
{
    *code = -1;
    *vk = 0;
    if (k >= 1000 && k <= 1003) {
        static const int mouse_vk[] = { VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1 };
        *code = 256 + (k - 1000);
        *vk = mouse_vk[k - 1000];
    } else if (k > 0 && k < 256) {
        *code = k;          /* extended keys (arrows, Home...) are 0x80 + their base code */
        *vk = (int)MapVirtualKeyA(k >= 128 ? 0xE000u | (unsigned)(k & 0x7f) : (unsigned)k, 3 /* VSC_TO_VK_EX */);
    }
    return *code >= 0;
}

/* coop_game_keys(which, key1, key2): which is GK_CHAT 0, GK_PING 1, GK_REVIVE 2, GK_VOICE 3, GK_EMOTE 4 or GK_CHAR 5; keys
 * are Lua's (input_lua_key; the wheel cannot be used) */
static void l_coop_game_keys(void)
{
    int which = (int)lua_getnumber(lua_getparam(1)), i;

    if (which < 0 || which >= GK_COUNT)
        return;
    for (i = 0; i < 2; i++) {
        unsigned int o = lua_getparam(2 + i);

        input_lua_key((o && lua_isnumber(o)) ? (int)lua_getnumber(o) : -1, &gamekeys[which].code[i], &gamekeys[which].vk[i]);
    }
    logmsg("game keys %d: %d/%d", which, gamekeys[which].code[0], gamekeys[which].code[1]);
}

/* coop_mouse(dx, dy): scripted mouse movement (background test mode only; the real mouse is not touched) */
/* coop_last_press() -> the newest press as a Lua key code (scan code, 1000+ mouse button, as the controls use)
 * and how many ms ago, or nil */
static void l_coop_last_press(void)
{
    if (last_press_code < 0)
        return;
    lua_pushnumber(last_press_code >= 256 ? 1000 + last_press_code - 256 : last_press_code);
    lua_pushnumber((double)(GetTickCount() - last_press_tick));
}

static void l_coop_mouse(void)
{
    unsigned int a = lua_getparam(1), b = lua_getparam(2);

    if (!synth)
        return;
    replay_input.mouse[0] += (a && lua_isnumber(a)) ? (LONG)lua_getnumber(a) : 0;
    replay_input.mouse[1] += (b && lua_isnumber(b)) ? (LONG)lua_getnumber(b) : 0;
}

void input_lua_init(void)
{
    tolua_function(NULL, "coop_mouse", l_coop_mouse);
    tolua_function(NULL, "coop_game_keys", l_coop_game_keys);
    tolua_function(NULL, "coop_last_press", l_coop_last_press);
}
