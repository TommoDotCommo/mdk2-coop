/*
 * HUD: the other players on screen. Drawn into the back buffer just before the game shows
 * a frame (the SwapBuffers hook, input.c), so screenshots have it too.
 *
 * The game makes a legacy OpenGL context (wglCreateContext), so the fixed-function
 * pipeline is there: text from a Windows font (wglUseFontBitmaps display lists), boxes
 * with glRect. Everything the game had set is put back: attributes (glPushAttrib), matrices,
 * the shader program, the framebuffer and the active texture unit.
 *
 * Panel (top left), one row per other player: number, name, a health bar, hp, character,
 * how far away (game units), ping; "loading" while they are not in our level. Lua decides when it shows
 * (coop_hud: in a level, no menu up, [hud] show on).
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "bridge.h"
#include "combat_core.h"

#define GL_PROJECTION           0x1701
#define GL_MODELVIEW            0x1700
#define GL_TEXTURE              0x1702
#define GL_ALL_ATTRIB_BITS      0x000fffff
#define GL_BLEND                0x0BE2
#define GL_SRC_ALPHA            0x0302
#define GL_ONE_MINUS_SRC_ALPHA  0x0303
#define GL_DEPTH_TEST           0x0B71
#define GL_CULL_FACE            0x0B44
#define GL_TEXTURE_2D           0x0DE1
#define GL_LIGHTING             0x0B50
#define GL_ALPHA_TEST           0x0BC0
#define GL_FOG                  0x0B60
#define GL_SCISSOR_TEST         0x0C11
#define GL_STENCIL_TEST         0x0B90
#define GL_CURRENT_PROGRAM      0x8B8D
#define GL_FRAMEBUFFER          0x8D40
#define GL_FRAMEBUFFER_BINDING  0x8CA6
#define GL_ACTIVE_TEXTURE       0x84E0
#define GL_TEXTURE0             0x84C0
#define GL_VERSION              0x1F02
#define GL_RENDERER             0x1F01
#define GL_UNSIGNED_BYTE        0x1401
#define GL_BACK                 0x0405
#define GL_FRAMEBUFFER_SRGB     0x8DB9
#define GL_LINE_LOOP            0x0002
#define GL_LINE_SMOOTH          0x0B20
#define GL_QUADS                0x0007
#define GL_TEXTURE_ENV          0x2300
#define GL_TEXTURE_ENV_MODE     0x2200
#define GL_MODULATE             0x2100
#define GL_TEXTURE_MIN_FILTER   0x2801
#define GL_TEXTURE_MAG_FILTER   0x2800
#define GL_TEXTURE_WRAP_S       0x2802
#define GL_TEXTURE_WRAP_T       0x2803
#define GL_LINEAR               0x2601
#define GL_CLAMP_TO_EDGE        0x812F
#define GL_BGRA                 0x80E1
#define GL_RGBA                 0x1908
#define GL_UNPACK_ROW_LENGTH    0x0CF2

typedef void (WINAPI *gl_v_u)(unsigned);
static struct {
    void (WINAPI *PushAttrib)(unsigned);
    void (WINAPI *PopAttrib)(void);
    void (WINAPI *MatrixMode)(unsigned);
    void (WINAPI *PushMatrix)(void);
    void (WINAPI *PopMatrix)(void);
    void (WINAPI *LoadIdentity)(void);
    void (WINAPI *Ortho)(double, double, double, double, double, double);
    void (WINAPI *Viewport)(int, int, int, int);
    void (WINAPI *Enable)(unsigned);
    void (WINAPI *Disable)(unsigned);
    void (WINAPI *BlendFunc)(unsigned, unsigned);
    void (WINAPI *Color4f)(float, float, float, float);
    void (WINAPI *Rectf)(float, float, float, float);
    void (WINAPI *RasterPos2f)(float, float);
    void (WINAPI *Begin)(unsigned);
    void (WINAPI *End)(void);
    void (WINAPI *Vertex2f)(float, float);
    void (WINAPI *LineWidth)(float);
    void (WINAPI *ListBase)(unsigned);
    void (WINAPI *CallLists)(int, unsigned, const void *);
    void (WINAPI *GetIntegerv)(unsigned, int *);
    const char *(WINAPI *GetString)(unsigned);
    void (WINAPI *DrawBuffer)(unsigned);
    unsigned (WINAPI *GenLists)(int);
    void (WINAPI *DeleteLists)(unsigned, int);
    void (WINAPI *GenTextures)(int, unsigned *);
    void (WINAPI *BindTexture)(unsigned, unsigned);
    void (WINAPI *TexImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
    void (WINAPI *TexParameteri)(unsigned, unsigned, int);
    void (WINAPI *TexEnvi)(unsigned, unsigned, int);
    void (WINAPI *TexCoord2f)(float, float);
    void (WINAPI *PixelStorei)(unsigned, int);
    /* 1.3+ through wglGetProcAddress */
    void (WINAPI *UseProgram)(unsigned);
    void (WINAPI *BindFramebuffer)(unsigned, unsigned);
    void (WINAPI *ActiveTexture)(unsigned);
    BOOL (WINAPI *UseFontBitmapsA)(HDC, DWORD, DWORD, DWORD);    /* wgl, from opengl32.dll */
} gl;

static int ready = -1;          /* -1 not tried, 0 failed, 1 ok */
static int shown;               /* coop_hud(on) from Lua, this frame */
static int outlines;            /* coop_hud's third argument: outline the players through walls */
static BYTE *camera;            /* the game's camera gob (mdkGetCameraGob), from coop_hud */
static float fov_deg = 64.0f;   /* vertical field of view: from where puppets at known spots appear (1-2) */
static float head_z = 1.8f;     /* tag this far above a puppet's feet (Kurt is about 1.45 tall) */
static float body_h = 1.5f;     /* outline height, feet to top of the head */
static float body_hw = 0.5f;    /* and half its width */
#define OUT_NEAR 3.0f           /* outlines fade out between this distance (game units) and OUT_FAR */
#define OUT_FAR  11.0f
/* fonts: 0 is the HUD's, 1, 2 and 3 smaller / larger / tiny ones for the controls view (ctrlview.c) */
struct fnt { unsigned base; int px, h; int w[128]; HFONT hf; int game; };
static struct fnt fn[4];
static int fsel;                /* the font in use */
#define font_base fn[fsel].base
#define font_px   fn[fsel].px   /* font built for this text height (pixels) */
#define font_h    fn[fsel].h
#define char_w    fn[fsel].w    /* advance widths, for measuring text */
#define font      fn[fsel].hf
#define font_game fn[fsel].game  /* drawn with the game's own menu font instead of a Windows one */

static int load_gl(void)
{
    HMODULE m = GetModuleHandleA("opengl32.dll");
    PROC (WINAPI *get)(const char *) = m ? (void *)GetProcAddress(m, "wglGetProcAddress") : NULL;

#define GL1(name) (*(FARPROC *)&gl.name = GetProcAddress(m, "gl" #name))
#define GLX(name) (*(PROC *)&gl.name = get("gl" #name))
    if (!m || !get)
        return 0;
    GL1(PushAttrib); GL1(PopAttrib); GL1(MatrixMode); GL1(PushMatrix); GL1(PopMatrix); GL1(LoadIdentity);
    GL1(Ortho); GL1(Viewport); GL1(Enable); GL1(Disable); GL1(BlendFunc); GL1(Color4f); GL1(Rectf);
    GL1(RasterPos2f); GL1(ListBase); GL1(CallLists); GL1(GetIntegerv); GL1(GetString); GL1(DrawBuffer);
    GL1(GenLists); GL1(DeleteLists);
    GL1(Begin); GL1(End); GL1(Vertex2f); GL1(LineWidth);
    GL1(GenTextures); GL1(BindTexture); GL1(TexImage2D); GL1(TexParameteri); GL1(TexEnvi); GL1(TexCoord2f);
    GL1(PixelStorei);
    GLX(UseProgram); GLX(BindFramebuffer); GLX(ActiveTexture);
    *(FARPROC *)&gl.UseFontBitmapsA = GetProcAddress(m, "wglUseFontBitmapsA");
#undef GL1
#undef GLX
    if (!gl.PushAttrib || !gl.Rectf || !gl.CallLists || !gl.GenLists || !gl.GetString || !gl.UseFontBitmapsA ||
        !gl.Begin || !gl.End || !gl.Vertex2f || !gl.LineWidth)
        return 0;
    logmsg("hud: GL %s on %s; program %s, framebuffers %s", gl.GetString(GL_VERSION), gl.GetString(GL_RENDERER),
           gl.UseProgram ? "yes" : "no", gl.BindFramebuffer ? "yes" : "no");
    return 1;
}

