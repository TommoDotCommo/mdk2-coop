/*
 * Controls view: a keyboard and a mouse (or, on the Gamepad page, a controller) drawn over the
 * frame, every bound key coloured by what it does and labelled, with the full list of bindings
 * below (Options > Co-op > Controls). The look is the game's: black and amber, notched panels.
 * Lua (coop_dev.lua) owns the data: one page per situation (on foot, sniper mode, ...), it sends
 * the page with coop_ctrl_* and this file only draws it, through the HUD's GL state (hud.c).
 *
 * Layout, in window fractions: title and page tabs on top, the keyboard (main block, the
 * navigation block with the arrows, then the mouse) below them, the list of bindings under
 * that. The right quarter is left empty: the game's own menu (page, preset, edit) is drawn
 * there, and this is drawn after it.
 */
#include <stdio.h>
#include <string.h>
#include "bridge.h"

#define CV_KEYS     160
#define CV_LINES    64
#define CV_TABS     10

enum { G_NONE, G_MOVE, G_LOOK, G_JUMP, G_COMBAT, G_ITEM, G_SNIPER, G_SYSTEM, G_COOP, G_MENU, G_COUNT };

static const float group_rgb[G_COUNT][3] = {
    { 0.42f, 0.34f, 0.26f },    /* none */
    { 0.25f, 0.55f, 0.95f },    /* move */
    { 0.20f, 0.75f, 0.80f },    /* look */
    { 0.65f, 0.45f, 0.95f },    /* jump, jet, parachute */
    { 0.90f, 0.35f, 0.35f },    /* combat */
    { 0.35f, 0.80f, 0.45f },    /* inventory */
    { 0.95f, 0.65f, 0.25f },    /* sniper */
    { 0.78f, 0.74f, 0.66f },    /* system */
    { 0.95f, 0.50f, 0.80f },    /* co-op */
    { 0.95f, 0.85f, 0.35f },    /* menus */
};

static struct {
    int on;
    char title[64], sub[96], hint[128];
    char tab[CV_TABS][24];
    int ntabs, active;
    struct { int code, group; char label[40]; } key[CV_KEYS];
    int nkeys;
    struct { int group; char keys[48]; char desc[96]; } line[CV_LINES];
    int nlines;
    int focus;                  /* a key to outline (the one being rebound), or 0 */
    int pad;                    /* the Gamepad page: a controller instead of the keyboard */
} cv;

/* the pad's buttons are bound like keys, with these codes (coop_ctrl_padbtn) */
#define PAD_CODE 2000
static const char *const pad_btn[] = { "LS", "RS", "A", "B", "X", "Y", "START", "BACK", "LT", "RT", "LB", "RB" };

int ctrlview_on(void)
{
    return cv.on;
}

/* ---- the keyboard: DirectInput scan codes, positions in key units ---- */
struct kcap { short code; float x, y, w; const char *name; };

