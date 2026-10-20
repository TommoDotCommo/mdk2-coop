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
#include <mmsystem.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

int test_background;

/* ---- synthetic presses ---- */

#define MOUSE_BUTTON0   256     /* coop_press codes: 0-255 keyboard scancodes, 256+ mouse buttons */
#define CODES           264

static double release_at[CODES];       /* held while now_ms() < release_at */

void input_press(int code, int hold_ms)
{
    if (code < 0 || code >= CODES)
        return;
    release_at[code] = now_ms() + hold_ms;
    logmsg("press %d for %d ms", code, hold_ms);
}

static int held(int code)
{
    return now_ms() < release_at[code];
}

/* ---- devices ---- */

enum { DEV_OTHER, DEV_KEYBOARD, DEV_MOUSE };

struct dev {
    void *ptr;
    int kind;
    BYTE reported[CODES];   /* last state given out through GetDeviceData */
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
    return dev_find(self) ? DI_OK : acquire_orig(self);
}

static HRESULT WINAPI dev_unacquire(void *self)
{
    return dev_find(self) ? DI_OK : unacquire_orig(self);
}

static HRESULT WINAPI dev_poll(void *self)
{
    return dev_find(self) ? DI_OK : poll_orig(self);
}

static HRESULT WINAPI dev_set_coop(void *self, HWND wnd, DWORD flags)
{
    return dev_find(self) ? DI_OK : set_coop_orig(self, wnd, flags);
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

    if (!d)
        return get_state_orig(self, size, out);
    memset(out, 0, size);
    if (d->kind == DEV_KEYBOARD) {
        for (i = 0; i < 256 && i < (int)size; i++)
            ((BYTE *)out)[i] = code_state(d, i);
    } else {
        /* DIMOUSESTATE(2): lX, lY, lZ, then 4 (8) buttons */
        for (i = 0; i < 8 && 12 + i < (int)size; i++)
            ((BYTE *)out)[12 + i] = code_state(d, MOUSE_BUTTON0 + i);
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
    room = *inout;
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
        if (!(flags & DIGDD_PEEK))
            d->reported[code] = s;
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
    logmsg("test: dinput device %p kind %d", *out, kind);
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
    if (!test_background || !di)
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

static HWND WINAPI hook_CreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR name, DWORD style, int x, int y,
                                        int w, int h, HWND parent, HMENU menu, HINSTANCE inst, LPVOID param)
{
    HWND wnd;
    if (!parent) {
        x = place_x;
        y = place_y;
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
    return SetWindowLongA(wnd, index, value);
}

/* Shown behind every other window, without activation */
static BOOL WINAPI hook_ShowWindow(HWND wnd, int cmd)
{
    BOOL r;
    if (cmd == SW_SHOW || cmd == SW_SHOWNORMAL || cmd == SW_RESTORE || cmd == SW_SHOWDEFAULT)
        cmd = SW_SHOWNOACTIVATE;
    else if (cmd == SW_SHOWMINIMIZED)
        cmd = SW_SHOWMINNOACTIVE;
    r = ShowWindow(wnd, cmd);
    SetWindowPos(wnd, HWND_BOTTOM, place_x, place_y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
    return r;
}

static BOOL WINAPI hook_SetWindowPos(HWND wnd, HWND after, int x, int y, int w, int h, UINT flags)
{
    /* keep the z-order (HWND_TOP / HWND_TOPMOST would put the game over the desktop) and
     * our position */
    if (wnd == game_wnd) {
        x = place_x;
        y = place_y;
    }
    return SetWindowPos(wnd, after, x, y, w, h, flags | SWP_NOACTIVATE | SWP_NOZORDER);
}

static BOOL WINAPI hook_SetWindowPlacement(HWND wnd, const WINDOWPLACEMENT *wp)
{
    WINDOWPLACEMENT p = *wp;
    if (wnd == game_wnd) {
        OffsetRect(&p.rcNormalPosition, place_x - p.rcNormalPosition.left, place_y - p.rcNormalPosition.top);
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

static BOOL WINAPI hook_SwapBuffers(HDC dc)
{
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

/* DllMain: read the switch and, when on, take over the window calls before WinMain runs */
void input_init(void)
{
    char ini[MAX_PATH];

    snprintf(ini, MAX_PATH, "%s\\coop.ini", coop_dir);
    test_background = GetPrivateProfileIntA("test", "background", 0, ini);
    if (!test_background)
        return;
    place_x = GetPrivateProfileIntA("test", "x", 0, ini);
    place_y = GetPrivateProfileIntA("test", "y", 0, ini);
    fps_cap = GetPrivateProfileIntA("test", "fps", 60, ini);
    timeBeginPeriod(1);     /* Sleep(1) granularity for the cap */
    patch_import("user32.dll", "CreateWindowExA", (void *)hook_CreateWindowExA);
    patch_import("user32.dll", "SetWindowLongA", (void *)hook_SetWindowLongA);
    patch_import("user32.dll", "ShowWindow", (void *)hook_ShowWindow);
    patch_import("user32.dll", "SetWindowPos", (void *)hook_SetWindowPos);
    patch_import("user32.dll", "SetWindowPlacement", (void *)hook_SetWindowPlacement);
    patch_import("gdi32.dll", "SwapBuffers", (void *)hook_SwapBuffers);
    logmsg("test: background mode at %d, %d, %d fps (scripted input only, window never activated)",
           place_x, place_y, fps_cap);
}
