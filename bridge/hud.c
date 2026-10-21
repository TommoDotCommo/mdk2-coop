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
    void (WINAPI *ListBase)(unsigned);
    void (WINAPI *CallLists)(int, unsigned, const void *);
    void (WINAPI *GetIntegerv)(unsigned, int *);
    const char *(WINAPI *GetString)(unsigned);
    void (WINAPI *DrawBuffer)(unsigned);
    unsigned (WINAPI *GenLists)(int);
    void (WINAPI *DeleteLists)(unsigned, int);
    /* 1.3+ through wglGetProcAddress */
    void (WINAPI *UseProgram)(unsigned);
    void (WINAPI *BindFramebuffer)(unsigned, unsigned);
    void (WINAPI *ActiveTexture)(unsigned);
    BOOL (WINAPI *UseFontBitmapsA)(HDC, DWORD, DWORD, DWORD);    /* wgl, from opengl32.dll */
} gl;

static int ready = -1;          /* -1 not tried, 0 failed, 1 ok */
static int shown;               /* coop_hud(on) from Lua, this frame */
static BYTE *camera;            /* the game's camera gob (mdkGetCameraGob), from coop_hud */
static float fov_deg = 64.0f;   /* vertical field of view: from where puppets at known spots appear (1-2) */
static float head_z = 1.8f;     /* tag this far above a puppet's feet (Kurt is about 1.45 tall) */
static unsigned font_base;
static int font_px, font_h;     /* font built for this text height (pixels) */
static int char_w[128];         /* advance widths, for measuring text */
static HFONT font;

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
    GLX(UseProgram); GLX(BindFramebuffer); GLX(ActiveTexture);
    *(FARPROC *)&gl.UseFontBitmapsA = GetProcAddress(m, "wglUseFontBitmapsA");
#undef GL1
#undef GLX
    if (!gl.PushAttrib || !gl.Rectf || !gl.CallLists || !gl.GenLists || !gl.GetString || !gl.UseFontBitmapsA)
        return 0;
    logmsg("hud: GL %s on %s; program %s, framebuffers %s", gl.GetString(GL_VERSION), gl.GetString(GL_RENDERER),
           gl.UseProgram ? "yes" : "no", gl.BindFramebuffer ? "yes" : "no");
    return 1;
}

/* A bold Windows font as display lists, sized to the window */
static int make_font(HDC dc, int px)
{
    HFONT old;
    ABC abc[96];
    int i;

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

    gl.ListBase(font_base - 32);
    gl.Color4f(0.0f, 0.0f, 0.0f, 0.85f);
    gl.RasterPos2f(x + 1.0f, (float)h - y - 1.0f);
    gl.CallLists(n, GL_UNSIGNED_BYTE, s);
    gl.Color4f(r, g, b, 1.0f);
    gl.RasterPos2f(x, (float)h - y);
    gl.CallLists(n, GL_UNSIGNED_BYTE, s);
}

/* a box from (x, y) top left, y down */
static void box(float x, float y, float w, float bh, int h, float r, float g, float b, float a)
{
    gl.Color4f(r, g, b, a);
    gl.Rectf(x, (float)h - y - bh, x + w, (float)h - y);
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

/* Name tags over the other players' heads, nearest first so overlapping ones stack upwards;
 * the ones off screen at its edge with an arrow */
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
            text(sx - tw * 0.5f, sy, h, name, 0.95f, 0.85f, 0.4f);
            text(dx < 0 ? sx - tw * 0.5f - font_h : sx + tw * 0.5f + 6, sy, h, dx < 0 ? "<" : ">", 0.95f, 0.85f, 0.4f);
        }
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

/* SwapBuffers hook, before the game's frame is shown */
static void draw_status(int w, int h)
{
    float y = h * 0.70f, bw = w * 0.30f;

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

    /* the panel and tags follow coop_hud's "shown"; the revive status shows whenever it is set */
    if (!have_remote || (!shown && !status1[0] && !status2[0]))
        return;
    if (ready < 0)
        ready = load_gl();
    if (!ready)
        return;
    GetClientRect(WindowFromDC(dc), &rc);
    w = rc.right;
    h = rc.bottom;
    if (w <= 0 || h <= 0 || !make_font(dc, h / 42 < 13 ? 13 : h / 42))
        return;

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

    if (shown) {
        draw_tags(w, h);
        draw_panel(w, h);
    }
    draw_status(w, h);
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

/* coop_hud(on, camera): show the HUD this frame (Lua: in a level, no menu, [hud] show), with
 * the game's camera gob for the name tags */
static void l_coop_hud(void)
{
    unsigned int o = lua_getparam(1);
    shown = o && lua_isnumber(o) && lua_getnumber(o) != 0.0;
    camera = tolua_getusertype(2, NULL);
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
}