static const struct kcap main_keys[] = {
    { 1, 0, 0, 1, "Esc" },
    { 59, 2, 0, 1, "F1" }, { 60, 3, 0, 1, "F2" }, { 61, 4, 0, 1, "F3" }, { 62, 5, 0, 1, "F4" },
    { 63, 6.5f, 0, 1, "F5" }, { 64, 7.5f, 0, 1, "F6" }, { 65, 8.5f, 0, 1, "F7" }, { 66, 9.5f, 0, 1, "F8" },
    { 67, 11, 0, 1, "F9" }, { 68, 12, 0, 1, "F10" }, { 87, 13, 0, 1, "F11" }, { 88, 14, 0, 1, "F12" },
    { 41, 0, 1.3f, 1, "`" }, { 2, 1, 1.3f, 1, "1" }, { 3, 2, 1.3f, 1, "2" }, { 4, 3, 1.3f, 1, "3" },
    { 5, 4, 1.3f, 1, "4" }, { 6, 5, 1.3f, 1, "5" }, { 7, 6, 1.3f, 1, "6" }, { 8, 7, 1.3f, 1, "7" },
    { 9, 8, 1.3f, 1, "8" }, { 10, 9, 1.3f, 1, "9" }, { 11, 10, 1.3f, 1, "0" }, { 12, 11, 1.3f, 1, "-" },
    { 13, 12, 1.3f, 1, "=" }, { 14, 13, 1.3f, 2, "Bksp" },
    { 15, 0, 2.3f, 1.5f, "Tab" }, { 16, 1.5f, 2.3f, 1, "Q" }, { 17, 2.5f, 2.3f, 1, "W" },
    { 18, 3.5f, 2.3f, 1, "E" }, { 19, 4.5f, 2.3f, 1, "R" }, { 20, 5.5f, 2.3f, 1, "T" },
    { 21, 6.5f, 2.3f, 1, "Y" }, { 22, 7.5f, 2.3f, 1, "U" }, { 23, 8.5f, 2.3f, 1, "I" },
    { 24, 9.5f, 2.3f, 1, "O" }, { 25, 10.5f, 2.3f, 1, "P" }, { 26, 11.5f, 2.3f, 1, "[" },
    { 27, 12.5f, 2.3f, 1, "]" }, { 43, 13.5f, 2.3f, 1.5f, "\\" },
    { 58, 0, 3.3f, 1.75f, "Caps" }, { 30, 1.75f, 3.3f, 1, "A" }, { 31, 2.75f, 3.3f, 1, "S" },
    { 32, 3.75f, 3.3f, 1, "D" }, { 33, 4.75f, 3.3f, 1, "F" }, { 34, 5.75f, 3.3f, 1, "G" },
    { 35, 6.75f, 3.3f, 1, "H" }, { 36, 7.75f, 3.3f, 1, "J" }, { 37, 8.75f, 3.3f, 1, "K" },
    { 38, 9.75f, 3.3f, 1, "L" }, { 39, 10.75f, 3.3f, 1, ";" }, { 40, 11.75f, 3.3f, 1, "'" },
    { 28, 12.75f, 3.3f, 2.25f, "Enter" },
    { 42, 0, 4.3f, 2.25f, "Shift" }, { 44, 2.25f, 4.3f, 1, "Z" }, { 45, 3.25f, 4.3f, 1, "X" },
    { 46, 4.25f, 4.3f, 1, "C" }, { 47, 5.25f, 4.3f, 1, "V" }, { 48, 6.25f, 4.3f, 1, "B" },
    { 49, 7.25f, 4.3f, 1, "N" }, { 50, 8.25f, 4.3f, 1, "M" }, { 51, 9.25f, 4.3f, 1, "," },
    { 52, 10.25f, 4.3f, 1, "." }, { 53, 11.25f, 4.3f, 1, "/" }, { 54, 12.25f, 4.3f, 2.75f, "Shift" },
    { 29, 0, 5.3f, 1.25f, "Ctrl" }, { 56, 2.5f, 5.3f, 1.25f, "Alt" }, { 57, 3.75f, 5.3f, 6.25f, "Space" },
    { 184, 10, 5.3f, 1.25f, "Alt" }, { 157, 13.75f, 5.3f, 1.25f, "Ctrl" },
};

/* the block right of it: Insert/Home/Page keys and the arrows, 3 wide, x from the block's left */
static const struct kcap nav_keys[] = {
    { 210, 0, 1.3f, 1, "Ins" }, { 199, 1, 1.3f, 1, "Home" }, { 201, 2, 1.3f, 1, "PgUp" },
    { 211, 0, 2.3f, 1, "Del" }, { 207, 1, 2.3f, 1, "End" }, { 209, 2, 2.3f, 1, "PgDn" },
    { 200, 1, 4.3f, 1, "Up" },
    { 203, 0, 5.3f, 1, "Left" }, { 208, 1, 5.3f, 1, "Down" }, { 205, 2, 5.3f, 1, "Right" },
};

/* ---- Lua side ---- */
static double num_param(int n)
{
    unsigned int o = lua_getparam(n);
    return (o && lua_isnumber(o)) ? lua_getnumber(o) : 0.0;
}

/* a string argument, or "" (numbers come through as their text) */
static const char *str_param(int n)
{
    long len;
    unsigned int o = lua_getparam(n);
    return o ? luaL_check_lstr(n, &len) : "";
}