/* The game's menu font (font.dds / font.lua from mdk2.tar, the launcher reads them): a sheet of 16x16 cells of
 * 32 pixels, any letter case. Used for the HUD's text; without the files the bold Windows font below. */
#define GFONT_SCALE 1.15f       /* a cell is this times the text height (the caps fill about 0.6 of a cell) */
#define GFONT_BASE  0.80f       /* the baseline is this far down a cell */
static struct image gfont_img;
static float gfont_adv[256];
static unsigned gfont_tex;
static int gfont_state;         /* 0 not tried, 1 loaded, -1 not there */

static int gfont_load(void)
{
    if (!gfont_state)
        gfont_state = art_font(&gfont_img, gfont_adv) && gfont_img.w == 512 && gfont_img.h == 512 ? 1 : -1;
    if (gfont_state < 0 && gfont_img.px) {
        free(gfont_img.px);
        gfont_img.px = NULL;
    }
    return gfont_state > 0;
}

/* the sheet as a texture, the first time text is drawn (the GL state is saved around the HUD) */
static int gfont_upload(void)
{
    if (gfont_tex)
        return 1;
    gl.GenTextures(1, &gfont_tex);
    gl.BindTexture(GL_TEXTURE_2D, gfont_tex);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 512, 512, 0, GL_BGRA, GL_UNSIGNED_BYTE, gfont_img.px);
    return gfont_tex != 0;
}

/* one pass of glyph quads at (x, y) baseline, y down */
static void gfont_pass(float x, float y, int h, const char *s, float r, float g, float b, float a)
{
    float cell = font_px * GFONT_SCALE, top = y - GFONT_BASE * cell;

    gl.Color4f(r, g, b, a);
    gl.Begin(GL_QUADS);
    for (; *s; s++) {
        int c = (unsigned char)*s < 128 && (unsigned char)*s >= 32 ? (unsigned char)*s : '?';
        float u0 = ((c % 16) * 32 + 0.5f) / 512.0f, u1 = ((c % 16) * 32 + 31.5f) / 512.0f;
        float v0 = ((c / 16) * 32 + 0.5f) / 512.0f, v1 = ((c / 16) * 32 + 31.5f) / 512.0f;

        gl.TexCoord2f(u0, v0); gl.Vertex2f(x, (float)h - top);
        gl.TexCoord2f(u1, v0); gl.Vertex2f(x + cell, (float)h - top);
        gl.TexCoord2f(u1, v1); gl.Vertex2f(x + cell, (float)h - top - cell);
        gl.TexCoord2f(u0, v1); gl.Vertex2f(x, (float)h - top - cell);
        x += char_w[c];
    }
    gl.End();
}

static void gfont_text(float x, float y, int h, const char *s, float r, float g, float b)
{
    if (!gfont_upload())
        return;
    gl.Enable(GL_TEXTURE_2D);
    gl.BindTexture(GL_TEXTURE_2D, gfont_tex);
    gl.TexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    gfont_pass(x + 1.5f, y + 1.5f, h, s, 0.0f, 0.0f, 0.0f, 0.75f);
    /* the sheet is grey chrome: lift the colour so the text keeps the brightness it had */
    gfont_pass(x, y, h, s, r * 1.3f > 1 ? 1 : r * 1.3f, g * 1.3f > 1 ? 1 : g * 1.3f, b * 1.3f > 1 ? 1 : b * 1.3f, 1.0f);
    gl.Disable(GL_TEXTURE_2D);
}

/* A bold Windows font as display lists, sized to the window */
static int make_font(HDC dc, int px)
{
    HFONT old;
    ABC abc[96];
    int i;

    if ((fsel == 0 || fsel == 2) && gfont_load()) {     /* the HUD text and the big headings: the game's font */
        int c;
        font_game = 1;
        font_px = font_h = px;
        for (c = 0; c < 128; c++)
            char_w[c] = (int)((gfont_adv[c] + 0.06f) * px * GFONT_SCALE + 0.5f);
        return 1;
    }
    font_game = 0;
    if (font_base && font_px == px)
        return 1;
    if (font_base)
        gl.DeleteLists(font_base, 96);
    if (font)
        DeleteObject(font);
    font = CreateFontA(-px, 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                       ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, "Tahoma");
    old = SelectObject(dc, font);
    font_base = gl.GenLists(96);
    /* the first call fails on some drivers; a second one works */
    if (font_base && !gl.UseFontBitmapsA(dc, 32, 96, font_base) && !gl.UseFontBitmapsA(dc, 32, 96, font_base)) {
        static int logged;
        if (logged++ < 3)
            logmsg("hud: no font (%dpx, error %lu)", px, (unsigned long)GetLastError());
        gl.DeleteLists(font_base, 96);
        font_base = 0;
    }
    if (!font_base) {
        SelectObject(dc, old);
        return 0;
    }
    memset(char_w, 0, sizeof(char_w));
    if (GetCharABCWidthsA(dc, 32, 127, abc))
        for (i = 0; i < 96; i++)
            char_w[32 + i] = abc[i].abcA + (int)abc[i].abcB + abc[i].abcC;
    SelectObject(dc, old);
    font_px = px;
    font_h = px;
    return 1;
}

static int text_width(const char *s)
{
    int w = 0;

    for (; *s; s++)
        if ((unsigned char)*s >= 32 && (unsigned char)*s < 128)
            w += char_w[(unsigned char)*s];
    return w;
}

/* text with its baseline at (x, y), y down from the top of an h-pixel window; a dark shadow first */
static void text(float x, float y, int h, const char *s, float r, float g, float b)
{
    int n = (int)strlen(s);

    if (font_game) {
        gfont_text(x, y, h, s, r, g, b);
        return;
    }
    gl.ListBase(font_base - 32);
    gl.Color4f(0.0f, 0.0f, 0.0f, 0.85f);
    gl.RasterPos2f(x + 1.0f, (float)h - y - 1.0f);
    gl.CallLists(n, GL_UNSIGNED_BYTE, s);
    gl.Color4f(r, g, b, 1.0f);
    gl.RasterPos2f(x, (float)h - y);
    gl.CallLists(n, GL_UNSIGNED_BYTE, s);
}

/* for ctrlview.c: the same primitives, and a font choice (0 HUD, 1 small, 2 large) */
void hud_font_use(int i) { fsel = i; }
int hud_font_h(void) { return font_h; }
int hud_text_w(const char *s) { return text_width(s); }
void hud_text(float x, float y, int h, const char *s, float r, float g, float b) { text(x, y, h, s, r, g, b); }

/* a box from (x, y) top left, y down */
static void box(float x, float y, float w, float bh, int h, float r, float g, float b, float a)
{
    gl.Color4f(r, g, b, a);
    gl.Rectf(x, (float)h - y - bh, x + w, (float)h - y);
}

void hud_box(float x, float y, float w, float bh, int h, float r, float g, float b, float a)
{
    box(x, y, w, bh, h, r, g, b, a);
}