/* coop_ctrl_show(on) */
static void l_ctrl_show(void)
{
    cv.on = num_param(1) != 0.0;
}

/* coop_ctrl_clear(title, subtitle): start a new page */
static void l_ctrl_clear(void)
{
    cv.nkeys = cv.nlines = cv.ntabs = 0;
    cv.focus = cv.pad = 0;
    cv.hint[0] = 0;
    snprintf(cv.title, sizeof(cv.title), "%s", str_param(1));
    snprintf(cv.sub, sizeof(cv.sub), "%s", str_param(2));
}

/* coop_ctrl_tab(name, active): a page tab */
static void l_ctrl_tab(void)
{
    if (cv.ntabs >= CV_TABS)
        return;
    snprintf(cv.tab[cv.ntabs], sizeof(cv.tab[0]), "%s", str_param(1));
    if (num_param(2) != 0.0)
        cv.active = cv.ntabs;
    cv.ntabs++;
}

static void bind_key(int code, int group, const char *label)
{
    int i;

    for (i = 0; i < cv.nkeys; i++)
        if (cv.key[i].code == code) {         /* two actions on one key: "Fwd/Zoom" */
            size_t n = strlen(cv.key[i].label);
            if (n + strlen(label) + 2 < sizeof(cv.key[i].label))
                snprintf(cv.key[i].label + n, sizeof(cv.key[i].label) - n, "/%s", label);
            return;
        }
    if (cv.nkeys >= CV_KEYS)
        return;
    cv.key[cv.nkeys].code = code;
    cv.key[cv.nkeys].group = group >= 0 && group < G_COUNT ? group : 0;
    snprintf(cv.key[cv.nkeys].label, sizeof(cv.key[0].label), "%s", label);
    cv.nkeys++;
}

/* coop_ctrl_bind(code, group, label): this key (scan code, or 1000.. for the mouse) does this */
static void l_ctrl_bind(void)
{
    bind_key((int)num_param(1), (int)num_param(2), str_param(3));
}

/* coop_ctrl_padmode(on): draw a controller instead of the keyboard and mouse */
static void l_ctrl_padmode(void)
{
    cv.pad = num_param(1) != 0.0;
}

/* coop_ctrl_padbtn(name, group, label): this pad button (LS, RS, A, B, X, Y, START, BACK, LT, RT,
 * LB, RB) does this */
static void l_ctrl_padbtn(void)
{
    const char *name = str_param(1);
    unsigned i;

    for (i = 0; i < sizeof(pad_btn) / sizeof(pad_btn[0]); i++)
        if (!strcmp(name, pad_btn[i]))
            bind_key(PAD_CODE + (int)i, (int)num_param(2), str_param(3));
}

/* coop_ctrl_line(group, keys, description): one row of the list */
static void l_ctrl_line(void)
{
    int group = (int)num_param(1);

    if (cv.nlines >= CV_LINES)
        return;
    cv.line[cv.nlines].group = group >= 0 && group < G_COUNT ? group : 0;
    snprintf(cv.line[cv.nlines].keys, sizeof(cv.line[0].keys), "%s", str_param(2));
    snprintf(cv.line[cv.nlines].desc, sizeof(cv.line[0].desc), "%s", str_param(3));
    cv.nlines++;
}

static void l_ctrl_hint(void)
{
    snprintf(cv.hint, sizeof(cv.hint), "%s", str_param(1));
}

/* coop_ctrl_focus(code): outline this key (0 none) */
static void l_ctrl_focus(void)
{
    cv.focus = (int)num_param(1);
}

/* ---- drawing ---- */
/* The game's panels have cut corners: fill and edge of a box whose corners are notched by c pixels
 * (the diagonal is a staircase of t-pixel steps) */
static void panel(float x, float y, float w, float bh, int h, float c, float t, const float *fill, float fa,
                  const float *edge, float ea)
{
    float i;

    if (c > w * 0.3f) c = w * 0.3f;
    if (c > bh * 0.3f) c = bh * 0.3f;
    if (fill) {
        hud_box(x, y + c, w, bh - 2 * c, h, fill[0], fill[1], fill[2], fa);
        hud_box(x + c, y, w - 2 * c, c, h, fill[0], fill[1], fill[2], fa);
        hud_box(x + c, y + bh - c, w - 2 * c, c, h, fill[0], fill[1], fill[2], fa);
    }
    if (!edge)
        return;
    hud_box(x + c, y, w - 2 * c, t, h, edge[0], edge[1], edge[2], ea);
    hud_box(x + c, y + bh - t, w - 2 * c, t, h, edge[0], edge[1], edge[2], ea);
    hud_box(x, y + c, t, bh - 2 * c, h, edge[0], edge[1], edge[2], ea);
    hud_box(x + w - t, y + c, t, bh - 2 * c, h, edge[0], edge[1], edge[2], ea);
    for (i = 0; i < c; i += t) {                /* the four diagonals */
        hud_box(x + c - i - t, y + i, t, t, h, edge[0], edge[1], edge[2], ea);
        hud_box(x + w - c + i, y + i, t, t, h, edge[0], edge[1], edge[2], ea);
        hud_box(x + c - i - t, y + bh - i - t, t, t, h, edge[0], edge[1], edge[2], ea);
        hud_box(x + w - c + i, y + bh - i - t, t, t, h, edge[0], edge[1], edge[2], ea);
    }
}

static const float amber[3] = { 1.00f, 0.72f, 0.18f };
static const float amber_dim[3] = { 0.55f, 0.36f, 0.08f };
static const float ink[3] = { 0.07f, 0.045f, 0.02f };

/* s cut to fit maxw pixels in the current font, ending in "." when it was cut */
static void fit(char *dst, size_t size, const char *s, float maxw)
{
    size_t n;

    snprintf(dst, size, "%s", s);
    n = strlen(dst);
    if (hud_text_w(dst) <= maxw)
        return;
    while (n > 1 && hud_text_w(dst) + hud_text_w(".") > maxw)
        dst[--n] = 0;
    if (n < size - 1)
        strcat(dst, ".");
}

static int find_binding(int code)
{
    int i;

    for (i = 0; i < cv.nkeys; i++)
        if (cv.key[i].code == code)
            return i;
    return -1;
}

/* one key cap at (x, y), size (kw, kh) pixels, for scan code `code` */
static void draw_cap(float x, float y, float kw, float kh, int h, int code, const char *name)
{
    int b = find_binding(code);
    const float *rgb = group_rgb[b >= 0 ? cv.key[b].group : G_NONE];
    char buf[48];
    int sh = (hud_font_use(3), hud_font_h());

    float c = kh * 0.2f > 6 ? 6 : kh * 0.2f;

    if (b >= 0) {
        float fill[3] = { rgb[0] * 0.40f, rgb[1] * 0.40f, rgb[2] * 0.40f };
        panel(x, y, kw, kh, h, c, 2.0f, fill, 0.95f, rgb, 1.0f);
    } else {
        panel(x, y, kw, kh, h, c, 1.0f, ink, 0.9f, group_rgb[G_NONE], 0.5f);
    }
    if (code && code == cv.focus)
        panel(x - 4, y - 4, kw + 8, kh + 8, h, c + 2, 3.0f, NULL, 0, amber, 1.0f);
    hud_font_use(3);
    if (kh < sh * 2.6f) {                       /* too low for two lines: what it does, else its name */
        fit(buf, sizeof(buf), b >= 0 ? cv.key[b].label : name, kw - 6);
        hud_text(x + 4, y + kh * 0.5f + sh * 0.4f, h, buf, b >= 0 ? 1.0f : 0.5f, b >= 0 ? 0.97f : 0.42f, b >= 0 ? 0.88f : 0.32f);
        return;
    }
    fit(buf, sizeof(buf), name, kw - 6);
    if (b >= 0)
        hud_text(x + 4, y + sh + 1, h, buf, 0.95f, 0.82f, 0.55f);
    else
        hud_text(x + 4, y + sh + 1, h, buf, 0.5f, 0.42f, 0.32f);
    if (b >= 0) {
        fit(buf, sizeof(buf), cv.key[b].label, kw - 6);
        hud_text(x + 4, y + kh - 5, h, buf, 1.0f, 0.97f, 0.88f);
    }
}