static float dist_to(BYTE *gob)
{
    float *a = (float *)(gob + GOB_POSITION), *b;
    float dx, dy, dz;

    if (!gob_alive(local_player))
        return -1.0f;
    b = (float *)(local_player + GOB_POSITION);
    dx = a[0] - b[0]; dy = a[1] - b[1]; dz = a[2] - b[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

static const char *character(int type)
{
    switch (type) {
    case 100: return "Kurt";
    case 101: return "Max";
    case 102: return "Doc";
    default: return "";
    }
}

/* One row per other player: number, name, health bar and hp, character, distance, ping;
 * columns measured first so the panel fits its text */
/* v rotated by the inverse of unit quaternion q (w, x, y, z): world direction -> camera's frame */
static void unrotate(const float *q, const float *v, float *out)
{
    float w = q[0], x = -q[1], y = -q[2], z = -q[3];
    /* t = 2 * cross(q.xyz, v); out = v + w * t + cross(q.xyz, t) */
    float tx = 2.0f * (y * v[2] - z * v[1]), ty = 2.0f * (z * v[0] - x * v[2]), tz = 2.0f * (x * v[1] - y * v[0]);
    out[0] = v[0] + w * tx + (y * tz - z * ty);
    out[1] = v[1] + w * ty + (z * tx - x * tz);
    out[2] = v[2] + w * tz + (x * ty - y * tx);
}

/* World point -> pixel (x right, y down) in a w x h view; 0 if behind the camera. The camera
 * looks along its local +y with +z up and +x to the right (as the players: yaw 0 faces +y). */
static int project(const float *p, int w, int h, float *sx, float *sy)
{
    float d[3], c[3], k;

    if (!gob_alive(camera))
        return 0;
    d[0] = p[0] - ((float *)(camera + GOB_POSITION))[0];
    d[1] = p[1] - ((float *)(camera + GOB_POSITION))[1];
    d[2] = p[2] - ((float *)(camera + GOB_POSITION))[2];
    unrotate((float *)(camera + GOB_ORIENTATION), d, c);
    if (c[1] < 0.2f)
        return 0;
    k = (h * 0.5f) / tanf(fov_deg * 3.14159265f / 360.0f);
    *sx = w * 0.5f + c[0] / c[1] * k;
    *sy = h * 0.5f - c[2] / c[1] * k;
    return 1;
}

/* Tags already placed this frame: a new one that would overlap one goes above it */
static float placed[MAX_PLAYERS][4];   /* x0, y0, x1, y1 */
static int nplaced;

static void place_tag(float *sx, float *sy, float tw)
{
    int i, moved = 1, tries = 0;

    while (moved && tries++ < MAX_PLAYERS) {
        moved = 0;
        for (i = 0; i < nplaced; i++) {
            float x0 = *sx - tw * 0.5f - 4, x1 = *sx + tw * 0.5f + 4, y0 = *sy - font_h - 2, y1 = *sy + 4;
            if (x0 < placed[i][2] && x1 > placed[i][0] && y0 < placed[i][3] && y1 > placed[i][1]) {
                *sy = placed[i][1] - 6;
                moved = 1;
            }
        }
    }
    placed[nplaced][0] = *sx - tw * 0.5f - 4;
    placed[nplaced][1] = *sy - font_h - 2;
    placed[nplaced][2] = *sx + tw * 0.5f + 4;
    placed[nplaced][3] = *sy + 4;
    nplaced++;
}

/* The outline of a person (not the mesh: the engine's renderer is not hooked), as the right half of a
 * silhouette in units of the body: x across, as a fraction of the half width, y up from the feet to 1.
 * Drawn through a closed Catmull-Rom curve, so it has no corners. */
static const float person[][2] = {
    {0.00f, 1.00f}, {0.17f, 0.97f}, {0.21f, 0.89f}, {0.15f, 0.82f}, {0.11f, 0.77f},    /* head, neck */
    {0.30f, 0.74f}, {0.52f, 0.68f}, {0.60f, 0.52f}, {0.57f, 0.40f}, {0.47f, 0.41f},    /* shoulder, arm */
    {0.40f, 0.52f}, {0.33f, 0.50f}, {0.32f, 0.40f}, {0.30f, 0.20f}, {0.30f, 0.03f},    /* body, leg */
    {0.40f, 0.00f}, {0.06f, 0.00f}, {0.05f, 0.38f},                                    /* foot, crotch */
};
#define PERSON_N   ((int)(sizeof(person) / sizeof(person[0])))
#define OUT_PTS    (2 * PERSON_N - 1)

/* control point i of the whole loop: the right half down, then the left half back up */
static void person_pt(int i, float *x, float *y)
{
    i = (i % OUT_PTS + OUT_PTS) % OUT_PTS;
    if (i < PERSON_N) {
        *x = person[i][0];
        *y = person[i][1];
    } else {
        *x = -person[2 * PERSON_N - 1 - i][0];
        *y = person[2 * PERSON_N - 1 - i][1];
    }
}

/* A person's outline with its feet at (fx, fy), ppu pixels per game unit, wide: width factor, lying: on the
 * floor. Thin and soft: the colour at alpha a, with a faint dark line under it so it reads on bright
 * walls. No depth test, so it shows through walls. */
static void outline(float fx, float fy, float ppu, float wide, int lying, int h, float r, float g, float b, float a)
{
    float hw = body_hw * ppu * wide, ht = body_h * ppu;
    int i, k, pass;

    for (pass = 0; pass < 2; pass++) {
        gl.LineWidth(pass ? 1.5f : 3.0f);
        gl.Color4f(pass ? r : 0.0f, pass ? g : 0.0f, pass ? b : 0.0f, pass ? a : a * 0.3f);
        gl.Begin(GL_LINE_LOOP);
        for (i = 0; i < OUT_PTS; i++) {
            float p[4][2];
            for (k = 0; k < 4; k++)
                person_pt(i - 1 + k, &p[k][0], &p[k][1]);
            for (k = 0; k < 4; k++) {           /* 4 steps from point i to i + 1 */
                float t = k / 4.0f, t2 = t * t, t3 = t2 * t, nx, ny, x, y;
                nx = 0.5f * (2 * p[1][0] + (-p[0][0] + p[2][0]) * t + (2 * p[0][0] - 5 * p[1][0] + 4 * p[2][0] - p[3][0]) * t2 +
                             (-p[0][0] + 3 * p[1][0] - 3 * p[2][0] + p[3][0]) * t3);
                ny = 0.5f * (2 * p[1][1] + (-p[0][1] + p[2][1]) * t + (2 * p[0][1] - 5 * p[1][1] + 4 * p[2][1] - p[3][1]) * t2 +
                             (-p[0][1] + 3 * p[1][1] - 3 * p[2][1] + p[3][1]) * t3);
                if (lying) {                    /* head to the right, flattened by the view */
                    x = fx + (ny - 0.5f) * ht;
                    y = fy - 0.12f * ht - nx * hw * 0.45f;
                } else {
                    x = fx + nx * hw * 1.6f;
                    y = fy - ny * ht;
                }
                gl.Vertex2f(x, (float)h - y);
            }
        }
        gl.End();
    }
}

/* Every other player in the level, outlined on screen whatever is in the way. Standing: a person's outline
 * from the feet up; downed: lying down. Coloured by slot so they stay apart, faded out when close (where you
 * can see them and the line is only in the way). */
static void draw_outlines(int w, int h)
{
    static const float col[8][3] = {
        {0.95f, 0.80f, 0.45f}, {0.50f, 0.80f, 0.90f}, {0.60f, 0.88f, 0.60f}, {0.90f, 0.62f, 0.80f},
        {0.92f, 0.65f, 0.45f}, {0.72f, 0.65f, 0.92f}, {0.90f, 0.90f, 0.90f}, {0.68f, 0.80f, 0.50f}
    };
    int s;

    if (!gob_alive(camera))
        return;
    gl.Enable(GL_LINE_SMOOTH);
    for (s = 0; s < MAX_PLAYERS; s++) {
        float feet[3], top[3], fx, fy, tx, ty, ppu, dist, a, wide;
        const float *cp = (float *)(camera + GOB_POSITION);
        int type;

        if (!player_in_level(s) || !gob_alive(puppets[s]) || player_life(s) >= LIFE_DEAD)
            continue;
        memcpy(feet, puppets[s] + GOB_POSITION, sizeof(feet));
        memcpy(top, feet, sizeof(top));
        top[2] += body_h;
        dist = sqrtf((feet[0] - cp[0]) * (feet[0] - cp[0]) + (feet[1] - cp[1]) * (feet[1] - cp[1]) +
                     (feet[2] - cp[2]) * (feet[2] - cp[2]));
        a = (dist - OUT_NEAR) / (OUT_FAR - OUT_NEAR);       /* gone when close, full at a distance */
        if (a <= 0.02f)
            continue;
        a = (a > 1.0f ? 1.0f : a) * 0.6f;
        if (!project(feet, w, h, &fx, &fy) || !project(top, w, h, &tx, &ty))
            continue;
        ppu = fabsf(fy - ty) / body_h;                      /* pixels per game unit at their depth */
        if (ppu < 1.0f || fx < -w || fx > 2 * w || fy < -h || fy > 2 * h)
            continue;
        type = player_type(s);
        wide = type == 101 ? 1.35f : type == 102 ? 1.15f : 1.0f;      /* Max is broad, Doc a little */
        outline(fx, fy, ppu, wide, player_life(s) == LIFE_DOWNED, h, col[s % 8][0], col[s % 8][1], col[s % 8][2], a);
    }
    gl.Disable(GL_LINE_SMOOTH);
}

/* Name tags over the other players' heads, nearest first so overlapping ones stack upwards;
 * the ones off screen at its edge with an arrow */
/* a small speaker with sound bars, centred on cx, its bottom at `bottom` (screen y, down): above a
 * talking player's name tag */
static void draw_talk_icon(float cx, float bottom, int h)
{
    float u = font_h * 0.32f, x = cx - u * 3.5f, y = bottom;
    int i;

    box(x - 4, y - u * 5 - 4, u * 7 + 8, u * 5 + 8, h, 0.0f, 0.0f, 0.0f, 0.45f);
    box(x, y - u * 3.5f, u * 1.5f, u * 2, h, 0.45f, 1.0f, 0.55f, 1.0f);          /* speaker body */
    box(x + u * 1.5f, y - u * 4.5f, u * 1.2f, u * 4, h, 0.45f, 1.0f, 0.55f, 1.0f);
    for (i = 0; i < 2; i++) {                                                    /* sound bars */
        float bh = u * (2.0f + i * 1.6f);
        box(x + u * (3.4f + i * 1.7f), y - u * 2.5f - bh * 0.5f, u * 0.8f, bh, h, 0.45f, 1.0f, 0.55f, 1.0f);
    }
}

static void draw_tags(int w, int h)
{
    int order[MAX_PLAYERS], n = 0, i, j, s;

    for (s = 0; s < MAX_PLAYERS; s++)       /* a dead player's puppet is parked away: no tag */
        if (player_in_level(s) && gob_alive(puppets[s]) && player_life(s) < LIFE_DEAD)
            order[n++] = s;
    for (i = 1; i < n; i++)
        for (j = i; j > 0 && dist_to(puppets[order[j]]) < dist_to(puppets[order[j - 1]]); j--) {
            int t = order[j];
            order[j] = order[j - 1];
            order[j - 1] = t;
        }
    nplaced = 0;
    for (i = 0; i < n; i++) {
        float p[3], sx, sy, tw;
        const char *name;
        int on;

        s = order[i];
        memcpy(p, puppets[s] + GOB_POSITION, sizeof(p));
        p[2] += player_life(s) == LIFE_DOWNED ? head_z * 0.3f : head_z;     /* lying down */
        name = player_name(s);
        tw = (float)text_width(name);
        on = project(p, w, h, &sx, &sy);
        if (on && sx >= 0 && sx <= w && sy >= 0 && sy <= h) {
            place_tag(&sx, &sy, tw);
            box(sx - tw * 0.5f - 4, sy - font_h - 2, tw + 8, font_h + 6, h, 0.0f, 0.0f, 0.0f, 0.35f);
            if (voice_talking(s)) {
                draw_talk_icon(sx, sy - font_h - 6, h);
                placed[nplaced - 1][1] -= font_h * 1.9f;        /* the icon's room: tags stack above it */
                text(sx - tw * 0.5f, sy, h, name, 0.45f, 1.0f, 0.55f);
            } else
                text(sx - tw * 0.5f, sy, h, name, 0.95f, 0.85f, 0.4f);
        } else {
            /* off screen or behind us: pin to the nearest edge in its direction */
            float cx = w * 0.5f, cy = h * 0.5f, dx, dy, m;
            if (!on) {
                float d[3], c[3];
                d[0] = p[0] - ((float *)(camera + GOB_POSITION))[0];
                d[1] = p[1] - ((float *)(camera + GOB_POSITION))[1];
                d[2] = p[2] - ((float *)(camera + GOB_POSITION))[2];
                if (!gob_alive(camera))
                    continue;
                unrotate((float *)(camera + GOB_ORIENTATION), d, c);
                dx = c[0];
                dy = 0.0f;
                if (dx == 0.0f)
                    dx = 1.0f;
            } else {
                dx = sx - cx;
                dy = sy - cy;
            }
            m = fabsf(dx) / (cx - tw * 0.5f - 24) > fabsf(dy) / (cy - font_h - 24) ? fabsf(dx) / (cx - tw * 0.5f - 24)
                                                                                    : fabsf(dy) / (cy - font_h - 24);
            if (m <= 0.0f)
                m = 1.0f;
            sx = cx + dx / m;
            sy = cy + dy / m;
            place_tag(&sx, &sy, tw);
            box(sx - tw * 0.5f - 4, sy - font_h - 2, tw + 8, font_h + 6, h, 0.0f, 0.0f, 0.0f, 0.5f);
            if (voice_talking(s)) {
                draw_talk_icon(sx, sy - font_h - 6, h);
                placed[nplaced - 1][1] -= font_h * 1.9f;
            }
            text(sx - tw * 0.5f, sy, h, name, voice_talking(s) ? 0.45f : 0.95f, voice_talking(s) ? 1.0f : 0.85f,
                 voice_talking(s) ? 0.55f : 0.4f);
            text(dx < 0 ? sx - tw * 0.5f - font_h : sx + tw * 0.5f + 6, sy, h, dx < 0 ? "<" : ">", 0.95f, 0.85f, 0.4f);
        }
    }
}

BYTE *hud_camera(void)
{
    return camera;
}

/* one colour per player, for chat names and pings */
static void slot_colour(int slot, float *c)
{
    static const float pal[16][3] = { { 0.95f, 0.85f, 0.4f }, { 0.45f, 0.85f, 1.0f }, { 0.55f, 1.0f, 0.55f },
                                     { 1.0f, 0.6f, 0.85f }, { 1.0f, 0.65f, 0.3f }, { 0.75f, 0.65f, 1.0f },
                                     { 0.6f, 1.0f, 0.9f }, { 1.0f, 0.55f, 0.5f },
                                     { 0.8f, 0.9f, 0.65f }, { 0.55f, 0.7f, 1.0f },
                                     { 1.0f, 0.8f, 0.65f }, { 0.9f, 0.55f, 1.0f },
                                     { 0.65f, 0.95f, 0.8f }, { 1.0f, 0.9f, 0.75f },
                                     { 0.8f, 0.7f, 0.95f }, { 0.75f, 0.95f, 1.0f } };
    memcpy(c, pal[slot & 15], sizeof(pal[0]));
}

/* Pings: a diamond-ish marker (two boxes) at the point, name and distance beside it; off screen,
 * pinned to the edge in its direction like the name tags */
static void draw_pings(int w, int h)
{
    int s;

    for (s = 0; s < MAX_PLAYERS; s++) {
        float p[3], sx, sy, c[3], d = -1.0f, a, m = font_h * 0.5f;
        double age;
        char label[PLAYER_NAME + 16];
        int on;

        if (!ping_get(s, p, &age))
            continue;
        a = ping_life_left(age) < 0.25f ? ping_life_left(age) * 4.0f : 1.0f;
        slot_colour(s, c);
        if (gob_alive(camera)) {
            float *q = (float *)(camera + GOB_POSITION);
            d = sqrtf((p[0] - q[0]) * (p[0] - q[0]) + (p[1] - q[1]) * (p[1] - q[1]) + (p[2] - q[2]) * (p[2] - q[2]));
        }
        snprintf(label, sizeof(label), d >= 0 ? "%s  %.0f" : "%s", player_name(s), d);
        on = project(p, w, h, &sx, &sy);
        if (!on) {
            float cc[3], dd[3];
            if (!gob_alive(camera))
                continue;
            dd[0] = p[0] - ((float *)(camera + GOB_POSITION))[0];
            dd[1] = p[1] - ((float *)(camera + GOB_POSITION))[1];
            dd[2] = p[2] - ((float *)(camera + GOB_POSITION))[2];
            unrotate((float *)(camera + GOB_ORIENTATION), dd, cc);
            sx = w * 0.5f + (cc[0] < 0 ? -1.0f : 1.0f) * w;
            sy = h * 0.5f;
        }
        if (sx < m * 2) sx = m * 2;
        if (sx > w - m * 2) sx = w - m * 2;
        if (sy < m * 2) sy = m * 2;
        if (sy > h - m * 2) sy = h - m * 2;
        box(sx - m - 2, sy - m - 2, m * 2 + 4, m * 2 + 4, h, 0.0f, 0.0f, 0.0f, 0.6f * a);
        box(sx - m, sy - m, m * 2, m * 2, h, c[0], c[1], c[2], 0.9f * a);
        box(sx - m * 0.4f, sy - m * 0.4f, m * 0.8f, m * 0.8f, h, 0.0f, 0.0f, 0.0f, 0.7f * a);
        text(sx - text_width(label) * 0.5f, sy - m - 6, h, label, c[0], c[1], c[2]);
    }
}

/* Speech bubbles over heads: a player's latest message for a few seconds, "..." while they type */
static void draw_bubbles(int w, int h)
{
    int s;

    for (s = 0; s < MAX_PLAYERS; s++) {
        BYTE *gob = s == my_slot ? local_player : puppets[s];
        const char *msg = NULL;
        char dots[4] = "...";
        double age;
        float p[3], sx, sy, tw, c[3];
        int typing = chat_typing(s) || (s == my_slot && chat_is_open());

        if (!gob_alive(gob) || (s != my_slot && (!player_in_level(s) || player_life(s) >= LIFE_DEAD)))
            continue;
        if (!typing && !(chat_last(s, &msg, &age) && age < 8000.0))
            continue;
        if (typing) {
            snprintf(dots, sizeof(dots), "%.*s", 1 + (int)(now_ms() / 400) % 3, "...");
            msg = dots;
        }
        memcpy(p, gob + GOB_POSITION, sizeof(p));
        p[2] += head_z + 0.7f;
        if (!project(p, w, h, &sx, &sy) || sx < 0 || sx > w || sy < 0 || sy > h)
            continue;
        if (s != my_slot && voice_talking(s))
            sy -= font_h * 1.9f;        /* above the speaker icon over their name tag */
        tw = (float)text_width(msg);
        slot_colour(s, c);
        box(sx - tw * 0.5f - 6, sy - font_h - 3, tw + 12, font_h + 9, h, 0.0f, 0.0f, 0.0f, 0.6f);
        box(sx - tw * 0.5f - 6, sy + font_h * 0.0f + 6, tw + 12, 2, h, c[0], c[1], c[2], 0.9f);
        text(sx - tw * 0.5f, sy, h, msg, 0.97f, 0.97f, 0.97f);
    }
}

/* Chat: the last messages above the bottom left corner, recent ones only unless the line is open;
 * the typing line under them */
static void draw_chat(int w, int h)
{
    const int keep = 6;
    float x = font_h * 0.8f, y = h - font_h * 2.2f, line_h = font_h * 1.3f;
    int open = chat_is_open(), i, slot, shown_lines = 0, idx[6];
    const char *msg;
    double age;
    char buf[96];

    for (i = 0; i < keep && chat_line(i, &slot, &msg, &age); i++)
        if (open ? age < 60000.0 : age < 10000.0)
            idx[shown_lines++] = i;
    if (!shown_lines && !open)
        return;
    if (open)
        y -= line_h;
    for (i = 0; i < shown_lines; i++) {
        float c[3], nw, base = y - (shown_lines - 1 - i) * line_h;
        char name[PLAYER_NAME + 4];

        chat_line(idx[i], &slot, &msg, &age);
        slot_colour(slot < 0 ? 0 : slot, c);
        if (slot < 0) {             /* the game's own notice: no speaker */
            c[0] = c[1] = c[2] = 0.7f;
            name[0] = 0;
        } else {
            snprintf(name, sizeof(name), "%s:", player_name(slot));
        }
        nw = name[0] ? (float)text_width(name) + text_width(" ") : 0.0f;
        box(x - 4, base - font_h, nw + text_width(msg) + 8, line_h, h, 0.0f, 0.0f, 0.0f, 0.4f);
        if (name[0])
            text(x, base, h, name, c[0], c[1], c[2]);
        text(x + nw, base, h, msg, slot < 0 ? 0.8f : 0.95f, slot < 0 ? 0.8f : 0.95f, slot < 0 ? 0.8f : 0.95f);
    }
    if (open) {
        snprintf(buf, sizeof(buf), "Say: %s%s", chat_input(), (int)(now_ms() / 500) & 1 ? "_" : " ");
        box(x - 4, y + line_h - font_h, w * 0.4f, line_h, h, 0.0f, 0.0f, 0.0f, 0.6f);
        text(x, y + line_h, h, buf, 1.0f, 1.0f, 1.0f);
    }
}

/* revive.c: one or two centred lines in the lower third, with a progress bar when >= 0 */
static char status1[96], status2[96];
static float status_progress = -1.0f;

void hud_status(const char *line1, const char *line2, float progress)
{
    snprintf(status1, sizeof(status1), "%s", line1 ? line1 : "");
    snprintf(status2, sizeof(status2), "%s", line2 ? line2 : "");
    status_progress = progress;
}

/* coop_emote_caption: what the emote browser (coop_dev.lua) is playing, drawn at the top while an emote runs */
static char caption1[96], caption2[96];

/* coop_banner: the cutscene line (coop_dev.lua, "Cutscenes"), drawn whether or not the panel shows */
static char banner1[96], banner2[96];
static float banner_progress = -1.0f;

static int dump_rows;           /* coop_hud_dump: log the next frame's rows */
static int dump_status;         /* and its status lines */
static int frames_drawn;        /* frames with the HUD drawn, for tests */

static void draw_panel(int w, int h)
{
    char info[MAX_PLAYERS][48], ping[MAX_PLAYERS][16], name[MAX_PLAYERS][PLAYER_NAME + 8];
    int hp[MAX_PLAYERS], maxhp[MAX_PLAYERS], live[MAX_PLAYERS];
    float pad = font_h * 0.6f, gap = font_h * 0.8f, row = font_h * 1.5f;
    float x0 = font_h * 0.8f, y0 = font_h * 0.8f, numw = text_width("8") + gap;
    float namew = 0, barw = font_h * 4.5f, infow = 0, pingw = 0, panelw, x, y;
    int s, rows = 0;

    for (s = 0; s < MAX_PLAYERS; s++) {
        live[s] = player_connected(s);
        if (!live[s])
            continue;
        rows++;
        snprintf(name[s], sizeof(name[s]), "%s", player_name(s));
        player_health(s, &hp[s], &maxhp[s]);
        if (!player_in_level(s))
            snprintf(info[s], sizeof(info[s]), "loading");
        else if (player_life(s) >= LIFE_DEAD)
            snprintf(info[s], sizeof(info[s]), "%s  dead", character(player_type(s)));
        else if (gob_alive(puppets[s]) && gob_alive(local_player))
            snprintf(info[s], sizeof(info[s]), "%d  %s  %.0f away%s", hp[s], character(player_type(s)),
                     dist_to(puppets[s]), player_life(s) == LIFE_DOWNED ? "  DOWN" : "");
        else
            snprintf(info[s], sizeof(info[s]), "%d  %s", hp[s], character(player_type(s)));
        ping[s][0] = 0;
        if (player_ping(s) > 0)
            snprintf(ping[s], sizeof(ping[s]), "%d ms", player_ping(s));
        if (text_width(name[s]) > namew)
            namew = (float)text_width(name[s]);
        if (text_width(info[s]) > infow)
            infow = (float)text_width(info[s]);
        if (text_width(ping[s]) > pingw)
            pingw = (float)text_width(ping[s]);
    }
    if (!rows)
        return;
    panelw = pad + numw + namew + gap + barw + gap + infow + (pingw > 0 ? gap + pingw : 0) + pad;
    if (panelw > w - x0 * 2)
        panelw = w - x0 * 2;
    box(x0, y0, panelw, pad * 2 + row * (rows - 1) + font_h, h, 0.02f, 0.03f, 0.06f, 0.55f);
    y = y0 + pad;
    for (s = 0; s < MAX_PLAYERS; s++) {
        float base = y + font_h * 0.8f;
        char num[8];

        if (!live[s])
            continue;
        x = x0 + pad;
        snprintf(num, sizeof(num), "%d", s + 1);
        text(x, base, h, num, 1.0f, 0.78f, 0.25f);
        x += numw;
        if (voice_talking(s))
            text(x, base, h, name[s], 0.45f, 1.0f, 0.55f);
        else
            text(x, base, h, name[s], 0.92f, 0.95f, 1.0f);
        x += namew + gap;
        if (player_in_level(s) && maxhp[s] > 0) {
            float f = hp[s] > 0 ? (float)hp[s] / maxhp[s] : 0.0f, bh = font_h * 0.5f, by = y + font_h * 0.3f;
            if (f > 1.0f)
                f = 1.0f;
            box(x, by, barw, bh, h, 0.25f, 0.06f, 0.06f, 0.85f);
            box(x, by, barw * f, bh, h, f > 0.3f ? 0.25f : 0.95f, f > 0.3f ? 0.85f : 0.25f, 0.3f, 0.95f);
        }
        x += barw + gap;
        text(x, base, h, info[s], 0.8f, 0.85f, 0.9f);
        if (ping[s][0])
            text(x0 + panelw - pad - text_width(ping[s]), base, h, ping[s], 0.55f, 0.62f, 0.72f);
        if (dump_rows)
            logmsg("hud row: %d %s | %s | %s", s + 1, name[s], info[s], ping[s]);
        y += row;
    }
    dump_rows = 0;
}

/* emote.c's wheel: the slots round the middle of the screen, the chosen one lit, a dot for the pointer */
static void draw_emote_wheel(int w, int h)
{
    const char *labels[8];
    int n, sel, k;
    float vx, vy, cx = w * 0.5f, cy = h * 0.5f, rad = h * 0.22f, dot = font_h * 0.5f;
    const char *mid, *stay;

    if (!emote_wheel_info(&n, &sel, &vx, &vy, labels, &stay))
        return;
    box(cx - rad - font_h * 4.0f, cy - rad - font_h * 2.0f, (rad + font_h * 4.0f) * 2, (rad + font_h * 2.0f) * 2, h,
        0.0f, 0.0f, 0.0f, 0.35f);
    for (k = 0; k < n; k++) {
        float a = 6.2831853f * (float)k / (float)n, tw = (float)text_width(labels[k]);
        float bx = cx + rad * sinf(a), by = cy - rad * cosf(a), bw = tw + font_h, bh = font_h * 1.6f;

        if (k == sel) {
            box(bx - bw * 0.5f - 3, by - bh * 0.5f - 3, bw + 6, bh + 6, h, 1.0f, 0.85f, 0.3f, 0.95f);
            box(bx - bw * 0.5f, by - bh * 0.5f, bw, bh, h, 0.25f, 0.2f, 0.05f, 0.95f);
            text(bx - tw * 0.5f, by + font_h * 0.35f, h, labels[k], 1.0f, 0.9f, 0.4f);
        } else {
            box(bx - bw * 0.5f, by - bh * 0.5f, bw, bh, h, 0.02f, 0.03f, 0.06f, 0.75f);
            text(bx - tw * 0.5f, by + font_h * 0.35f, h, labels[k], 0.8f, 0.85f, 0.9f);
        }
    }
    box(cx - 2, cy - rad * 0.35f, 4, rad * 0.7f, h, 1.0f, 1.0f, 1.0f, 0.06f);
    box(cx - rad * 0.35f, cy - 2, rad * 0.7f, 4, h, 1.0f, 1.0f, 1.0f, 0.06f);
    box(cx + vx * rad * 0.6f - dot * 0.5f, cy + vy * rad * 0.6f - dot * 0.5f, dot, dot, h, 1.0f, 0.85f, 0.3f, 0.95f);
    mid = sel >= 0 ? "Let go to choose" : stay;
    text(cx - text_width(mid) * 0.5f, cy + rad + font_h * 2.4f, h, mid, 0.9f, 0.9f, 0.9f);
}

/* cutscene banner: the voted/waiting state and its countdown, low on the picture */
static void draw_banner(int w, int h)
{
    float y = h * 0.84f, bw = w * 0.34f;
    int lines = banner2[0] ? 2 : 1;

    if (!banner1[0] && !banner2[0])
        return;
    box(w * 0.5f - bw * 0.5f - 14, y - font_h - 6, bw + 28, font_h * lines + 14 + (banner_progress >= 0.0f ? 12 : 0), h,
        0, 0, 0, 0.55f);
    if (banner1[0])
        text(w * 0.5f - text_width(banner1) * 0.5f, y, h, banner1, 1.0f, 0.85f, 0.3f);
    if (banner2[0])
        text(w * 0.5f - text_width(banner2) * 0.5f, y + font_h, h, banner2, 0.9f, 0.9f, 0.9f);
    if (banner_progress >= 0.0f) {
        float by = y + font_h * lines - font_h + 8;
        box(w * 0.5f - bw * 0.5f, by, bw, 6, h, 0.2f, 0.2f, 0.2f, 0.8f);
        box(w * 0.5f - bw * 0.5f, by, bw * (banner_progress > 1.0f ? 1.0f : banner_progress), 6, h, 0.95f, 0.75f, 0.25f, 0.9f);
    }
    if (dump_status)
        logmsg("hud banner: %s | %s | %.2f", banner1, banner2, banner_progress);
}

/* cutscene box, bottom right: the time the movie has left, the skip key and the vote (coop_movie_box) */
static char mbox[3][96];

static void draw_movie_box(int w, int h)
{
    float tw = 0, x, y, m = h * 0.03f;
    int i, n = 0;

    for (i = 0; i < 3; i++)
        if (mbox[i][0]) {
            n++;
            if (text_width(mbox[i]) > tw)
                tw = text_width(mbox[i]);
        }
    if (!n)
        return;
    x = w - m - tw;
    y = h - m - font_h * (n - 1) - 4;
    if (voice_talking(-1))
        y -= font_h * 2.0f;             /* above the MIC box */
    box(x - 12, y - font_h - 4, tw + 24, font_h * n + 12, h, 0, 0, 0, 0.55f);
    for (i = 0; i < 3; i++)
        if (mbox[i][0]) {
            float r = i ? 0.9f : 1.0f, g = i ? 0.9f : 0.85f, b = i ? 0.9f : 0.3f;
            text(x + tw - text_width(mbox[i]), y, h, mbox[i], r, g, b);
            y += font_h;
        }
    if (dump_status)
        logmsg("hud moviebox: %s | %s | %s", mbox[0], mbox[1], mbox[2]);
}

/* Text is measured with the actual menu font before drawing; bounded pools only. */
static void combat_text(int x, int y, int width, int h, const char *line, const float *c)
{
    char clipped[128];
    size_t n;
    snprintf(clipped, sizeof(clipped), "%s", line);
    n = strlen(clipped);
    while (n && hud_text_w(clipped) > width) clipped[--n] = 0;
    hud_text(x, y, h, clipped, c[0], c[1], c[2]);
}
static void draw_combat(int w, int h)
{
    const struct cb_state *s = combat_state();
    struct cb_layout l;
    const float white[3] = {0.9f, 0.9f, 0.9f};
    float c[3], sx, sy;
    unsigned i;
    int row = 0, p, first, count = 0, slots[CB_PLAYERS];
    char line[128];
    uint64_t now = (uint64_t)now_ms();
    hud_font_use(1);
    l = cb_layout(w, h, hud_font_h());
    /* At most 32 recent world markers. Rising numbers share the chat palette. */
    for (i = 0; i < s->size && i < 32; i++) {
        const struct cb_line *hit = &s->lines[(s->next + CB_KEEP - 1 - i) % CB_KEEP];
        double age = (double)(now - hit->at);
        float pos[3];
        if (age > 1000) continue;
        memcpy(pos, hit->e.pos, sizeof(pos)); pos[2] += 1.0f + (float)age / 700.0f;
        if (!project(pos, w, h, &sx, &sy)) continue;
        slot_colour(hit->e.attacker, c);
        snprintf(line, sizeof(line), "+%u%s", hit->e.amount, hit->e.flags & CB_KILL ? " K" : "");
        if (sx < 8 || sx + hud_text_w(line) > w - 8 || sy < hud_font_h() + 8 || sy > h - 8) continue;
        /* Cross at the reported target position, number rising above it. */
        hud_text(sx, sy, h, line, c[0], c[1], c[2]);
        if (age < 250 && project(hit->e.pos, w, h, &sx, &sy) && sx >= 6 && sx <= w-6 && sy >= 6 && sy <= h-6) {
            hud_box(sx-4, sy-1, 8, 2, h, c[0], c[1], c[2], 1);
            hud_box(sx-1, sy-4, 2, 8, h, c[0], c[1], c[2], 1);
        }
    }
    /* Hide the log and DPS panels until their presentation has been revised.
     * Keep world hitmarkers, damage numbers and the telemetry ledger active. */
    if (0) {
        hud_box(l.x-4, l.y-l.row, l.width+8,
                l.row * (l.feed_rows + (combat_meter() ? l.meter_rows + 2 : 0) + 1), h, 0, 0, 0, 0.6f);
        combat_text(l.x, l.y, l.width, h, "COMBAT  Ctrl+M meter  Ctrl+D dump", white);
        for (i = 0; i < (unsigned)l.feed_rows; i++) {
            unsigned back = i + combat_scroll();
            const struct cb_event *e;
            if (back >= s->size) break;
            e = &s->lines[(s->next + CB_KEEP - 1 - back) % CB_KEEP].e;
            slot_colour(e->attacker, c);
            snprintf(line, sizeof(line), "P%02u +%05u T%03u/%08x W%04u %c",
                    e->attacker+1, e->amount, e->kind, e->target, e->weapon, e->flags & CB_KILL ? 'K' : '-');
            combat_text(l.x, l.y + (++row)*l.row, l.width, h, line, c);
        }
        row = l.feed_rows + 1;
        if (combat_meter()) {
            combat_text(l.x, l.y+row++*l.row, l.width, h, "PLAYER  5s/fight DPS  fight/all damage", white);
            for (p = 0; p < CB_PLAYERS; p++)
                if (player_connected(p) || s->total[p]) slots[count++] = p;
            first = count ? (combat_page() % ((count + l.meter_rows - 1) / l.meter_rows)) * l.meter_rows : 0;
            for (p = first; p < count && p < first + l.meter_rows; p++) {
                int slot = slots[p];
                slot_colour(slot, c);
                snprintf(line, sizeof(line), "P%02d %.6s %.1f/%.1f %llu/%llu", slot+1, player_name(slot),
                        cb_rolling(s, slot, now), cb_fight_dps(s, slot, now),
                        (unsigned long long)s->fight[slot], (unsigned long long)s->total[slot]);
                combat_text(l.x, l.y+row++*l.row, l.width, h, line, c);
            }
        }
    }
    hud_font_use(0);
}

/* SwapBuffers hook, before the game's frame is shown */
static void draw_status(int w, int h)
{
    float y = h * 0.70f, bw = w * 0.30f;

    if (caption1[0] && emote_active() >= 0) {
        float cw = (float)(text_width(caption1) > text_width(caption2) ? text_width(caption1) : text_width(caption2));
        box(w * 0.5f - cw * 0.5f - 12, font_h * 0.8f, cw + 24, font_h * 2.4f, h, 0, 0, 0, 0.55f);
        text(w * 0.5f - text_width(caption1) * 0.5f, font_h * 2.0f, h, caption1, 1.0f, 0.85f, 0.3f);
        text(w * 0.5f - text_width(caption2) * 0.5f, font_h * 3.0f, h, caption2, 0.9f, 0.9f, 0.9f);
    }

    if (voice_talking(-1)) {        /* bottom right: the chat line has the bottom left */
        float mw = text_width("MIC") + 16.0f, mx = w - font_h * 0.8f - mw;
        box(mx, h - font_h * 2.4f, mw, font_h + 8, h, 0.0f, 0.0f, 0.0f, 0.5f);
        text(mx + 8.0f, h - font_h * 1.4f, h, "MIC", 0.45f, 1.0f, 0.55f);
    }
    if (!status1[0] && !status2[0]) {
        if (dump_status)
            logmsg("hud status: (none)");
        dump_status = 0;
        return;
    }
    box(w * 0.5f - bw * 0.5f - 12, y - font_h - 6, bw + 24, font_h * (status2[0] ? 2 : 1) + 12 +
        (status_progress >= 0.0f ? 14 : 0), h, 0, 0, 0, 0.45f);
    if (status1[0])
        text(w * 0.5f - text_width(status1) * 0.5f, y, h, status1, 1.0f, 0.85f, 0.3f);
    if (status2[0])
        text(w * 0.5f - text_width(status2) * 0.5f, y + font_h, h, status2, 0.9f, 0.9f, 0.9f);
    if (status_progress >= 0.0f) {
        float by = y + font_h * (status2[0] ? 2 : 1) - font_h + 8;
        box(w * 0.5f - bw * 0.5f, by, bw, 6, h, 0.2f, 0.2f, 0.2f, 0.8f);
        box(w * 0.5f - bw * 0.5f, by, bw * (status_progress > 1.0f ? 1.0f : status_progress), 6, h, 0.3f, 0.9f, 0.4f, 0.9f);
    }
    if (dump_status)
        logmsg("hud status: %s | %s | %.2f", status1, status2, status_progress);
    dump_status = 0;
}

void hud_frame(HDC dc)
{
    int prog = 0, fbo = 0, tex = 0, w, h;
    RECT rc;

    chat_frame(have_remote && shown);
    combat_frame(have_remote && shown);
    /* the panel and tags follow coop_hud's "shown"; the revive status shows whenever it is set */
    if (!guide_on() && !ctrlview_on() && !emote_wheel_open() && !(caption1[0] && emote_active() >= 0) && !banner1[0] && !banner2[0] && !mbox[0][0] && !mbox[1][0] &&
        (!have_remote || (!shown && !status1[0] && !status2[0] && !voice_talking(-1))))
        return;
    if (ready < 0)
        ready = load_gl();
    if (!ready)
        return;
    GetClientRect(WindowFromDC(dc), &rc);
    w = rc.right;
    h = rc.bottom;
    fsel = 0;
    if (w <= 0 || h <= 0 || !make_font(dc, h / 42 < 13 ? 13 : h / 42))
        return;
    if (ctrlview_on() || guide_on() || (shown && have_remote)) {
        fsel = 1;
        make_font(dc, h / 64 < 10 ? 10 : h / 64);
        fsel = 2;
        make_font(dc, h / 26 < 18 ? 18 : h / 26);
        fsel = 3;
        make_font(dc, h / 80 < 9 ? 9 : h / 80);
        fsel = 0;
    }

    if (gl.UseProgram) {
        gl.GetIntegerv(GL_CURRENT_PROGRAM, &prog);
        gl.UseProgram(0);
    }
    if (gl.BindFramebuffer) {
        gl.GetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
        gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    }
    if (gl.ActiveTexture) {
        gl.GetIntegerv(GL_ACTIVE_TEXTURE, &tex);
        gl.ActiveTexture(GL_TEXTURE0);
    }
    gl.PushAttrib(GL_ALL_ATTRIB_BITS);
    gl.DrawBuffer(GL_BACK);
    gl.Viewport(0, 0, w, h);
    gl.Disable(GL_DEPTH_TEST); gl.Disable(GL_CULL_FACE); gl.Disable(GL_TEXTURE_2D); gl.Disable(GL_LIGHTING);
    gl.Disable(GL_ALPHA_TEST); gl.Disable(GL_FOG); gl.Disable(GL_SCISSOR_TEST); gl.Disable(GL_STENCIL_TEST);
    gl.Disable(GL_FRAMEBUFFER_SRGB);
    gl.Enable(GL_BLEND);
    gl.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl.MatrixMode(GL_TEXTURE); gl.PushMatrix(); gl.LoadIdentity();
    gl.MatrixMode(GL_PROJECTION); gl.PushMatrix(); gl.LoadIdentity();
    gl.Ortho(0, w, 0, h, -1, 1);
    gl.MatrixMode(GL_MODELVIEW); gl.PushMatrix(); gl.LoadIdentity();

    if (shown && have_remote) {
        if (outlines)
            draw_outlines(w, h);
        draw_tags(w, h);
        draw_panel(w, h);
        draw_pings(w, h);
        draw_bubbles(w, h);
        draw_chat(w, h);
        if (!ctrlview_on() && !guide_on() && !emote_wheel_open() &&
            !banner1[0] && !banner2[0] && !mbox[0][0] && !status1[0]) draw_combat(w, h);
    }
    draw_emote_wheel(w, h);
    draw_banner(w, h);
    draw_movie_box(w, h);
    draw_status(w, h);
    ctrlview_draw(w, h);
    guide_draw(w, h);
    fsel = 0;
    frames_drawn++;

    gl.MatrixMode(GL_MODELVIEW); gl.PopMatrix();
    gl.MatrixMode(GL_PROJECTION); gl.PopMatrix();
    gl.MatrixMode(GL_TEXTURE); gl.PopMatrix();
    gl.MatrixMode(GL_MODELVIEW);
    gl.PopAttrib();
    if (gl.ActiveTexture)
        gl.ActiveTexture((unsigned)tex);
    if (gl.BindFramebuffer)
        gl.BindFramebuffer(GL_FRAMEBUFFER, (unsigned)fbo);
    if (gl.UseProgram)
        gl.UseProgram((unsigned)prog);
}

/* coop_hud(on, camera, outlines): show the HUD this frame (Lua: in a level, no menu, [hud] show), with
 * the game's camera gob for the name tags and outlines, and whether to outline the players ([hud] outline) */
static void l_coop_hud(void)
{
    unsigned int o = lua_getparam(1);
    shown = o && lua_isnumber(o) && lua_getnumber(o) != 0.0;
    camera = tolua_getusertype(2, NULL);
    o = lua_getparam(3);
    outlines = o && lua_isnumber(o) && lua_getnumber(o) != 0.0;
}

/* a string argument, or "" */
static const char *str_param(int n)
{
    long len;
    return lua_getparam(n) ? luaL_check_lstr(n, &len) : "";
}

/* coop_emote_caption(line1, line2) */
static void l_coop_emote_caption(void)
{
    snprintf(caption1, sizeof(caption1), "%s", str_param(1));
    snprintf(caption2, sizeof(caption2), "%s", str_param(2));
}

/* coop_banner(line1, line2, progress): the cutscene line; no arguments clears it. progress 0..1 draws a bar. */
static void l_coop_banner(void)
{
    unsigned int o = lua_getparam(3);

    snprintf(banner1, sizeof(banner1), "%s", str_param(1));
    snprintf(banner2, sizeof(banner2), "%s", str_param(2));
    banner_progress = o && lua_isnumber(o) ? (float)lua_getnumber(o) : -1.0f;
}

/* coop_movie_box(line1, line2, line3): the cutscene box (line 1 in the banner's colour); no arguments clears it */
static void l_coop_movie_box(void)
{
    int i;

    for (i = 0; i < 3; i++)
        snprintf(mbox[i], sizeof(mbox[i]), "%s", str_param(i + 1));
}

/* coop_hud_dump(): log the panel's rows as the next frame draws them, and the frames drawn so far */
static void l_coop_hud_dump(void)
{
    dump_rows = 1;
    dump_status = 1;
    logmsg("hud: %d frames drawn", frames_drawn);
}

/* coop_hud_tune(fov, head): field of view (vertical, degrees) and tag height, for calibrating */
static void l_coop_hud_tune(void)
{
    unsigned int o = lua_getparam(1), p = lua_getparam(2);
    if (o && lua_isnumber(o))
        fov_deg = (float)lua_getnumber(o);
    if (p && lua_isnumber(p))
        head_z = (float)lua_getnumber(p);
    logmsg("hud: fov %.1f, head %.2f", fov_deg, head_z);
    if (gob_alive(camera)) {
        float *c = (float *)(camera + GOB_POSITION), *q = (float *)(camera + GOB_ORIENTATION);
        logmsg("hud: camera %s at (%.2f, %.2f, %.2f) quat (%.3f, %.3f, %.3f, %.3f)", gob_name(camera), c[0], c[1], c[2],
               q[0], q[1], q[2], q[3]);
    }
}

void hud_lua_init(void)
{
    tolua_function(NULL, "coop_hud", l_coop_hud);
    tolua_function(NULL, "coop_hud_tune", l_coop_hud_tune);
    tolua_function(NULL, "coop_hud_dump", l_coop_hud_dump);
    tolua_function(NULL, "coop_emote_caption", l_coop_emote_caption);
    tolua_function(NULL, "coop_banner", l_coop_banner);
    tolua_function(NULL, "coop_movie_box", l_coop_movie_box);
}