static void draw_mouse(float x0, float y0, float u, int h)
{
    float bw = u * 1.35f, cw = u * 1.1f, bh = u * 2.0f, g = u * 0.06f, tot = bw * 2 + cw;
    const float *none = group_rgb[G_NONE];
    float by = y0 + u * 0.9f, ry = by + bh + u * 0.2f;

    hud_font_use(1);
    hud_text(x0, y0 + hud_font_h(), h, "Mouse", amber[0], amber[1], amber[2]);
    panel(x0 - 5, by - 5, tot + 10, bh + u * 2.5f + 10, h, 8, 1.0f, ink, 0.9f, none, 0.5f);
    draw_cap(x0, by, bw, bh, h, 1000, "Left");
    draw_cap(x0 + bw + g, by, cw - g * 2, bh * 0.34f, h, 1002, "Mid");
    draw_cap(x0 + bw + g, by + bh * 0.36f, cw - g * 2, bh * 0.31f, h, 1008, "Whl+");
    draw_cap(x0 + bw + g, by + bh * 0.69f, cw - g * 2, bh * 0.31f, h, 1009, "Whl-");
    draw_cap(x0 + bw + cw, by, bw, bh, h, 1001, "Right");
    draw_cap(x0, ry, tot * 0.5f - g, u * 0.95f, h, 1004, "Move X");
    draw_cap(x0 + tot * 0.5f + g, ry, tot * 0.5f - g, u * 0.95f, h, 1006, "Move Y");
    draw_cap(x0, ry + u * 1.1f, tot, u * 0.95f, h, 1003, "Side button");
}

/* the controller, in units of q pixels: 12 wide, 7.2 high */
static void draw_pad(float x0, float y0, float q, int h)
{
    const float *none = group_rgb[G_NONE];
    float cx = x0, cy = y0;
    int i;
    struct { const char *name; float x, y, w, hh; } el[] = {
        { "LT", 1.5f, 0.0f, 2.2f, 1.3f }, { "RT", 8.3f, 0.0f, 2.2f, 1.3f },
        { "LB", 1.0f, 1.45f, 3.4f, 0.8f }, { "RB", 7.6f, 1.45f, 3.4f, 0.8f },
        { "LS", 1.9f, 2.9f, 2.0f, 2.0f }, { "RS", 5.9f, 4.7f, 1.9f, 1.9f },
        { "Y", 8.55f, 2.7f, 1.1f, 1.1f }, { "X", 7.45f, 3.8f, 1.1f, 1.1f },
        { "B", 9.65f, 3.8f, 1.1f, 1.1f }, { "A", 8.55f, 4.9f, 1.1f, 1.1f },
        { "BACK", 4.5f, 3.1f, 1.2f, 0.6f }, { "START", 6.3f, 3.1f, 1.2f, 0.6f },
    };

    static const float body[3] = { 0.13f, 0.09f, 0.05f };

    hud_font_use(1);
    /* shoulders sit behind the body, which has two grips */
    panel(cx + 0.2f * q, cy + 2.4f * q, 11.6f * q, 3.0f * q, h, q * 0.9f, 1.5f, body, 0.95f, none, 0.8f);
    panel(cx + 0.7f * q, cy + 5.0f * q, 2.6f * q, 2.2f * q, h, q * 0.6f, 1.5f, body, 0.95f, none, 0.8f);
    panel(cx + 8.7f * q, cy + 5.0f * q, 2.6f * q, 2.2f * q, h, q * 0.6f, 1.5f, body, 0.95f, none, 0.8f);
    panel(cx + 5.3f * q, cy + 2.35f * q, 1.4f * q, 0.6f * q, h, q * 0.2f, 1.0f, NULL, 0, none, 0.5f);
    /* the D-pad is never bound on its own: it follows the left stick */
    {
        int b = find_binding(PAD_CODE);
        const float *rgb = group_rgb[b >= 0 ? cv.key[b].group : G_NONE];
        float fill[3] = { rgb[0] * 0.40f, rgb[1] * 0.40f, rgb[2] * 0.40f };
        float dx = cx + 3.45f * q, dy = cy + 4.9f * q;
        panel(dx, dy + 0.55f * q, 1.7f * q, 0.6f * q, h, 3, 1.5f, fill, 0.95f, rgb, 1.0f);
        panel(dx + 0.55f * q, dy, 0.6f * q, 1.7f * q, h, 3, 1.5f, fill, 0.95f, rgb, 1.0f);
    }
    for (i = 0; i < (int)(sizeof(el) / sizeof(el[0])); i++) {
        unsigned k;
        int code = 0;
        for (k = 0; k < sizeof(pad_btn) / sizeof(pad_btn[0]); k++)
            if (!strcmp(el[i].name, pad_btn[k]))
                code = PAD_CODE + (int)k;
        draw_cap(cx + el[i].x * q, cy + el[i].y * q, el[i].w * q, el[i].hh * q, h, code, el[i].name);
    }
}

/* The game's menus live in the middle 4:3 of the window (x in 0..1 is that part), so on a wide
 * window there is a free strip at each side. The menu column is the right part of the 4:3
 * area (menu x 0.78 in Lua, labels end there, values 0.14 further): keys left of it, the
 * mouse in the free strip right of it (or beside the arrows when the window is 4:3). */
#define MENU_LEFT_FRAC  0.545f

void ctrlview_draw(int w, int h)
{
    float area = h * 4.0f / 3.0f, offx, menu_l, menu_r, m = w * 0.025f, u, x, y, kx, ky, nx, mx, colw, rowh, kbr;
    int i, rows, cols, tabh, th, sh, side;
    char buf[160];

    if (!cv.on)
        return;
    if (area > w)
        area = (float)w;
    offx = (w - area) * 0.5f;
    menu_l = offx + area * MENU_LEFT_FRAC;
    menu_r = offx + area;
    side = (w - menu_r) > w * 0.11f;            /* room for the mouse right of the menu */
    kbr = menu_l - m;                           /* keys end here */

    /* dim everything but the menu's column */
    hud_box(0, 0, menu_l - m * 0.4f, h, h, 0.03f, 0.02f, 0.01f, 0.93f);
    hud_box(menu_l - m * 0.4f, 0, 3, h, h, amber_dim[0], amber_dim[1], amber_dim[2], 0.9f);
    if (side) {
        hud_box(menu_r, 0, w - menu_r, h, h, 0.03f, 0.02f, 0.01f, 0.93f);
        hud_box(menu_r - 3, 0, 3, h, h, amber_dim[0], amber_dim[1], amber_dim[2], 0.9f);
    }

    hud_font_use(2);
    th = hud_font_h();
    hud_text(m, h * 0.03f + th, h, cv.title, amber[0], amber[1], amber[2]);
    hud_box(m, h * 0.03f + th + 6, (kbr - m) * 0.5f, 4, h, amber[0], amber[1], amber[2], 1.0f);
    hud_box(m + (kbr - m) * 0.5f, h * 0.03f + th + 6, (kbr - m) * 0.5f, 2, h, amber_dim[0], amber_dim[1], amber_dim[2], 0.8f);
    hud_font_use(0);
    sh = hud_font_h();
    hud_text(m, h * 0.03f + th + sh * 1.6f + 6, h, cv.sub, 0.95f, 0.68f, 0.35f);
    y = h * 0.03f + th + sh * 2.5f + 6;
    tabh = sh + 12;
    x = m;
    hud_font_use(1);
    sh = hud_font_h();
    for (i = 0; i < cv.ntabs; i++) {
        float tw = (float)hud_text_w(cv.tab[i]) + 20;
        int on = i == cv.active;

        if (on)
            panel(x, y, tw, tabh, h, 6, 2.0f, amber_dim, 0.95f, amber, 1.0f);
        else
            panel(x, y, tw, tabh, h, 6, 1.0f, ink, 0.95f, group_rgb[G_NONE], 0.6f);
        hud_text(x + 10, y + sh + 7, h, cv.tab[i], on ? 1.0f : 0.6f, on ? 0.97f : 0.5f, on ? 0.85f : 0.38f);
        x += tw + 5;
    }

    /* keys: the main block is 15 units wide, the arrows block 3 */
    ky = y + tabh + h * 0.03f;
    if (side)
        u = (kbr - m) / (15.0f + 0.5f + 3.0f);
    else
        u = (kbr - m) / (15.0f + 0.5f + 3.0f + 0.5f + 3.7f);
    if (u > h * 0.085f)
        u = h * 0.085f;
    kx = m;
    nx = kx + u * 15.5f;
    if (cv.pad) {
        float q = (kbr - m) / 12.0f;
        if (q > u * 6.4f / 7.2f)
            q = u * 6.4f / 7.2f;
        draw_pad(m + ((kbr - m) - q * 12.0f) * 0.5f, ky - u * 0.4f, q, h);
    } else {
        for (i = 0; i < (int)(sizeof(main_keys) / sizeof(main_keys[0])); i++) {
            const struct kcap *k = &main_keys[i];
            draw_cap(kx + k->x * u, ky + k->y * u, k->w * u - 3, u - 3, h, k->code, k->name);
        }
        for (i = 0; i < (int)(sizeof(nav_keys) / sizeof(nav_keys[0])); i++) {
            const struct kcap *k = &nav_keys[i];
            draw_cap(nx + k->x * u, ky + k->y * u, k->w * u - 3, u - 3, h, k->code, k->name);
        }
    }
    if (cv.pad) {
        /* no mouse here */
    } else if (side) {
        float mu = (w - menu_r - m * 2) / 3.7f;
        if (mu > u * 1.2f)
            mu = u * 1.2f;
        mx = menu_r + m;
        draw_mouse(mx, ky + u * 0.4f, mu, h);
    } else {
        mx = nx + u * 3.5f;
        draw_mouse(mx, ky, u, h);
    }

    /* the list: every binding of the page, in columns */
    hud_font_use(1);
    sh = hud_font_h();
    rowh = sh * 1.55f;
    y = ky + u * 6.4f + h * 0.025f;
    hud_box(m, y - 6, kbr - m, 2, h, amber_dim[0], amber_dim[1], amber_dim[2], 0.8f);
    y += sh * 1.2f;
    cols = 1;
    colw = (kbr - m) / cols;
    rows = (int)((h * 0.955f - y) / rowh);
    if (rows < 1)
        rows = 1;
    while (cols < 4 && cv.nlines > rows * cols) {
        cols++;
        colw = (kbr - m) / cols;
    }
    for (i = 0; i < cv.nlines && i < rows * cols; i++) {
        float lx = m + (i / rows) * colw, ly = y + (i % rows) * rowh;
        const float *rgb = group_rgb[cv.line[i].group];
        float keyw = colw * 0.36f;

        hud_box(lx, ly - sh + 3, 6, sh, h, rgb[0], rgb[1], rgb[2], 1.0f);
        fit(buf, sizeof(buf), cv.line[i].keys, keyw - 12);
        hud_text(lx + 14, ly + 2, h, buf, 1.0f, 0.93f, 0.75f);
        fit(buf, sizeof(buf), cv.line[i].desc, colw - keyw - 18);
        hud_text(lx + keyw, ly + 2, h, buf, 0.86f, 0.74f, 0.56f);
    }
    if (cv.hint[0]) {
        fit(buf, sizeof(buf), cv.hint, kbr - m);
        hud_text(m, h * 0.985f, h, buf, 0.72f, 0.55f, 0.3f);
    }
    hud_font_use(0);
}

void ctrlview_lua_init(void)
{
    tolua_function(NULL, "coop_ctrl_show", l_ctrl_show);
    tolua_function(NULL, "coop_ctrl_clear", l_ctrl_clear);
    tolua_function(NULL, "coop_ctrl_tab", l_ctrl_tab);
    tolua_function(NULL, "coop_ctrl_bind", l_ctrl_bind);
    tolua_function(NULL, "coop_ctrl_line", l_ctrl_line);
    tolua_function(NULL, "coop_ctrl_hint", l_ctrl_hint);
    tolua_function(NULL, "coop_ctrl_focus", l_ctrl_focus);
    tolua_function(NULL, "coop_ctrl_padmode", l_ctrl_padmode);
    tolua_function(NULL, "coop_ctrl_padbtn", l_ctrl_padbtn);
}
