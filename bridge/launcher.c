/*
 * Launcher: a screen shown when the game starts, before its window. The player picks host,
 * join or solo and the connection, changes co-op and display settings and reads about the
 * mod, then the game starts with them. It runs on the game's thread inside the game's first
 * CreateWindowExA (input.c), which comes before the game runs settings.lua and with it
 * coop_init: coop.ini and the video line of settings.lua written here apply to this start.
 *
 *   [launcher] show=1|0           0: start straight away (Shift held at start shows it anyway)
 *              mode=host|join|solo  the last choice; solo turns co-op off for that start only
 *                                   (host or join is read back from [net] role)
 *
 * Skipped when Steam starts the game to join a lobby (+connect_lobby) and in background test
 * mode, unless [test] launcher=1: then it runs hidden, driven by coop\launcher.txt (lines
 * "key <vk>", "click <x> <y>" on the 1280x720 canvas, "text <s>", "shot <name>" saving
 * coop\<name>.tga); the file is deleted once read.
 *
 * The Saves page lists the co-op sessions saves.ini knows (saves.c): who played, how many,
 * when, and how far they got, and picks what Host starts: a new game at a level and
 * checkpoint, or a session to continue. The choice goes to coop.ini for coop_dev.lua, which
 * uses it once:  [start] action=new|resume  level= cp=  sid=
 *
 * Art comes from the player's own mdk2.tar (the title screen and the menu font); without it
 * the launcher still works, plainer.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>
#include "bridge.h"
#include <shellapi.h>
#include "launcher_session.h"

#ifndef MOD_VERSION
#define MOD_VERSION "dev"
#endif

#define PROJECT_URL     "https://github.com/TommoDotCommo/mdk2-coop"
#define ISSUES_URL      PROJECT_URL "/issues/new"
#define SETTINGS_MAX    8000    /* see loader.c */

int coop_enabled = 1;           /* 0: the player chose solo, coop_init does nothing */

/* ---- art from mdk2.tar ---- */

static struct image title, font;
static float glyph_adv[256];                /* font.lua dim[col][row]: advance in cells */

/* One member of <game dir>\mdk2.tar (plain ustar), malloc'd, or NULL */
static BYTE *tar_read(const char *name, DWORD *size)
{
    char path[MAX_PATH], hdr[512], field[13];
    __int64 pos = 0;
    BYTE *data = NULL;
    FILE *f;

    snprintf(path, MAX_PATH, "%s\\mdk2.tar", game_dir);
    if (!(f = fopen(path, "rb")))
        return NULL;
    while (fread(hdr, 1, 512, f) == 512 && hdr[0]) {
        unsigned long long n;
        memcpy(field, hdr + 124, 12);
        field[12] = 0;
        n = strtoull(field, NULL, 8);
        hdr[99] = 0;
        if (_stricmp(hdr, name) == 0) {
            if (n < 64 << 20 && (data = malloc(n + 1)) && fread(data, 1, n, f) == n) {
                data[n] = 0;
                *size = (DWORD)n;
            } else {
                free(data);
                data = NULL;
            }
            break;
        }
        pos += 512 + ((n + 511) & ~511ull);
        if (_fseeki64(f, pos, SEEK_SET))
            break;
    }
    fclose(f);
    return data;
}

/* One 4x4 block of a DXT image into out[0..15] (0xAARRGGBB); kind 1, 3 or 5 */
static void dxt_block(const BYTE *b, int kind, DWORD *out)
{
    const BYTE *c = kind == 1 ? b : b + 8;
    unsigned c0 = c[0] | c[1] << 8, c1 = c[2] | c[3] << 8, bits = c[4] | c[5] << 8 | c[6] << 16 | (unsigned)c[7] << 24;
    int rgb[4][3], i, a[8];

    for (i = 0; i < 2; i++) {
        unsigned v = i ? c1 : c0;
        rgb[i][0] = ((v >> 11) & 31) * 255 / 31;
        rgb[i][1] = ((v >> 5) & 63) * 255 / 63;
        rgb[i][2] = (v & 31) * 255 / 31;
    }
    for (i = 0; i < 3; i++) {
        if (c0 > c1 || kind != 1) {
            rgb[2][i] = (2 * rgb[0][i] + rgb[1][i]) / 3;
            rgb[3][i] = (rgb[0][i] + 2 * rgb[1][i]) / 3;
        } else {
            rgb[2][i] = (rgb[0][i] + rgb[1][i]) / 2;
            rgb[3][i] = 0;
        }
    }
    if (kind == 5) {
        unsigned long long ab = 0;

        for (i = 0; i < 6; i++)
            ab |= (unsigned long long)b[2 + i] << (8 * i);
        a[0] = b[0];
        a[1] = b[1];
        for (i = 0; i < 6 && a[0] > a[1]; i++)
            a[2 + i] = ((6 - i) * a[0] + (1 + i) * a[1]) / 7;
        for (i = 0; i < 4 && a[0] <= a[1]; i++)
            a[2 + i] = ((4 - i) * a[0] + (1 + i) * a[1]) / 5;
        if (a[0] <= a[1]) {
            a[6] = 0;
            a[7] = 255;
        }
        for (i = 0; i < 16; i++) {
            int ci = (bits >> (2 * i)) & 3, al = a[(ab >> (3 * i)) & 7];
            out[i] = (DWORD)al << 24 | rgb[ci][0] << 16 | rgb[ci][1] << 8 | rgb[ci][2];
        }
        return;
    }
    for (i = 0; i < 16; i++) {
        int ci = (bits >> (2 * i)) & 3, al = 255;

        if (kind == 3)
            al = ((b[i / 2] >> (4 * (i & 1))) & 15) * 17;
        else if (c0 <= c1 && ci == 3)
            al = 0;
        out[i] = (DWORD)al << 24 | rgb[ci][0] << 16 | rgb[ci][1] << 8 | rgb[ci][2];
    }
}

/* DXT1/3/5 DDS (the Overhaul Games build keeps its menu art compressed); like the 32-bit ones its rows are bottom up */
static int dds_load_dxt(const BYTE *d, DWORD size, struct image *img)
{
    const DWORD *h = (const DWORD *)d;
    int w = h[4], ht = h[3], kind, bx, by, x, y, bs;
    DWORD blk[16];

    if (size < 128 || h[0] != 0x20534444 || !(h[20] & 4) || w <= 0 || ht <= 0 || w > 4096 || ht > 4096 ||
        (w | ht) & 3)
        return 0;
    kind = h[21] == 0x31545844 ? 1 : h[21] == 0x33545844 ? 3 : h[21] == 0x35545844 ? 5 : 0;
    bs = kind == 1 ? 8 : 16;
    if (!kind || 128 + (DWORD)(w / 4) * (ht / 4) * bs > size || !(img->px = malloc(w * ht * 4)))
        return 0;
    for (by = 0; by < ht / 4; by++)
        for (bx = 0; bx < w / 4; bx++) {
            dxt_block(d + 128 + (by * (w / 4) + bx) * bs, kind, blk);
            for (y = 0; y < 4; y++)
                for (x = 0; x < 4; x++)
                    img->px[(ht - 1 - (by * 4 + y)) * w + bx * 4 + x] = blk[y * 4 + x];
        }
    img->w = w;
    img->h = ht;
    return 1;
}

/* Uncompressed 32-bit DDS (the format of the game's menu art); rows are stored bottom up */
static int dds_load(const char *name, struct image *img)
{
    DWORD size, *h;
    BYTE *d = tar_read(name, &size);
    int w, ht, y;

    if (!d)
        return 0;
    h = (DWORD *)d;
    if (dds_load_dxt(d, size, img)) {
        free(d);
        return 1;
    }
    w = h[4];
    ht = h[3];
    if (size < 128 || h[0] != 0x20534444 || !(h[20] & 0x40) || h[22] != 32 || h[23] != 0xff0000 ||
        h[24] != 0xff00 || h[25] != 0xff || w <= 0 || ht <= 0 || w > 4096 || ht > 4096 ||
        128 + (DWORD)w * ht * 4 > size || !(img->px = malloc(w * ht * 4))) {
        logmsg("launcher: %s is not the expected image", name);
        free(d);
        return 0;
    }
    for (y = 0; y < ht; y++)
        memcpy(img->px + y * w, d + 128 + (ht - 1 - y) * w * 4, w * 4);
    img->w = w;
    img->h = ht;
    free(d);
    return 1;
}

/* font.lua: 16 rows of 16 widths, dim[col][row] for the character row * 16 + col; font.dds is the
 * 512x512 sheet of 32x32 cells. Also the HUD's text (hud.c). */
int art_font(struct image *img, float adv[256])
{
    DWORD size;
    char *s = (char *)tar_read("font.lua", &size), *p;
    int k = 0;

    if (!s)
        return 0;
    for (p = strchr(s, '{'); p && *p && k < 256; ) {
        if (isdigit((unsigned char)*p) || *p == '.') {
            adv[(k % 16) * 16 + k / 16] = (float)strtod(p, &p);
            k++;
        } else {
            p++;
        }
    }
    free(s);
    if (k != 256)
        return 0;
    return dds_load("font.dds", img);
}

static int font_load(void)
{
    return art_font(&font, glyph_adv);
}

/* ---- settings ---- */

static char ini[MAX_PATH], player_ini[MAX_PATH], settings_lua[MAX_PATH];

enum { MODE_HOST, MODE_JOIN, MODE_SOLO };
static const char *mode_names[] = { "host", "join", "solo" };

static struct {
    int mode, steam, friendly_fire, difficulty, save_interval, controls, hud, show, show_lobby;
    int section, char_follow, devkeys, outline, movietimer, ghosts, emotes, charwheel, chat, pings, pad;
    int voice, voice_volume, voice_range, voice_mode, voice_in_dev, voice_out_dev;
    char name[PLAYER_NAME], lobby[24], host[64], port[8], listen[8];
} cfg;

/* the switches of Options > Co-op (Gameplay, Interface, Voice chat), as coop.ini keys: section, key, default */
static const struct { const char *section, *key; int dflt; int *v; } switches[] = {
    { "game", "char_follow", 1, &cfg.char_follow }, { "controls", "devkeys", 0, &cfg.devkeys },
    { "hud", "outline", 0, &cfg.outline }, { "hud", "movietimer", 1, &cfg.movietimer },
    { "features", "ghosts", 1, &cfg.ghosts }, { "features", "emotes", 1, &cfg.emotes },
    { "features", "charwheel", 1, &cfg.charwheel }, { "features", "chat", 1, &cfg.chat },
    { "features", "pings", 1, &cfg.pings }, { "pad", "enabled", 1, &cfg.pad }, { "voice", "enabled", 1, &cfg.voice },
};
#define NSWITCHES ((int)(sizeof(switches) / sizeof(switches[0])))
/* voice chat's choices, as coop.voice_volumes / coop.voice_ranges in coop_dev.lua */
static const int voice_volumes[] = { 50, 100, 150, 200 };
static const char *voice_volume_names[] = { "Quiet", "Normal", "Loud", "Louder" };
static const int voice_ranges[] = { 25, 50, 100, 0 };
static const char *voice_mode_names[] = { "Push to talk (B)", "Voice activity" };

#include "launcher_voice_win.h"
static const char *voice_range_names[] = { "Short", "Medium", "Long", "Everywhere" };

/* the game's sound levels, lines of settings.lua (0..1; the game's own options write them the same way) */
static const char *sound_calls[3] = { "chSndSetEffectsVolume", "chSndMusicVolume", "chSndSetVoiceVolume" };
static int sound[3], sound_read[3];          /* 0..10, tenths */

/* The difficulty presets of Options > Co-op > Gameplay > Difficulty (coop.diffpresets in
 * coop_dev.lua: keep the two the same). Values of the [game] keys in diff_keys order; a key
 * that is not in coop.ini counts as Normal's. Custom: the keys match no preset, kept as they are. */
#define NDIFF 7
static const char *diff_keys[NDIFF] = { "friendly_fire", "enemy_hp", "enemy_scale", "dmg_taken", "dmg_dealt",
                                        "down_secs", "revive_secs" };
static const int diff_presets[4][NDIFF] = {
    { 0,  75,   0,  50, 100, 60, 2 },   /* Story */
    { 0, 100,  50, 100, 100, 30, 3 },   /* Normal */
    { 0, 125,  75, 150, 100, 20, 4 },   /* Hard */
    { 1, 150, 100, 200, 100, 10, 5 },   /* Nightmare */
};
static const char *diff_names[] = { "Story", "Normal", "Hard", "Nightmare", "Custom" };
#define DIFF_CUSTOM 4
static int diff[NDIFF], diff_read[NDIFF];      /* the values now, and as coop.ini had them */

/* Every checkpoint of the campaign in play order (tools/checkpoints.txt: the debug menu's list;
 * 3-16 sits between 3-6 and 3-7), for the Saves page's level selector and "how far along" */
static const short checkpoints[][2] = {
    {1,1}, {1,2}, {1,3}, {1,4}, {1,5}, {1,6}, {1,7}, {1,8}, {2,1}, {2,2}, {2,3}, {2,4},
    {2,5}, {2,6}, {2,7}, {2,8}, {2,9}, {2,10}, {3,1}, {3,2}, {3,3}, {3,4}, {3,5}, {3,6},
    {3,16}, {3,7}, {3,8}, {3,9}, {3,10}, {3,11}, {4,1}, {4,2}, {4,3}, {4,4}, {4,5}, {4,6},
    {4,7}, {4,8}, {4,9}, {4,10}, {4,11}, {4,14}, {5,1}, {5,2}, {5,3}, {5,4}, {5,5}, {5,6},
    {5,7}, {5,8}, {5,9}, {5,10}, {6,1}, {6,2}, {6,3}, {6,4}, {6,5}, {6,6}, {6,7}, {7,1},
    {7,2}, {7,3}, {7,4}, {7,5}, {7,6}, {7,7}, {7,8}, {8,1}, {8,2}, {8,3}, {8,4}, {8,5},
    {8,6}, {8,7}, {9,1}, {9,3}, {9,4}, {9,5}, {9,6}, {9,7}, {9,8}, {9,11}, {9,12}, {10,1},
    {10,2}, {10,3}, {10,4}, {10,5}, {10,6}, {10,11}, {11,1}, {11,6}, {11,11}, {12,1}, {12,6},
    {12,11},
};
#define NCP ((int)(sizeof(checkpoints) / sizeof(checkpoints[0])))

/* index into checkpoints[] of level l checkpoint cp, or of the last one before it; -1 if none */
static int cp_index(int l, int cp)
{
    int i, found = -1;

    for (i = 0; i < NCP; i++)
        if (checkpoints[i][0] == l && checkpoints[i][1] == cp)
            return i;
    for (i = 0; i < NCP; i++)
        if (checkpoints[i][0] < l || (checkpoints[i][0] == l && checkpoints[i][1] <= cp))
            found = i;
    return found;
}

/* the first checkpoint of level l in checkpoints[], and how many it has */
static int level_first(int l)
{
    int i;

    for (i = 0; i < NCP; i++)
        if (checkpoints[i][0] == l)
            return i;
    return 0;
}

static int level_count(int l)
{
    int i, n = 0;

    for (i = 0; i < NCP; i++)
        n += checkpoints[i][0] == l;
    return n;
}

#define NLEVELS 12

/* What the player sees of each level: where and when it is set (the loading screen caption in
 * the game's own text, mdk2.str of the player's mdk2.tar, read at run time: nothing of it is
 * kept in this repo) and who plays it. Levels 11 and 12 have no caption: placeholders. */
struct level_info { char place[72], time[16]; const char *hero; int known; };
static struct level_info linfo[NLEVELS + 1];
static int linfo_done;
static const char *level_heroes[NLEVELS + 1] = { "", "Kurt", "Max", "Doc", "Kurt", "Max", "Doc and Hyde", "Kurt",
                                                 "Max", "Doc", "Kurt", "Max", "Doc and Hyde" };

/* "Aboard the Alien Minecrawler, Edmonton: 9:05AM " -> place, "9:05 AM"; 0 if it has no time */
static int caption_split(const char *text, char *place, int psize, char *time, int tsize)
{
    int n = (int)strlen(text), i;

    while (n > 0 && text[n - 1] == ' ')
        n--;
    if (n < 12 || text[n - 1] != 'M' || (text[n - 2] != 'A' && text[n - 2] != 'P'))
        return 0;
    for (i = n - 3; i > 0 && (isdigit((BYTE)text[i - 1]) || text[i - 1] == ':'); i--)
        ;
    if (!isdigit((BYTE)text[i]) || !strchr(text + i, ':') || i < 4)
        return 0;
    snprintf(time, tsize, "%.*s %cM", n - 2 - i, text + i, text[n - 2]);
    n = i;
    while (n > 0 && (text[n - 1] == ' ' || text[n - 1] == ',' || text[n - 1] == ':'))
        n--;
    snprintf(place, psize, "%.*s", n, text);
    return 1;
}

static void level_info_load(void)
{
    DWORD size, i, run = 0;
    BYTE *d;
    char text[160];
    int l, found = 0;

    linfo_done = 1;
    for (l = 1; l <= NLEVELS; l++) {
        linfo[l].hero = level_heroes[l];
        snprintf(linfo[l].place, sizeof(linfo[l].place), l > 10 ? "Location to be named" : "");
        snprintf(linfo[l].time, sizeof(linfo[l].time), "%s", l > 10 ? "time unknown" : "");
    }
    if (!(d = tar_read("mdk2.str", &size)))
        return;
    /* UTF-16 text that is all ASCII; the loading screen captions of levels 1 to 10 come first in
     * the file, in level order (the later one, 8:59AM, is the intro of level 1) */
    for (i = 0; i + 1 < size && found < 10; i += 2) {
        if (d[i] >= 32 && d[i] < 127 && d[i + 1] == 0) {
            if (run < sizeof(text) - 1)
                text[run++] = (char)d[i];
            continue;
        }
        if (run > 10) {
            char place[72], time[16];

            text[run] = 0;
            if (caption_split(text, place, sizeof(place), time, sizeof(time))) {
                found++;
                snprintf(linfo[found].place, sizeof(linfo[found].place), "%s", place);
                snprintf(linfo[found].time, sizeof(linfo[found].time), "%s", time);
                linfo[found].known = 1;
            }
        }
        run = 0;
    }
    free(d);
    if (found == 10)
        logmsg("launcher: level captions read (%d)", found);
    if (found < 10) {
        logmsg("launcher: only %d level captions found in mdk2.str: names not shown", found);
        for (l = 1; l <= 10; l++) {
            linfo[l].place[0] = linfo[l].time[0] = 0;
            linfo[l].known = 0;
        }
    }
}

static const struct level_info *level_info(int l)
{
    if (!linfo_done)
        level_info_load();
    return &linfo[l >= 1 && l <= NLEVELS ? l : 0];
}

/* "Alien Orbiter, Earth Orbit - 11:50 AM - Kurt" (what the game says about level l) */
static void level_line(int l, char *out, int size)
{
    const struct level_info *li = level_info(l);

    out[0] = 0;
    if (!li->hero)
        return;
    if (li->place[0])
        snprintf(out, size, "%s  -  %s  -  %s", li->place, li->time, li->hero);
    else
        snprintf(out, size, "%s", li->hero);
}

/* the level's checkpoints in play order: the 1-based number of cp in level l, and how many it has;
 * a checkpoint that is not in the list counts as the last one before it (0 if none) */
static int cp_ordinal(int l, int cp, int *of)
{
    int i = cp_index(l, cp);

    *of = level_count(l);
    if (i < 0 || checkpoints[i][0] != l)
        return 0;
    return i - level_first(l) + 1;
}

/* for coop_level_info (saves.c) */
void level_info_get(int l, const char **place, const char **time, const char **hero)
{
    const struct level_info *li = level_info(l);

    *place = li->place;
    *time = li->time;
    *hero = li->hero ? li->hero : "";
}

/* "Checkpoint 7 of 12" for raw checkpoint number cp of level l */
static void cp_text_of(int l, int cp, char *out, int size)
{
    int of, k = cp_ordinal(l, cp, &of);

    if (k)
        snprintf(out, size, "Checkpoint %d of %d", k, of);
    else
        snprintf(out, size, "Checkpoint %d", cp);
}

/* The Saves page: the sessions saves.ini has (newest first), what Host starts (sel: a session,
 * or -1 for a new game at start_level / start_cp, which is only started when start_set) */
static struct coop_session sessions[12];
static int nsessions, sel = -1, saves_top, delete_ask = -1;
static int start_level = 1, start_cp, start_set;
static unsigned int own_id;

/* Controls page presets ([controls] preset) */
static const char *ctrl_ids[] = { "original", "modern", "custom" };
static const char *ctrl_names[] = { "Original", "Modern", "Custom" };
static const int intervals[] = { 60, 120, 300, 600 };
static const char *interval_text[] = { "1 min", "2 min", "5 min", "10 min" };

/* the video line of settings.lua: chSetVideoMode('<s>', a, b, width, height, hz, fullscreen) */
static struct { int have, fullscreen, w, h, hz; char head[64]; } video;
static struct { int w, h; } res[64];
static int nres, res_index, video_dirty;

static int index_of(const int *t, int n, int v, int dflt)
{
    int i;
    for (i = 0; i < n; i++)
        if (t[i] == v)
            return i;
    return dflt;
}

static void put(const char *section, const char *key, const char *value)
{
    WritePrivateProfileStringA(section, key, value, ini);
}

static void put_int(const char *section, const char *key, int v)
{
    char s[16];
    snprintf(s, sizeof(s), "%d", v);
    put(section, key, s);
}

static int read_file(const char *path, char *buf, int size)
{
    FILE *f = fopen(path, "rb");
    int n;
    if (!f)
        return -1;
    n = (int)fread(buf, 1, size - 1, f);
    fclose(f);
    buf[n] = 0;
    return n;
}

static void video_load(void)
{
    char buf[SETTINGS_MAX + 1], *p, *q;
    long long a;
    int b, i, j;
    DEVMODEA dm;

    memset(&dm, 0, sizeof(dm));
    dm.dmSize = sizeof(dm);
    for (i = 0; nres < 64 && EnumDisplaySettingsA(NULL, i, &dm); i++) {
        if (dm.dmBitsPerPel != 32 || dm.dmPelsWidth < 640 || dm.dmPelsHeight < 480)
            continue;
        for (j = 0; j < nres && (res[j].w != (int)dm.dmPelsWidth || res[j].h != (int)dm.dmPelsHeight); j++)
            ;
        if (j < nres)
            continue;
        for (j = nres++; j > 0 && (res[j - 1].w > (int)dm.dmPelsWidth ||
             (res[j - 1].w == (int)dm.dmPelsWidth && res[j - 1].h > (int)dm.dmPelsHeight)); j--)
            res[j] = res[j - 1];
        res[j].w = dm.dmPelsWidth;
        res[j].h = dm.dmPelsHeight;
    }
    if (read_file(settings_lua, buf, sizeof(buf)) < 0 || !(p = strstr(buf, "chSetVideoMode(")) ||
        !(q = strchr(p + 15, ',')) || q - p >= (int)sizeof(video.head) - 1 ||
        sscanf(q + 1, " %lld , %d , %d , %d , %d , %d", &a, &b, &video.w, &video.h, &video.hz, &video.fullscreen) != 6) {
        video.have = 0;
        video.fullscreen = 1;
        return;
    }
    /* keep the game's own first three arguments as they are */
    snprintf(video.head, sizeof(video.head), "%.*s, %lld, %d", (int)(q - p), p, a, b);
    video.have = 1;
    for (i = 0; i < nres; i++)
        if (res[i].w == video.w && res[i].h == video.h)
            res_index = i;
}

/* Rewrite (or add) the video line; the loader block and everything else stay */
static void video_save(void)
{
    char buf[SETTINGS_MAX + 1], out[SETTINGS_MAX + 256], line[160], *p, *e;
    int n = read_file(settings_lua, buf, sizeof(buf)), w = res[res_index].w, h = res[res_index].h;
    DEVMODEA dm;
    FILE *f;

    if (n < 0 || !nres)
        return;
    memset(&dm, 0, sizeof(dm));
    dm.dmSize = sizeof(dm);
    if (!video.have) {
        EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm);
        snprintf(video.head, sizeof(video.head), "chSetVideoMode(''%s", ", 0, 0");
        video.hz = dm.dmDisplayFrequency ? (int)dm.dmDisplayFrequency : -1;
        video.have = 1;
    }
    snprintf(line, sizeof(line), "%s, %d, %d, %d, %d)", video.head, w, h, video.hz, video.fullscreen);
    if ((p = strstr(buf, "chSetVideoMode("))) {
        e = strchr(p, ')');
        e = e ? e + 1 : p + strlen(p);
        snprintf(out, sizeof(out), "%.*s%s%s", (int)(p - buf), buf, line, e);
    } else {
        snprintf(out, sizeof(out), "%s\r\n%s", line, buf);
    }
    if (strlen(out) > SETTINGS_MAX || !(f = fopen(settings_lua, "wb"))) {
        logmsg("launcher: cannot write the video line to settings.lua");
        return;
    }
    fputs(out, f);
    fclose(f);
    logmsg("launcher: video %s", line);
}

/* the sound levels from settings.lua; a missing line counts as the game's default (0.25) */
static void sound_load(void)
{
    char buf[SETTINGS_MAX + 1], *p;
    float v;
    int i;

    for (i = 0; i < 3; i++)
        sound[i] = 3;
    if (read_file(settings_lua, buf, sizeof(buf)) >= 0)
        for (i = 0; i < 3; i++)
            if ((p = strstr(buf, sound_calls[i])) && sscanf(p + strlen(sound_calls[i]), " ( %f", &v) == 1)
                sound[i] = (int)(v * 10.0f + 0.5f) < 0 ? 0 : (int)(v * 10.0f + 0.5f) > 10 ? 10 : (int)(v * 10.0f + 0.5f);
    memcpy(sound_read, sound, sizeof(sound));
}

/* Rewrite (or add) the sound lines that changed; everything else in settings.lua stays */
static void sound_save(void)
{
    char buf[SETTINGS_MAX + 1], out[SETTINGS_MAX + 256], line[64], *p, *e;
    int i;
    FILE *f;

    if (!memcmp(sound, sound_read, sizeof(sound)) || read_file(settings_lua, buf, sizeof(buf)) < 0)
        return;
    for (i = 0; i < 3; i++) {
        if (sound[i] == sound_read[i])
            continue;
        snprintf(line, sizeof(line), "%s(%.6f)", sound_calls[i], sound[i] / 10.0);
        if ((p = strstr(buf, sound_calls[i]))) {
            e = strchr(p, ')');
            e = e ? e + 1 : p + strlen(p);
            snprintf(out, sizeof(out), "%.*s%s%s", (int)(p - buf), buf, line, e);
        } else {
            snprintf(out, sizeof(out), "%s\r\n%s", line, buf);
        }
        if (strlen(out) > SETTINGS_MAX) {
            logmsg("launcher: settings.lua too long for the sound lines");
            return;
        }
        strcpy(buf, out);
        logmsg("launcher: sound %s", line);
    }
    if (!(f = fopen(settings_lua, "wb"))) {
        logmsg("launcher: cannot write the sound lines to settings.lua");
        return;
    }
    fputs(buf, f);
    fclose(f);
}

/* which preset the difficulty values are (cfg.difficulty), and friendly fire's toggle from them */
static void diff_match(void)
{
    int p, i;

    for (p = 0; p < 4; p++) {
        for (i = 0; i < NDIFF && diff[i] == diff_presets[p][i]; i++)
            ;
        if (i == NDIFF)
            break;
    }
    cfg.difficulty = p;         /* 4: DIFF_CUSTOM */
    cfg.friendly_fire = diff[0] != 0;
}

static void sound_load(void);

static void settings_read(void)
{
    char s[16];
    int i;

    snprintf(ini, MAX_PATH, "%s\\coop.ini", coop_dir);
    snprintf(player_ini, MAX_PATH, "%s\\player.ini", coop_dir);
    GetEnvironmentVariableA("LOCALAPPDATA", settings_lua, MAX_PATH - 32);
    strcat(settings_lua, "\\MDK2HD\\settings.lua");

    /* host or join follows [net] role, which the installer and Options > Co-op change too */
    GetPrivateProfileStringA("launcher", "mode", "", s, sizeof(s), ini);
    if (!_stricmp(s, "solo"))
        cfg.mode = MODE_SOLO;
    else {
        GetPrivateProfileStringA("net", "role", "host", s, sizeof(s), ini);
        cfg.mode = !_stricmp(s, "client") ? MODE_JOIN : MODE_HOST;
    }
    GetPrivateProfileStringA("net", "transport", "udp", s, sizeof(s), ini);
    cfg.steam = !_stricmp(s, "steam");
    GetPrivateProfileStringA("net", "lobby_id", "", cfg.lobby, sizeof(cfg.lobby), ini);
    GetPrivateProfileStringA("net", "peer_host", "", cfg.host, sizeof(cfg.host), ini);
    GetPrivateProfileStringA("net", "peer_port", "27900", cfg.port, sizeof(cfg.port), ini);
    GetPrivateProfileStringA("net", "listen_port", "27900", cfg.listen, sizeof(cfg.listen), ini);
    GetPrivateProfileStringA("player", "name", "", cfg.name, sizeof(cfg.name), player_ini);
    for (i = 0; i < NDIFF; i++)
        diff[i] = diff_read[i] = GetPrivateProfileIntA("game", diff_keys[i], diff_presets[1][i], ini);
    diff_match();
    cfg.save_interval = index_of(intervals, 4, GetPrivateProfileIntA("save", "interval", 120, ini), 1);
    /* as coop_dev.lua reads it: before presets there was [controls] modern (default on) */
    GetPrivateProfileStringA("controls", "preset", "", s, sizeof(s), ini);
    for (cfg.controls = 0; cfg.controls < 3 && _stricmp(s, ctrl_ids[cfg.controls]); cfg.controls++)
        ;
    if (cfg.controls == 3)
        cfg.controls = GetPrivateProfileIntA("controls", "modern", 1, ini) ? 1 : 0;
    cfg.hud = GetPrivateProfileIntA("hud", "show", 1, ini) != 0;
    for (i = 0; i < NSWITCHES; i++)
        *switches[i].v = GetPrivateProfileIntA(switches[i].section, switches[i].key, switches[i].dflt, ini) != 0;
    cfg.voice_volume = index_of(voice_volumes, 4, GetPrivateProfileIntA("voice", "volume", 100, ini), 1);
    cfg.voice_range = index_of(voice_ranges, 4, GetPrivateProfileIntA("voice", "range", 50, ini), 1);
    lv_enum_devices();
    cfg.voice_mode = GetPrivateProfileIntA("voice", "mode", 0, ini) != 0;
    cfg.voice_in_dev = GetPrivateProfileIntA("voice", "in_dev", -1, ini) + 1;
    if (cfg.voice_in_dev < 0 || cfg.voice_in_dev >= lv_in_count) cfg.voice_in_dev = 0;
    cfg.voice_out_dev = GetPrivateProfileIntA("voice", "out_dev", -1, ini) + 1;
    if (cfg.voice_out_dev < 0 || cfg.voice_out_dev >= lv_out_count) cfg.voice_out_dev = 0;
    cfg.show = GetPrivateProfileIntA("launcher", "show", 1, ini) != 0;
    cfg.show_lobby = GetPrivateProfileIntA("launcher", "lobby", 1, ini) != 0;
    GetPrivateProfileStringA("player", "id", "", s, sizeof(s), player_ini);
    own_id = strtoul(s, NULL, 16);
    nsessions = sessions_list(sessions, 12, 0);
    video_load();
    sound_load();
}

/* What the chosen mode means for coop.ini; written when the game starts */
static void settings_apply(void)
{
    put("launcher", "mode", mode_names[cfg.mode]);
    /* what the host starts (coop_dev.lua uses it once; cleared otherwise so an old one never repeats) */
    put("start", "action", "");
    if (cfg.mode == MODE_HOST && sel >= 0 && sel < nsessions && sessions[sel].host == own_id) {
        put("start", "action", "resume");
        put_int("start", "sid", sessions[sel].sid);
    } else if (cfg.mode == MODE_HOST && start_set) {
        put("start", "action", "new");
        put_int("start", "level", start_level);
        put_int("start", "cp", checkpoints[level_first(start_level) + start_cp][1]);
    }
    coop_enabled = cfg.mode != MODE_SOLO;
    if (!coop_enabled)
        return;
    put("net", "role", cfg.mode == MODE_JOIN ? "client" : "host");
    put("net", "transport", cfg.steam ? "steam" : "udp");
    if (cfg.steam) {
        put("net", "lobby_id", cfg.mode == MODE_JOIN ? cfg.lobby : "");
    } else if (cfg.mode == MODE_JOIN) {
        put("net", "peer_host", cfg.host);
        put("net", "peer_port", cfg.port);
    } else {
        put("net", "listen_port", cfg.listen);
    }
    if (cfg.name[0])
        WritePrivateProfileStringA("player", "name", cfg.name, player_ini);
}

/* ---- drawing: a 32-bit DIB in logical 1280x720 units scaled by sc ---- */

#define LW 1280
#define LH 720

#define C_TEXT      0xe9f0f2
#define C_DIM       0x93a7ae
#define C_FAINT     0x5d7179
#define C_ORANGE    0xff8f1f
#define C_BLUE      0x45c9ff
#define C_PANEL     0x0a1317
#define C_TILE      0x15232a
#define C_TILE_HOT  0x1d323c
#define C_BAD       0xff5a4a

static HDC mem_dc;
static HBITMAP dib, dib_old;
static DWORD *fb, *bg;
static int fw, fh;
static float sc;
static HFONT f_body, f_small, f_label, f_big, f_tiny, f_tiny_b;
static char face[32];
static int guide_open;

static int px(float v) { return (int)(v * sc + 0.5f); }

static DWORD mix(DWORD d, DWORD c, int a)
{
    int r = ((d >> 16 & 255) * (255 - a) + (c >> 16 & 255) * a) / 255;
    int g = ((d >> 8 & 255) * (255 - a) + (c >> 8 & 255) * a) / 255;
    int b = ((d & 255) * (255 - a) + (c & 255) * a) / 255;
    return (DWORD)(r << 16 | g << 8 | b);
}

static DWORD scale_rgb(DWORD c, float k)
{
    int r = (int)((c >> 16 & 255) * k), g = (int)((c >> 8 & 255) * k), b = (int)((c & 255) * k);
    return (DWORD)((r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b));
}

static void fill(float x, float y, float w, float h, DWORD c, int a)
{
    int x0 = px(x), y0 = px(y), x1 = px(x + w), y1 = px(y + h), i, j;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > fw) x1 = fw;
    if (y1 > fh) y1 = fh;
    for (j = y0; j < y1; j++)
        for (i = x0; i < x1; i++)
            fb[j * fw + i] = a >= 255 ? c : mix(fb[j * fw + i], c, a);
}

static void frame_rect(float x, float y, float w, float h, float t, DWORD c, int a)
{
    fill(x, y, w, t, c, a);
    fill(x, y + h - t, w, t, c, a);
    fill(x, y + t, t, h - 2 * t, c, a);
    fill(x + w - t, y + t, t, h - 2 * t, c, a);
}

/* bilinear sample, x and y in image pixels */
static DWORD sample(const struct image *img, float x, float y)
{
    int x0, y0, x1, y1, k;
    float fx, fy;
    DWORD p[4], out = 0;

    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > img->w - 1) x = (float)(img->w - 1);
    if (y > img->h - 1) y = (float)(img->h - 1);
    x0 = (int)x; y0 = (int)y;
    x1 = x0 + 1 < img->w ? x0 + 1 : x0;
    y1 = y0 + 1 < img->h ? y0 + 1 : y0;
    fx = x - x0; fy = y - y0;
    p[0] = img->px[y0 * img->w + x0]; p[1] = img->px[y0 * img->w + x1];
    p[2] = img->px[y1 * img->w + x0]; p[3] = img->px[y1 * img->w + x1];
    for (k = 0; k < 32; k += 8) {
        float top = (p[0] >> k & 255) * (1 - fx) + (p[1] >> k & 255) * fx;
        float bot = (p[2] >> k & 255) * (1 - fx) + (p[3] >> k & 255) * fx;
        out |= (DWORD)(top * (1 - fy) + bot * fy + 0.5f) << k;
    }
    return out;
}

/* The title art, with the logo's centre (512, 324 in the image) at logical (360, 316),
 * vignetted, fading into the dark panel on the right. Drawn once into bg. */
static void draw_background(void)
{
    int i, j;
    for (j = 0; j < fh; j++) {
        float ly = j / sc;
        for (i = 0; i < fw; i++) {
            float lx = i / sc, dx = (lx - 360) / 560, dy = (ly - 316) / 520, v, panel;
            DWORD c = 0x081014;
            if (title.px) {
                float sx = lx - 360 + 512, sy = ly - 316 + 324;
                if (sx >= 0 && sy >= 0 && sx < title.w && sy < title.h)
                    c = sample(&title, sx, sy) & 0xffffff;
            }
            v = 1.0f - 0.75f * (dx * dx + dy * dy);
            c = scale_rgb(c, v < 0.25f ? 0.25f : v);
            panel = (lx - 600) / 150;
            if (panel > 0)
                c = mix(c, C_PANEL, panel >= 1 ? 238 : (int)(238 * panel));
            if (ly > 560 && lx < 760)       /* under the left caption */
                c = mix(c, 0x05090b, (int)((ly - 560) / 160 * 170));
            bg[j * fw + i] = c;
        }
    }
}

/* The game's menu font, cell 32x32, h logical pixels high */
static float title_width(const char *s, float h)
{
    float w = 0;
    for (; *s; s++)
        w += glyph_adv[(BYTE)toupper((BYTE)*s)] * h + h * 0.06f;
    return w;
}

static void title_glyphs(float x, float y, float h, const char *s, DWORD tint, int alpha)
{
    float k = 32.0f / (h * sc);         /* image pixels per screen pixel */
    int cell = px(h);

    for (; *s; s++) {
        int c = (BYTE)toupper((BYTE)*s), cx = (c % 16) * 32, cy = (c / 16) * 32, i, j;
        int x0 = px(x), y0 = px(y), wide = px((glyph_adv[c] + 0.12f) * h);
        if (wide > cell)
            wide = cell;
        for (j = 0; j < cell; j++) {
            if (y0 + j < 0 || y0 + j >= fh)
                continue;
            for (i = 0; i < wide; i++) {
                DWORD p, *d;
                int a;
                float sx = i * k, sy = j * k;
                if (x0 + i < 0 || x0 + i >= fw)
                    continue;
                /* stay inside the cell: the neighbours must not bleed in */
                p = sample(&font, cx + (sx > 30.99f ? 30.99f : sx), cy + (sy > 30.99f ? 30.99f : sy));
                if ((a = (int)(p >> 24) * alpha / 255) < 4)
                    continue;
                d = &fb[(y0 + j) * fw + x0 + i];
                *d = mix(*d, tint == 0 ? 0 : (DWORD)(((p >> 16 & 255) * (tint >> 16 & 255) / 255) << 16 |
                                                     ((p >> 8 & 255) * (tint >> 8 & 255) / 255) << 8 |
                                                     ((p & 255) * (tint & 255) / 255)), a);
            }
        }
        x += glyph_adv[c] * h + h * 0.06f;
    }
}

static void gdi_text(HFONT f, float x, float y, DWORD c, const char *s, UINT align);

/* heading in the game's font (with a shadow), or bold GDI text without the art */
static void heading(float x, float y, float h, const char *s, DWORD tint, UINT align)
{
    if (!font.px) {
        gdi_text(h > 30 ? f_big : f_label, x, y, tint, s, align);
        return;
    }
    if (align == DT_CENTER)
        x -= title_width(s, h) / 2;
    else if (align == DT_RIGHT)
        x -= title_width(s, h);
    title_glyphs(x + 2, y + 2, h, s, 0, 170);
    title_glyphs(x, y, h, s, tint, 255);
}

static HFONT make_font(int h, int weight, const char *name)
{
    return CreateFontA(-px((float)h), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, name);
}

static void fonts_make(void)
{
    HFONT probe = make_font(16, FW_NORMAL, "Bahnschrift");
    HGDIOBJ old = SelectObject(mem_dc, probe);
    GetTextFaceA(mem_dc, sizeof(face), face);
    SelectObject(mem_dc, old);
    DeleteObject(probe);
    if (_stricmp(face, "Bahnschrift"))
        strcpy(face, "Segoe UI");
    f_body = make_font(19, FW_NORMAL, face);
    f_small = make_font(16, FW_NORMAL, face);
    f_label = make_font(19, FW_SEMIBOLD, face);
    f_big = make_font(34, FW_BOLD, face);
    f_tiny = make_font(13, FW_NORMAL, face);
    f_tiny_b = make_font(13, FW_SEMIBOLD, face);
}

static void gdi_text(HFONT f, float x, float y, DWORD c, const char *s, UINT align)
{
    RECT r = { px(x), px(y), px(x), px(y) };
    SelectObject(mem_dc, f);
    SetTextColor(mem_dc, RGB(c >> 16 & 255, c >> 8 & 255, c & 255));
    if (align == DT_CENTER) { r.left -= 2000; r.right += 2000; }
    else if (align == DT_RIGHT) r.left -= 4000;
    else r.right += 4000;
    DrawTextA(mem_dc, s, -1, &r, align | DT_NOCLIP | DT_SINGLELINE | DT_NOPREFIX);
}

/* wrapped text in a column w wide; returns the height used */
static float gdi_para(HFONT f, float x, float y, float w, DWORD c, const char *s)
{
    RECT r = { px(x), px(y), px(x + w), px(y) };
    SelectObject(mem_dc, f);
    SetTextColor(mem_dc, RGB(c >> 16 & 255, c >> 8 & 255, c & 255));
    DrawTextA(mem_dc, s, -1, &r, DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
    DrawTextA(mem_dc, s, -1, &r, DT_WORDBREAK | DT_NOPREFIX);
    return (r.bottom - r.top) / sc;
}

static float gdi_width(HFONT f, const char *s)
{
    SIZE z;
    SelectObject(mem_dc, f);
    GetTextExtentPoint32A(mem_dc, s, (int)strlen(s), &z);
    return z.cx / sc;
}

/* ---- immediate-mode widgets ---- */

enum { PAGE_PLAY, PAGE_SAVES, PAGE_SETTINGS, PAGE_ABOUT, PAGE_HELP, PAGE_PROFILES };
static int page;

static int mouse_x = -1, mouse_y = -1;      /* logical */
static int clicked, activate, adjust;       /* this pass: mouse released, Enter, Left/Right */
static int focus = -1, kb_focus = 1;     /* rings only while the keyboard is in use */
static int ids[128], nids;                  /* focusable widgets of the last pass, in order */
static struct { float x, y, w, h; } hits[128];
static int nhits;
static char typed[64];                      /* characters for the focused field ('\b' backspace) */
static int ntyped;
static int field_focused;       /* the last render drew the focused control as a text field: Space types */
static int changed, done, quit, caret_on = 1;
static int lob_wants(void);
static void lob_open(void);
static int profile_session_active(void);

#define ID __LINE__

static int over(float x, float y, float w, float h)
{
    return mouse_x >= x && mouse_x < x + w && mouse_y >= y && mouse_y < y + h;
}

/* a focusable control: returns 1 when it is clicked or Enter is pressed on it */
static int control(int id, float x, float y, float w, float h)
{
    int hit = over(x, y, w, h);
    if (nids < 128)
        ids[nids++] = id;
    if (nhits < 128) {
        hits[nhits].x = x; hits[nhits].y = y; hits[nhits].w = w; hits[nhits].h = h;
        nhits++;
    }
    if (clicked && hit) {
        focus = id;
        return 1;
    }
    return activate && focus == id;
}

static void focus_ring(int id, float x, float y, float w, float h)
{
    if (focus == id && kb_focus)
        frame_rect(x - 3, y - 3, w + 6, h + 6, 2, C_BLUE, 200);
}

static int tab(int id, float x, float y, const char *s, int on)
{
    float w = font.px ? title_width(s, 26) : gdi_width(f_label, s);
    int hit = control(id, x - 8, y - 6, w + 16, 44);
    heading(x, y, 26, s, on ? C_ORANGE : over(x - 8, y - 6, w + 16, 44) ? C_TEXT : C_DIM, DT_LEFT);
    if (on)
        fill(x, y + 34, w, 3, C_ORANGE, 255);
    focus_ring(id, x - 8, y - 6, w + 16, 44);
    return hit;
}

static int button(int id, float x, float y, float w, float h, const char *s, int primary)
{
    int hit = control(id, x, y, w, h), hot = over(x, y, w, h);
    if (primary) {
        fill(x, y, w, h, hot ? 0xffa640 : C_ORANGE, 255);
        fill(x, y + h - 4, w, 4, 0xc0600c, 255);
        if (font.px)
            title_glyphs(x + (w - title_width(s, 34)) / 2, y + (h - 34) / 2 + 3, 34, s, 0x1c0d00, 255);
        else
            gdi_text(f_big, x + w / 2, y + (h - 40) / 2, 0x1c0d00, s, DT_CENTER);
    } else {
        fill(x, y, w, h, hot ? C_TILE_HOT : C_TILE, 235);
        frame_rect(x, y, w, h, 1, C_FAINT, 255);
        gdi_text(f_label, x + w / 2, y + (h - 24) / 2, C_TEXT, s, DT_CENTER);
    }
    focus_ring(id, x, y, w, h);
    return hit;
}

static int tile(int id, float x, float y, float w, float h, const char *name, const char *line, int on)
{
    int hit = control(id, x, y, w, h), hot = over(x, y, w, h);
    fill(x, y, w, h, on ? 0x2a2014 : hot ? C_TILE_HOT : C_TILE, 235);
    fill(x, y, 5, h, on ? C_ORANGE : C_FAINT, 255);
    if (on)
        frame_rect(x, y, w, h, 2, C_ORANGE, 255);
    heading(x + 24, y + 12, 26, name, on ? C_ORANGE : C_TEXT, DT_LEFT);
    gdi_text(f_small, x + 24, y + 44, on ? C_TEXT : C_DIM, line, DT_LEFT);
    focus_ring(id, x, y, w, h);
    return hit;
}

#define ROW 46      /* a settings row */

/* label on the left, value on the right of a row */
static void row_label(float x, float y, float w, const char *label, const char *hint)
{
    gdi_text(f_body, x, y + (hint ? 2 : 11), C_TEXT, label, DT_LEFT);
    if (hint)
        gdi_text(f_small, x, y + 24, C_FAINT, hint, DT_LEFT);
    fill(x, y + ROW - 1, w, 1, 0x22343c, 255);
}

static int toggle(int id, float x, float y, float w, const char *label, const char *hint, int *v)
{
    float sx = x + w - 66, sy = y + 10;
    int hit = control(id, x, y, w, ROW);
    row_label(x, y, w - 80, label, hint);
    if (hit || (adjust && focus == id)) {
        *v = adjust ? adjust > 0 : !*v;
        changed = 1;
    }
    fill(sx, sy, 60, 26, *v ? C_ORANGE : 0x2a3a42, 255);
    fill(*v ? sx + 36 : sx + 4, sy + 4, 20, 18, *v ? 0x2a1400 : C_DIM, 255);
    gdi_text(f_small, sx - 10, sy + 3, *v ? C_TEXT : C_FAINT, *v ? "On" : "Off", DT_RIGHT);
    focus_ring(id, x, y, w, ROW);
    return hit;
}

static int choice(int id, float x, float y, float w, const char *label, const char *hint,
                  const char **names, int n, int *v)
{
    float bx = x + w - 200;
    int hit = control(id, x, y, w, ROW), d = 0;
    row_label(x, y, w - 210, label, hint);
    if (focus == id && adjust)
        d = adjust;
    else if (hit)
        d = (clicked && mouse_x < bx + 40) ? -1 : 1;
    if (d) {
        *v = (*v + d + n) % n;
        changed = 1;
    }
    fill(bx, y + 6, 200, 34, C_TILE, 235);
    gdi_text(f_label, bx + 14, y + 10, C_DIM, "<", DT_LEFT);
    gdi_text(f_label, bx + 186, y + 10, C_DIM, ">", DT_RIGHT);
    gdi_text(f_body, bx + 100, y + 11, C_TEXT, names[*v], DT_CENTER);
    focus_ring(id, x, y, w, ROW);
    return d != 0;
}

/* a one-line text field; digits limits it to a number */
static int field(int id, float x, float y, float w, const char *label, char *buf, int size,
                 int digits, const char *placeholder)
{
    int edited = 0, i;
    if (focus == id)
        field_focused = 1;
    if (control(id, x, y + 24, w, 40) && !clicked && !profile_session_active() && page == PAGE_PLAY) {
        if (lob_wants()) lob_open();
        else done = 1;               /* Enter in a Play field starts the game or lobby */
    }
    gdi_text(f_small, x, y, C_DIM, label, DT_LEFT);
    if (focus == id) {
        for (i = 0; i < ntyped; i++) {
            int n = (int)strlen(buf);
            char c = typed[i];
            if (c == '\b') {
                if (n)
                    buf[n - 1] = 0;
            } else if (n < size - 1 && c >= 32 && c < 127 && (!digits || isdigit((BYTE)c)) &&
                       !(c == ' ' && digits)) {
                buf[n] = c;
                buf[n + 1] = 0;
            } else {
                continue;
            }
            edited = 1;
        }
        ntyped = 0;
    }
    fill(x, y + 24, w, 40, focus == id ? 0x0d1a20 : C_TILE, 245);
    fill(x, y + 62, w, 2, focus == id ? C_BLUE : C_FAINT, 255);
    if (buf[0])
        gdi_text(f_body, x + 12, y + 33, C_TEXT, buf, DT_LEFT);
    else if (placeholder)
        gdi_text(f_body, x + 12, y + 33, C_FAINT, placeholder, DT_LEFT);
    if (focus == id && caret_on)
        fill(x + 13 + (buf[0] ? gdi_width(f_body, buf) : 0), y + 33, 2, 24, C_BLUE, 255);
    if (edited)
        changed = 1;
    return edited;
}

static int link(int id, float x, float y, const char *s)
{
    float w = gdi_width(f_body, s);
    int hit = control(id, x, y, w, 26), hot = over(x, y, w, 26);
    gdi_text(f_body, x, y, hot ? C_TEXT : C_BLUE, s, DT_LEFT);
    fill(x, y + 24, w, 1, hot ? C_TEXT : C_BLUE, 160);
    focus_ring(id, x, y, w, 26);
    return hit;
}

static void open_url(const char *url)
{
    ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);
}

/* ---- pages ---- */

#define PX 764      /* the panel's content column */
#define PW 476

/* This PC's address on the network that leads to the internet: the local end of a UDP
 * socket "connected" to a public address (connecting a UDP socket sends nothing) */
static void lan_address(char *out, int size)
{
    struct sockaddr_in to, me;
    int len = sizeof(me);
    WSADATA wsa;
    SOCKET s;

    snprintf(out, size, "unknown");
    if (WSAStartup(MAKEWORD(2, 2), &wsa))
        return;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(53);
    to.sin_addr.s_addr = inet_addr("8.8.8.8");
    if ((s = socket(AF_INET, SOCK_DGRAM, 0)) != INVALID_SOCKET) {
        if (connect(s, (struct sockaddr *)&to, sizeof(to)) == 0 &&
            getsockname(s, (struct sockaddr *)&me, &len) == 0 && me.sin_addr.s_addr)
            snprintf(out, size, "%s", inet_ntoa(me.sin_addr));
        closesocket(s);
    }
    WSACleanup();
}

static void profile_load(void);

static void page_play(void)
{
    static const char *conn[] = { "Direct UDP", "Steam" };
    static char lan[32];
    float y = 112;
    char line[160];

    heading(PX, y, 20, "Your name", C_DIM, DT_LEFT);
    field(ID, PX + 150, y - 30, PW - 150, "", cfg.name, sizeof(cfg.name), 0, "Player");
    y += 50;
    if (tile(ID, PX, y, PW, 74, "Host a game", "Run the world. Friends join you.", cfg.mode == MODE_HOST))
        cfg.mode = MODE_HOST, changed = 1;
    y += 84;
    if (tile(ID, PX, y, PW, 74, "Join a friend", "Follow a host into their game.", cfg.mode == MODE_JOIN))
        cfg.mode = MODE_JOIN, changed = 1;
    y += 84;
    if (tile(ID, PX, y, PW, 74, "Play solo", "Plain MDK2 HD. Co-op is off for this start.", cfg.mode == MODE_SOLO))
        cfg.mode = MODE_SOLO, changed = 1;
    y += 92;

    if (cfg.mode != MODE_SOLO) {
        choice(ID, PX, y, PW, "Connection", cfg.steam ? "Friends-only lobby, no setup" :
               "LAN, or port forwarding", conn, 2, &cfg.steam);
        y += 66;
    }
    if (cfg.mode == MODE_HOST && cfg.steam) {
        gdi_para(f_small, PX, y, PW, C_DIM, "In the game: Options > Co-op > Session > Invite Steam friends, "
                 "or share the lobby ID shown there. Then start a New Game or load a save.");
    } else if (cfg.mode == MODE_HOST) {
        if (!lan[0])
            lan_address(lan, sizeof(lan));
        field(ID, PX, y, 140, "Port", cfg.listen, sizeof(cfg.listen), 1, "27900");
        snprintf(line, sizeof(line), "This PC on your network: %s\nOver the internet, forward UDP %s "
                 "to it and give friends your public IP.", lan, cfg.listen[0] ? cfg.listen : "27900");
        gdi_para(f_small, PX + 160, y + 4, PW - 160, C_DIM, line);
        if (link(ID, PX, y+66, "Saved addresses")) { page = PAGE_PROFILES; changed = 1; }
    } else if (cfg.mode == MODE_JOIN && cfg.steam) {
        field(ID, PX, y, PW, "Host's lobby ID (optional)", cfg.lobby, sizeof(cfg.lobby), 1,
              "Or accept a Steam invite in the game");
    } else if (cfg.mode == MODE_JOIN) {
        field(ID, PX, y, PW - 160, "Host address", cfg.host, sizeof(cfg.host), 0, "e.g. 192.168.1.20");
        field(ID, PX + PW - 140, y, 140, "Port", cfg.port, sizeof(cfg.port), 1, "27900");
        if (link(ID, PX, y+66, "Saved addresses")) { page = PAGE_PROFILES; changed = 1; }
    } else {
        gdi_para(f_small, PX, y - 6, PW, C_DIM, "The co-op mod stays installed but does nothing until you "
                 "pick Host or Join at the next start.");
    }
}

/* "3 days ago" */
static void ago(long t, char *out, int size)
{
    long d = (long)time(NULL) - t;

    if (t <= 0)
        snprintf(out, size, "never used");
    else if (d < 90)
        snprintf(out, size, "just now");
    else if (d < 3600)
        snprintf(out, size, "%ld min ago", d / 60);
    else if (d < 86400)
        snprintf(out, size, "%ld h ago", d / 3600);
    else if (d < 2 * 86400)
        snprintf(out, size, "yesterday");
    else if (d < 30 * 86400L)
        snprintf(out, size, "%ld days ago", d / 86400);
    else
        strftime(out, size, "%d %b %Y", localtime(&t));
}

/* "You, Alice, Bob" (our own name as "You"), cut to fit w */
/* cut out to fit w logical pixels in the small font, ending in "..." */
static void clip_text(char *out, float w)
{
    int n = (int)strlen(out);

    while (n > 4 && gdi_width(f_small, out) > w) {
        out[--n] = 0;
        if (n > 3)
            strcpy(out + n - 3, "...");
    }
}

#include "launcher_profiles_win.h"
#include "launcher_update_win.h"

static void who_text(const struct coop_session *s, float w, char *out, int size)
{
    int i;

    out[0] = 0;
    for (i = 0; i < s->nmembers; i++) {
        const char *name = !strcmp(s->members[i], s->me) ? "You" : s->members[i];

        if (i)
            strncat(out, ", ", size - strlen(out) - 1);
        strncat(out, name, size - strlen(out) - 1);
    }
    if (!out[0])
        snprintf(out, size, s->host == own_id ? "Just you so far" : "Players not recorded");
    clip_text(out, w);
}

/* How far into the campaign, 0..100 */
static int session_percent(const struct coop_session *s)
{
    int i = cp_index(s->level, s->cp);

    return i < 0 ? 0 : (i + 1) * 100 / NCP;
}

/* One saved co-op game: where it is up to, who played, how many, when */
static int session_card(int id, float x, float y, float w, const struct coop_session *s, int on, int usable)
{
    float h = 98, pw;
    int hit = control(id, x, y, w, h), hot = over(x, y, w, h) && usable, pct = session_percent(s);
    char line[160], when[32], pill[40];

    fill(x, y, w, h, on ? 0x2a2014 : hot ? C_TILE_HOT : C_TILE, 235);
    fill(x, y, 5, h, on ? C_ORANGE : usable ? C_FAINT : 0x2a3a42, 255);
    if (on)
        frame_rect(x, y, w, h, 2, C_ORANGE, 255);
    cp_text_of(s->level, s->cp, when, sizeof(when));
    snprintf(line, sizeof(line), "Level %d - %s", s->level, when);
    heading(x + 20, y + 9, 22, line, on ? C_ORANGE : usable ? C_TEXT : C_DIM, DT_LEFT);
    {
        int players = s->peak > 0 ? s->peak : s->nmembers;

        if (players > 0)
            snprintf(pill, sizeof(pill), "CO-OP  %d PLAYER%s", players, players == 1 ? "" : "S");
        else
            snprintf(pill, sizeof(pill), "CO-OP");
    }
    pw = gdi_width(f_small, pill) + 18;
    fill(x + w - pw - 10, y + 9, pw, 24, 0x1c4a5e, 255);
    gdi_text(f_small, x + w - pw / 2 - 10, y + 12, C_BLUE, pill, DT_CENTER);
    level_line(s->level, line, sizeof(line));
    clip_text(line, w - 40);
    gdi_text(f_small, x + 20, y + 38, on ? C_ORANGE : C_BLUE, line, DT_LEFT);
    who_text(s, w - 40, line, sizeof(line));
    gdi_text(f_small, x + 20, y + 56, usable ? C_TEXT : C_DIM, line, DT_LEFT);
    ago(s->updated, when, sizeof(when));
    if (s->host == own_id)
        snprintf(line, sizeof(line), "Hosted by you  -  %s", when);
    else
        snprintf(line, sizeof(line), "Hosted by %s  -  %s", s->host_name[0] ? s->host_name : "a friend", when);
    gdi_text(f_small, x + 20, y + 73, C_DIM, line, DT_LEFT);
    snprintf(line, sizeof(line), "%d%%", pct);
    gdi_text(f_small, x + w - 12, y + 73, C_DIM, line, DT_RIGHT);
    fill(x + 5, y + h - 5, w - 5, 5, 0x0d1a20, 255);
    fill(x + 5, y + h - 5, (w - 5) * pct / 100, 5, on ? C_ORANGE : C_BLUE, 255);
    focus_ring(id, x, y, w, h);
    return hit;
}

#define CARDS 3

static void page_saves(void)
{
    static char level_text[NLEVELS][12], cp_text[24][12];
    static const char *level_names[NLEVELS], *cp_names[24];
    float y = 92;
    int i, lv = start_level - 1, n, host = cfg.mode == MODE_HOST;
    char line[96];

    heading(PX, y, 18, "New game", C_BLUE, DT_LEFT);
    y += 24;
    if (host) {
        for (i = 0; i < NLEVELS; i++) {
            snprintf(level_text[i], sizeof(level_text[i]), "Level %d", i + 1);
            level_names[i] = level_text[i];
        }
        n = level_count(start_level);
        if (start_cp >= n)
            start_cp = n - 1;
        for (i = 0; i < n && i < 24; i++) {
            snprintf(cp_text[i], sizeof(cp_text[i]), "%d of %d", i + 1, n);
            cp_names[i] = cp_text[i];
        }
        if (choice(ID, PX, y, PW, "Start at level", sel < 0 && start_set ? "Chosen: the host starts here" :
                   "Pick one to start a new game there", level_names, NLEVELS, &lv)) {
            start_level = lv + 1;
            start_cp = 0;
            start_set = 1;
            sel = -1;
        }
        y += ROW + 2;
        if (choice(ID, PX, y, PW, "Checkpoint", NULL, cp_names, n, &start_cp)) {
            start_set = 1;
            sel = -1;
        }
        y += ROW;
        level_line(start_level, line, sizeof(line));
        clip_text(line, PW);
        gdi_text(f_small, PX, y + 2, C_BLUE, line, DT_LEFT);
        y += 26;
    } else {
        gdi_para(f_small, PX, y + 4, PW, C_DIM, cfg.mode == MODE_JOIN ?
                 "The host picks the level and checkpoint. You follow." :
                 "Solo is plain MDK2: use the game's own menus. Pick Host on the Play tab to choose a level.");
        y += 58;
    }

    heading(PX, y, 18, "Saved co-op games", C_BLUE, DT_LEFT);
    if (nsessions > CARDS) {
        snprintf(line, sizeof(line), "%d-%d of %d", saves_top + 1,
                 saves_top + CARDS < nsessions ? saves_top + CARDS : nsessions, nsessions);
        gdi_text(f_small, PX + PW, y + 2, C_DIM, line, DT_RIGHT);
    }
    y += 26;
    if (!nsessions) {
        gdi_para(f_small, PX, y + 6, PW, C_DIM, "None yet. A co-op game saves by itself 10 seconds into each "
                 "level, after every checkpoint and every couple of minutes. It shows up here with who you "
                 "played with and how far you got.");
        return;
    }
    for (i = saves_top; i < nsessions && i < saves_top + CARDS; i++) {
        int mine = sessions[i].host == own_id;

        if (session_card(ID + 5000 + i, PX, y, PW, &sessions[i], sel == i, host && mine) && host && mine) {
            sel = sel == i ? -1 : i;
            delete_ask = -1;
            changed = 1;
        }
        y += 102;
    }
    if (nsessions > CARDS) {
        if (saves_top > 0 && link(ID, PX, y + 2, "Newer")) {
            saves_top -= CARDS;
            changed = 1;
        }
        if (saves_top + CARDS < nsessions && link(ID, PX + PW - 60, y + 2, "Older")) {
            saves_top += CARDS;
            changed = 1;
        }
    }
    if (sel >= 0 && sel < nsessions) {
        if (link(ID, PX + 120, y + 2, delete_ask == sel ? "Click again to delete" : "Delete this save")) {
            if (delete_ask == sel) {
                session_remove(sessions[sel].sect);
                nsessions = sessions_list(sessions, 12, 0);
                sel = delete_ask = -1;
                saves_top = 0;
            } else {
                delete_ask = sel;
            }
            changed = 1;
        }
    }
}

/* what Host will start, above the button */
static void start_summary(char *out, int size)
{
    out[0] = 0;
    if (cfg.mode != MODE_HOST)
        return;
    if (sel >= 0 && sel < nsessions) {
        char who[64], cp[32];

        who_text(&sessions[sel], PW - 280, who, sizeof(who));
        cp_text_of(sessions[sel].level, sessions[sel].cp, cp, sizeof(cp));
        snprintf(out, size, "Continue level %d, %s: %s", sessions[sel].level, cp, who);
    } else if (start_set) {
        snprintf(out, size, "New game at level %d, checkpoint %d of %d", start_level, start_cp + 1,
                 level_count(start_level));
    } else {
        snprintf(out, size, "New game from the beginning (pick a level on the Saves tab)");
    }
}

static void page_settings(void)
{
    static const char *sections[] = { "Co-op", "Interface", "Voice and sound", "Display" };
    static const char *window[] = { "Windowed", "Fullscreen" };
    static const char *levels[] = { "0%", "10%", "20%", "30%", "40%", "50%", "60%", "70%", "80%", "90%", "100%" };
    static char res_text[64][24];
    static const char *res_names[64];
    float y = 92;
    int i;

    choice(ID, PX, y, PW, "Settings", NULL, sections, 4, &cfg.section);
    y += ROW + 10;
    if (cfg.section == 0) {
        toggle(ID, PX, y, PW, "Friendly fire", "The host's choice counts", &cfg.friendly_fire);
        if (cfg.friendly_fire != diff[0]) {         /* part of the difficulty: may make it Custom */
            diff[0] = cfg.friendly_fire;
            diff_match();
        }
        y += ROW + 2;
        if (choice(ID, PX, y, PW, "Difficulty", cfg.difficulty == DIFF_CUSTOM ? "Custom: in game, Options > Co-op" :
                   "The host's choice counts", diff_names, 5, &cfg.difficulty)) {
            if (cfg.difficulty == DIFF_CUSTOM)
                memcpy(diff, diff_read, sizeof(diff));      /* back to what coop.ini has */
            else
                memcpy(diff, diff_presets[cfg.difficulty], sizeof(diff));
            diff_match();
        }
        y += ROW + 2;
        choice(ID, PX, y, PW, "Co-op save every", NULL, interval_text, 4, &cfg.save_interval);
        y += ROW + 2;
        toggle(ID, PX, y, PW, "Back to level's hero", "After a level you switched hero in", &cfg.char_follow);
        y += ROW + 2;
        choice(ID, PX, y, PW, "Controls", cfg.controls == 1 ? "WASD, Space to jump" :
               cfg.controls == 0 ? "The game's own keys" : "Yours: Options > Co-op > Controls", ctrl_names, 3,
               &cfg.controls);
        y += ROW + 2;
        toggle(ID, PX, y, PW, "Gamepad", "A pad plays as you", &cfg.pad);
        y += ROW + 2;
        toggle(ID, PX, y, PW, "Developer keys", "F5 to F9: checkpoints, spawns, debug menu", &cfg.devkeys);
    } else if (cfg.section == 1) {
        toggle(ID, PX, y, PW, "Players on screen", "Health panel and name tags", &cfg.hud);
        y += ROW + 2;
        toggle(ID, PX, y, PW, "Player outlines", "Your friends through walls", &cfg.outline);
        y += ROW + 2;
        toggle(ID, PX, y, PW, "Cutscene timer", "Time left, with the skip vote", &cfg.movietimer);
        y += ROW + 2;
        toggle(ID, PX, y, PW, "Friends in minigames", "See the others as ghosts", &cfg.ghosts);
        y += ROW + 2;
        toggle(ID, PX, y, PW, "Emote wheel", NULL, &cfg.emotes);
        y += ROW + 2;
        toggle(ID, PX, y, PW, "Character wheel", "Switch hero at any time", &cfg.charwheel);
        y += ROW + 2;
        toggle(ID, PX, y, PW, "Text chat", NULL, &cfg.chat);
        y += ROW + 2;
        toggle(ID, PX, y, PW, "Pings", "Mark a spot for everyone", &cfg.pings);
    } else if (cfg.section == 2) {
        toggle(ID, PX, y, PW, "Voice chat", "Push to talk", &cfg.voice);
        y += ROW + 2;
        choice(ID, PX, y, PW, "Voice chat volume", NULL, voice_volume_names, 4, &cfg.voice_volume);
        y += ROW + 2;
        choice(ID, PX, y, PW, "Hearing range", "The host's choice counts", voice_range_names, 4, &cfg.voice_range);
        y += ROW + 10;
        choice(ID, PX, y, PW, "Effects", NULL, levels, 11, &sound[0]);
        y += ROW + 2;
        choice(ID, PX, y, PW, "Music", NULL, levels, 11, &sound[1]);
        y += ROW + 2;
        choice(ID, PX, y, PW, "Speech", NULL, levels, 11, &sound[2]);
        y += ROW + 2;
        choice(ID, PX, y, PW, "Lobby voice mode", "In game is push to talk", voice_mode_names, 2, &cfg.voice_mode);
        y += ROW + 2;
        choice(ID, PX, y, PW, "Microphone", NULL, lv_in_names, lv_in_count, &cfg.voice_in_dev);
        y += ROW + 2;
        choice(ID, PX, y, PW, "Voice output", NULL, lv_out_names, lv_out_count, &cfg.voice_out_dev);
    } else {
        if (choice(ID, PX, y, PW, "Screen", NULL, window, 2, &video.fullscreen))
            video_dirty = 1;
        y += ROW + 2;
        if (nres) {
            for (i = 0; i < nres; i++) {
                snprintf(res_text[i], sizeof(res_text[i]), "%d x %d", res[i].w, res[i].h);
                res_names[i] = res_text[i];
            }
            if (choice(ID, PX, y, PW, "Resolution", NULL, res_names, nres, &res_index))
                video_dirty = 1;
            y += ROW + 2;
        }
        toggle(ID, PX, y, PW, "Show launcher at start", "Off: hold Shift while the game starts", &cfg.show);
    }
}

static void page_about(void)
{
    float y = 92;
    char line[240], notes[180];
    int st, rb, auto_en;

    update_init();
    EnterCriticalSection(&update_lock);
    st = update_state;
    rb = update_rollback_ready;
    auto_en = update_sched.auto_enabled;
    snprintf(line, sizeof(line), "%s", update_msg[0] ? update_msg :
             (auto_en ? "Auto-check enabled (checks GitHub at most once per 24h)." : "Automatic update checks are off."));
    snprintf(notes, sizeof(notes), "%.160s", update_rel.notes);
    LeaveCriticalSection(&update_lock);

    heading(PX, y, 28, "MDK2 HD Co-op", C_TEXT, DT_LEFT);
    snprintf( notes[0] ? notes : line, 1, "%s", ""); /* keep compiler happy before reuse */
    {
        char vline[96];
        snprintf(vline, sizeof(vline), "Version %s  -  alpha", MOD_VERSION);
        gdi_text(f_small, PX + PW, y + 6, C_ORANGE, vline, DT_RIGHT);
    }
    y += 38;
    y += gdi_para(f_small, PX, y, PW, C_TEXT,
                  "Play the whole MDK2 campaign together. The host runs the world; everyone else "
                  "follows them from level to level as Kurt, Max or Doc.") + 8;
    y += gdi_para(f_tiny, PX, y, PW, C_DIM,
                  "-  Up to 16 players over Steam lobbies or direct UDP; co-op saves & drop-in rejoin\n"
                  "-  Synced enemies, bosses, doors & pickups; downed revives, pings, text & voice chat\n"
                  "-  Difficulty presets, Couch.cmd split-screen, and F1 Mod Quick Guide") + 12;

    heading(PX, y, 18, "Updates", C_BLUE, DT_LEFT);
    if (link(ID, PX + PW - 130, y - 2, auto_en ? "Auto-check: On" : "Auto-check: Off")) {
        EnterCriticalSection(&update_lock);
        update_sched.auto_enabled = !update_sched.auto_enabled;
        update_save_schedule();
        LeaveCriticalSection(&update_lock);
        changed = 1;
    }
    y += 24;
    y += gdi_para(f_small, PX, y, PW, st == UP_ERROR ? C_BAD : (st == UP_AVAILABLE || st == UP_APPLIED) ? C_ORANGE : C_DIM, line) + 4;
    if (st == UP_AVAILABLE) {
        EnterCriticalSection(&update_lock);
        snprintf(notes, sizeof(notes), "Notes: %.140s", update_rel.notes[0] ? update_rel.notes : "Release package verified via .sha256 sidecar.");
        LeaveCriticalSection(&update_lock);
        y += gdi_para(f_tiny, PX, y, PW, C_TEXT, notes) + 6;
    }
    if (button(ID, PX, y, 144, 36, st == UP_CHECKING ? "Checking..." : "Check now", 0) && st != UP_CHECKING && st != UP_DOWNLOADING)
        update_start_check(1);
    if (st == UP_AVAILABLE) {
        if (button(ID, PX + 154, y, 160, 36, "Install update", 1))
            update_start_apply();
    }
    if (rb) {
        if (button(ID, PX + (st == UP_AVAILABLE ? 324 : 154), y, 152, 36, "Roll back", 0))
            update_do_rollback();
    }
    y += 46;

    heading(PX, y, 18, "Credits", C_BLUE, DT_LEFT);
    y += 24;
    y += gdi_para(f_tiny, PX, y, PW, C_DIM,
                  "By TommoDotCommo, built with the Co-op Anything playbook. MIT licence (IMA ADPCM voice codec: original MIT code). "
                  "MDK2 HD belongs to its owners; this is an unofficial fan mod.") + 10;
    if (link(ID, PX, y, "Project page"))
        open_url(PROJECT_URL);
    if (link(ID, PX + 150, y, "Report a bug"))
        open_url(ISSUES_URL);
    if (link(ID, PX + 300, y, "Open mod folder"))
        ShellExecuteA(NULL, "open", coop_dir, NULL, NULL, SW_SHOWNORMAL);
}

/* Help: "Send diagnostics" (diag.c runs dist/diag.ps1 hidden): 0 idle, 1 working, 2 done, 3 failed; diag_msg: the zip, or why not */
static int diag_state;
static char diag_msg[MAX_PATH];

/* the bundle script finished? (the window's timer, and the test loop: the launcher is hidden there) */
static void diag_poll(void)
{
    if (diag_state != 1)
        return;
    diag_state = diag_bundle_poll(diag_msg, sizeof(diag_msg));
    changed = 1;
}

static void page_help(void)
{
    float y = 100;
    char line[200];

    heading(PX, y, 30, "Something wrong?", C_TEXT, DT_LEFT);
    y += 52;
    y += gdi_para(f_body, PX, y, PW, C_TEXT,
                  "One click saves a zip on your desktop with the logs of your recent runs, your co-op settings and a "
                  "summary of your PC and network. Send it to whoever is fixing the problem.") + 16;
    if (button(ID, PX, y, 360, 56, diag_state == 1 ? "Collecting..." : "Send diagnostics", 1) && diag_state != 1)
        diag_state = diag_bundle_start() ? 1 : 3;
    if (diag_state == 3 && !diag_msg[0])
        snprintf(diag_msg, sizeof(diag_msg), "could not start PowerShell");
    y += 72;
    if (diag_state == 1) {
        y += gdi_para(f_small, PX, y, PW, C_DIM, "Collecting logs and system details. This takes a few seconds.") + 14;
    } else if (diag_state == 2) {
        snprintf(line, sizeof(line), "Saved to your desktop: %s", strrchr(diag_msg, '\\') ? strrchr(diag_msg, '\\') + 1 : diag_msg);
        y += gdi_para(f_small, PX, y, PW, C_BLUE, line) + 14;
    } else if (diag_state == 3) {
        snprintf(line, sizeof(line), "Could not make the bundle: %s", diag_msg);
        y += gdi_para(f_small, PX, y, PW, C_BAD, line) + 14;
    }
    y += gdi_para(f_small, PX, y, PW, C_DIM,
                  "-  Your Windows and computer names, IP addresses and Steam IDs are replaced first\n"
                  "-  No saves and no game files go in it\n"
                  "-  It works after a crash too: start the game again and come here\n"
                  "-  In a game, F2 marks the moment something goes wrong: every player's game saves the last minute") + 18;
    if (link(ID, PX, y, "Report a bug"))
        open_url(ISSUES_URL);
    if (link(ID, PX + 180, y, "Open mod folder"))
        ShellExecuteA(NULL, "open", coop_dir, NULL, NULL, SW_SHOWNORMAL);
}

/* ---- lobby: who has connected, before the game starts ----
 * Host and Join over UDP first meet here. The host listens on the game's port and every joiner says hello
 * every 0.4 s (name, mod version, ready, its last round trip); the host answers with the roster, which
 * everyone draws, so each player can see at once whether they are connected and whether the others are.
 * The host presses Start: the roster says "starting in 3 s" and every launcher counts it down by itself.
 * The lobby socket is closed before the game starts; the game's own connection then takes over the port.
 * Text datagrams: "MDKL H <id> <ready> <rtt> <ts> <version>|<name>" to the host, and
 * "MDKL R <ts> <countdown ms or -1> <n>" followed by n lines "<name>|<id>|<ready>|<rtt>|<version>" back. */
#define LOB_MAX         16
#define LOB_COUNTDOWN   3000
#define LOB_STALE       4000
struct lob_member { char name[PLAYER_NAME + 1], ver[48]; unsigned id; int ready, rtt; DWORD seen, echo, got; struct sockaddr_in addr; };
static struct {
    int on, host, ready, n, rtt, connected, wsa;
    struct launcher_session session;
    struct ls_comm_state comm;
    SOCKET s;
    unsigned id;
    DWORD start_at, next_send, last_host, opened;
    struct lob_member m[LOB_MAX];
    struct sockaddr_in hostaddr;
    char status[96];
    char chat_in[LS_CHAT_MAX + 1];
} lob;

/* our name as it goes on the wire: '|' and line ends separate fields */
static int profile_session_active(void) { return lob.on; }

static void lob_sync_comm(void)
{
    uint32_t ids[LOB_MAX];
    const char *names[LOB_MAX];
    int i;
    for (i = 0; i < lob.n && i < LOB_MAX; i++) {
        ids[i] = lob.m[i].id;
        names[i] = lob.m[i].name;
    }
    ls_comm_sync_roster(&lob.comm, ids, names, lob.n);
}

static uint8_t lob_color_of(uint32_t id)
{
    int i;
    for (i = 0; i < lob.n; i++)
        if (lob.m[i].id == id)
            return (uint8_t)(i % LS_COLORS);
    return (uint8_t)(id % LS_COLORS);
}

static const char *lob_name(void)
{
    static char name[PLAYER_NAME + 1];
    char *c;

    snprintf(name, sizeof(name), "%s", cfg.name[0] ? cfg.name : "Player");
    for (c = name; *c; c++)
        if (*c == '|' || *c == '\n' || *c == '\r')
            *c = '_';
    return name;
}

static void lob_send_chat(const char *raw_text)
{
    char c1[256], m1[320];
    int clen, mlen, i;
    DWORD now = GetTickCount();

    if (!lob.on || (!lob.host && !lob.connected)) return;
    clen = ls_chat_encode_c1(lob.id, lob.comm.next_chat_seq++, lob_color_of(lob.id), raw_text, c1, sizeof(c1));
    if (clen <= 0) return;
    if (lob.host) {
        mlen = ls_chat_host_recv_c1(&lob.session, &lob.comm, now, c1, (size_t)clen, 1, m1, sizeof(m1));
        if (mlen > 0) {
            for (i = 1; i < lob.n; i++)
                sendto(lob.s, m1, mlen, 0, (struct sockaddr *)&lob.m[i].addr, sizeof(lob.m[i].addr));
            changed = 1;
        }
    } else {
        sendto(lob.s, c1, clen, 0, (struct sockaddr *)&lob.hostaddr, sizeof(lob.hostaddr));
    }
}

static int lob_wants(void)
{
    return cfg.mode != MODE_SOLO && !cfg.steam && cfg.show_lobby;
}

static void lob_leave(void)
{
    lv_close();
    if (lob.s != INVALID_SOCKET && lob.on)
        closesocket(lob.s);
    if (lob.wsa)
        WSACleanup();
    memset(&lob, 0, sizeof(lob));
    lob.s = INVALID_SOCKET;
    ls_leave(&lob.session);
    changed = 1;
}

static void lob_open(void)
{
    struct sockaddr_in a;
    WSADATA wsa;
    u_long nb = 1;
    int port = atoi(cfg.mode == MODE_HOST ? cfg.listen : cfg.port), no_reset = 0;
    DWORD ret;

    memset(&lob, 0, sizeof(lob));
    lob.s = INVALID_SOCKET;
    if (port <= 0 || port > 65535)
        port = 27900;
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) {
        logmsg("lobby: WSAStartup failed");
        done = 1;
        return;
    }
    lob.wsa = 1;
    lob.s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)(cfg.mode == MODE_HOST ? port : 0));
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (lob.s == INVALID_SOCKET || bind(lob.s, (struct sockaddr *)&a, sizeof(a)) != 0) {
        logmsg("lobby: cannot bind UDP %d (%d): starting without a lobby", port, WSAGetLastError());
        lob_leave();
        done = 1;
        return;
    }
    ioctlsocket(lob.s, FIONBIO, &nb);
    WSAIoctl(lob.s, 0x9800000c, &no_reset, sizeof(no_reset), NULL, 0, &ret, NULL, NULL);
    lob.on = 1;
    lob.opened = GetTickCount();
    lob.host = cfg.mode == MODE_HOST;
    ls_open(&lob.session, lob.host, lob.opened);
    ls_comm_init(&lob.comm);
    lob.id = GetTickCount() ^ ((unsigned)GetCurrentProcessId() << 16) ^ 0x5bd1e995u;
    if (!lob.host) {
        memset(&lob.hostaddr, 0, sizeof(lob.hostaddr));
        lob.hostaddr.sin_family = AF_INET;
        lob.hostaddr.sin_port = htons((u_short)port);
        lob.hostaddr.sin_addr.s_addr = inet_addr(cfg.host);
        snprintf(lob.status, sizeof(lob.status), "Looking for the host...");
        if (lob.hostaddr.sin_addr.s_addr == INADDR_NONE)
            snprintf(lob.status, sizeof(lob.status), "That is not an IP address: %s", cfg.host);
    } else {
        snprintf(lob.status, sizeof(lob.status), "Waiting for players (UDP %d)", port);
    }
    lob.n = 0;
    if (lob.host) {
        struct lob_member *m = &lob.m[lob.n++];
        snprintf(m->name, sizeof(m->name), "%s", lob_name());
        snprintf(m->ver, sizeof(m->ver), "%s", MOD_VERSION);
        m->id = lob.id;
        m->ready = 1;
    }
    lob_sync_comm();
    logmsg("lobby: open as %s on UDP %d", lob.host ? "host" : "client", port);
    changed = 1;
}

static void lob_send_roster(DWORD now)
{
    char body[1300], buf[1400];
    int i, n = 0, hn, left = lob.start_at ? (int)(lob.start_at - now) : -1;

    if (lob.start_at && left < 0)
        left = 0;
    for (i = 0; i < lob.n; i++) {
        int written = snprintf(body + n, sizeof(body) - n, "%s|%u|%d|%d|%s\n", lob.m[i].name, lob.m[i].id, lob.m[i].ready,
                               lob.m[i].rtt, lob.m[i].ver);
        if (written < 0 || written >= (int)sizeof(body) - n) return;
        n += written;
    }
    for (i = 1; i < lob.n; i++) {
        /* the header echoes this member's own hello time: its round trip is now minus that */
        hn = snprintf(buf, sizeof(buf), "MDKL R %lu %lu %d %d\n", (unsigned long)lob.m[i].echo,
                      (unsigned long)(now - lob.m[i].got), left, lob.n);
        memcpy(buf + hn, body, n);
        sendto(lob.s, buf, hn + n, 0, (struct sockaddr *)&lob.m[i].addr, sizeof(lob.m[i].addr));
    }
}

static void lob_poll(void)
{
    char buf[1500];
    struct sockaddr_in from;
    int flen, n, i, budget;
    DWORD now = GetTickCount();

    if (!lob.on)
        return;
    for (budget = 0; budget < 256; budget++) {
        flen = sizeof(from);
        n = recvfrom(lob.s, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &flen);
        if (n <= 0)
            break;
        if (lob.host && n == 8 && !memcmp(buf, "CPQ1", 4)) {
            static DWORD next_probe;
            unsigned char reply[64];
            if ((int32_t)(now-next_probe) >= 0) {
                int bytes = lp_answer((unsigned char *)buf, n, lob.n, 0, MOD_VERSION, reply);
                next_probe = now+100;
                if (bytes) sendto(lob.s, (const char *)reply, bytes, 0, (struct sockaddr *)&from, flen);
            }
            continue;
        }
        if (n == (int)sizeof(struct ls_voice_pkt) && !memcmp(buf, "MDKV", 4)) {
            struct ls_voice_pkt vchk;
            int16_t pcm_out[LS_VOICE_FRAME];
            uint32_t relay_ids[LOB_MAX];
            int played = 0, rcount = 0, ep_ok = 0, r;
            if (ls_voice_validate(buf, (size_t)n, &vchk)) {
                if (lob.host) {
                    for (i = 1; i < lob.n; i++)
                        if (lob.m[i].id == vchk.sender_id &&
                            lob.m[i].addr.sin_addr.s_addr == from.sin_addr.s_addr &&
                            lob.m[i].addr.sin_port == from.sin_port)
                            ep_ok = 1;
                } else {
                    ep_ok = (from.sin_addr.s_addr == lob.hostaddr.sin_addr.s_addr &&
                             from.sin_port == lob.hostaddr.sin_port);
                }
                if (ls_voice_accept(&lob.session, &lob.comm, now, buf, (size_t)n, ep_ok,
                                    lob.id, pcm_out, &played, relay_ids, &rcount)) {
                    if (lob.host && rcount > 0) {
                        for (r = 0; r < rcount; r++)
                            for (i = 1; i < lob.n; i++)
                                if (lob.m[i].id == relay_ids[r])
                                    sendto(lob.s, buf, n, 0, (struct sockaddr *)&lob.m[i].addr, sizeof(lob.m[i].addr));
                    }
                    if (played && cfg.voice)
                        lv_play_frame(cfg.voice_out_dev, pcm_out);
                    changed = 1;
                }
            }
            continue;
        }
        buf[n] = 0;
        if (n < 7 || memchr(buf, 0, n) || strncmp(buf, "MDKL ", 5))
            continue;
        if (lob.host && buf[5] == 'C' && buf[6] == '1') {
            char m1[320];
            unsigned sender_id = 0;
            int ep_ok = 0, mlen;
            if (sscanf(buf + 7, "%x", &sender_id) == 1) {
                for (i = 1; i < lob.n; i++)
                    if (lob.m[i].id == sender_id &&
                        lob.m[i].addr.sin_addr.s_addr == from.sin_addr.s_addr &&
                        lob.m[i].addr.sin_port == from.sin_port)
                        ep_ok = 1;
            }
            mlen = ls_chat_host_recv_c1(&lob.session, &lob.comm, now, buf, (size_t)n, ep_ok, m1, sizeof(m1));
            if (mlen > 0) {
                for (i = 1; i < lob.n; i++)
                    sendto(lob.s, m1, mlen, 0, (struct sockaddr *)&lob.m[i].addr, sizeof(lob.m[i].addr));
                changed = 1;
            }
            continue;
        }
        if (!lob.host && buf[5] == 'M' && buf[6] == '1') {
            int ep_ok = (from.sin_addr.s_addr == lob.hostaddr.sin_addr.s_addr &&
                         from.sin_port == lob.hostaddr.sin_port);
            if (ls_chat_client_recv_m1(&lob.session, &lob.comm, now, buf, (size_t)n, ep_ok))
                changed = 1;
            continue;
        }
        if (lob.host && buf[5] == 'H') {
            unsigned id;
            int ready, rtt, k = -1;
            unsigned long ts;
            char *who = strchr(buf + 7, '|'), ver[48];
            char *tail = buf + 7;

            if (sscanf(tail, "%x %d %d %lu %47[^|]", &id, &ready, &rtt, &ts, ver) < 5 || !who)
                continue;
            for (i = 1; i < lob.n; i++)
                if (lob.m[i].id == id)
                    k = i;
            if (k < 0) {
                if (lob.n >= LOB_MAX)
                    continue;
                k = lob.n++;
                memset(&lob.m[k], 0, sizeof(lob.m[k]));
                lob.m[k].id = id;
                logmsg("lobby: %s joined from %s:%d", who + 1, inet_ntoa(from.sin_addr), ntohs(from.sin_port));
            }
            lob.m[k].addr = from;
            lob.m[k].ready = ready != 0;
            lob.m[k].rtt = rtt;
            lob.m[k].seen = now;
            lob.m[k].echo = (DWORD)ts;
            lob.m[k].got = now;
            snprintf(lob.m[k].ver, sizeof(lob.m[k].ver), "%s", ver);
            snprintf(lob.m[k].name, sizeof(lob.m[k].name), "%s", who + 1);
            lob_sync_comm();
            changed = 1;
        } else if (!lob.host && buf[5] == 'R' && from.sin_addr.s_addr == lob.hostaddr.sin_addr.s_addr &&
                   from.sin_port == lob.hostaddr.sin_port) {
            unsigned long ts, held;
            int left, cnt, count = 0, own = 0, valid = 1;
            struct lob_member roster[LOB_MAX];
            char *line = strchr(buf, '\n');

            if (sscanf(buf + 7, "%lu %lu %d %d", &ts, &held, &left, &cnt) < 4 || !line)
                continue;
            if (cnt < 1 || cnt > LOB_MAX || left < -1 || left > LOB_COUNTDOWN)
                continue;
            if (ts)     /* ts: our own hello time, echoed; the host held it for `held` ms before answering */
                lob.rtt = (int)(now - (DWORD)ts - held) > 0 ? (int)(now - (DWORD)ts - held) : 0;
            for (line++; line && *line && count < LOB_MAX && count < cnt;) {
                char *end = strchr(line, '\n'), *f[5];
                int k = 0;
                if (end)
                    *end = 0;
                f[k++] = line;
                while (k < 5 && (line = strchr(line, '|')))
                    *line++ = 0, f[k++] = line;
                if (k == 5) {
                    struct lob_member *m = &roster[count++];
                    memset(m, 0, sizeof(*m));
                    snprintf(m->name, sizeof(m->name), "%s", f[0]);
                    m->id = (unsigned)strtoul(f[1], NULL, 10);
                    m->ready = atoi(f[2]);
                    m->rtt = atoi(f[3]);
                    snprintf(m->ver, sizeof(m->ver), "%s", f[4]);
                    if (m->id == lob.id) own++;
                    if (strcmp(m->ver, MOD_VERSION)) valid = 0;
                }
                line = end ? end + 1 : NULL;
            }
            if (count != cnt || own != 1 || !ls_roster(&lob.session, now, left, left < 0 || valid)) {
                continue;
            }
            lob.n = count;
            memcpy(lob.m, roster, lob.n * sizeof(roster[0]));
            lob_sync_comm();
            if (!lob.connected)
                logmsg("lobby: connected to the host (%d players)", lob.n);
            lob.connected = 1;
            lob.last_host = now;
            lob.start_at = lob.session.phase == LS_COUNTDOWN ? lob.session.deadline : 0;
            changed = 1;
        }
    }
    if (lob.host) {
        for (i = 1; i < lob.n; i++)
            if (now - lob.m[i].seen > LOB_STALE) {
                logmsg("lobby: %s left", lob.m[i].name);
                memmove(&lob.m[i], &lob.m[i + 1], (lob.n - i - 1) * sizeof(lob.m[0]));
                lob.n--;
                i--;
                lob_sync_comm();
                changed = 1;
            }
        if (now >= lob.next_send) {
            lob.next_send = now + 300;
            lob_send_roster(now);
        }
    } else {
        if (now >= lob.next_send && lob.hostaddr.sin_addr.s_addr != INADDR_NONE) {
            lob.next_send = now + 400;
            n = snprintf(buf, sizeof(buf), "MDKL H %x %d %d %lu %s|%s", lob.id, lob.ready, lob.rtt,
                         (unsigned long)now, MOD_VERSION, lob_name());
            sendto(lob.s, buf, n, 0, (struct sockaddr *)&lob.hostaddr, sizeof(lob.hostaddr));
        }
        if (lob.connected && now - lob.last_host > LOB_STALE) {
            lob.connected = 0;
            lob.n = 0;
            lob.start_at = 0;
            snprintf(lob.status, sizeof(lob.status), "Lost the host");
            changed = 1;
        } else if (!lob.connected && lob.hostaddr.sin_addr.s_addr != INADDR_NONE) {
            snprintf(lob.status, sizeof(lob.status), now - lob.opened > 5000 ?
                     "No answer from the host yet. Is it in its lobby, and is UDP %d open?" : "Looking for the host...",
                     ntohs(lob.hostaddr.sin_port));
        }
    }
    if (cfg.voice && (lob.host || lob.connected) && (lob.session.phase == LS_LOBBY || lob.session.phase == LS_COUNTDOWN)) {
        int16_t pcm_in[LS_VOICE_FRAME];
        int ptt_down = !headless && !field_focused && (GetAsyncKeyState('B') < 0);
        int frame_ptt = 0;
        while (lv_capture_frame(cfg.voice_in_dev, cfg.voice_mode, ptt_down, lob.comm.local_was_sending, now, pcm_in, &frame_ptt)) {
            uint8_t vflags = 0;
            if (ls_voice_should_send(&lob.comm, cfg.voice_mode, frame_ptt, pcm_in, LS_VOICE_VAD_DEFAULT, now, &vflags)) {
                struct ls_voice_pkt vpkt;
                ls_voice_pack(&lob.comm, lob.id, vflags, pcm_in, &vpkt);
                if (lob.host) {
                    for (i = 1; i < lob.n; i++)
                        sendto(lob.s, (const char *)&vpkt, sizeof(vpkt), 0, (struct sockaddr *)&lob.m[i].addr, sizeof(lob.m[i].addr));
                } else {
                    sendto(lob.s, (const char *)&vpkt, sizeof(vpkt), 0, (struct sockaddr *)&lob.hostaddr, sizeof(lob.hostaddr));
                }
                changed = 1;
            }
        }
    }
    {
        enum launcher_phase previous = lob.session.phase;
        ls_tick(&lob.session, now);
        if (previous != lob.session.phase) changed = 1;
    }
    if (lob.session.phase == LS_LOST) {
        lob.connected = 0; lob.start_at = 0; lob.n = 0;
        snprintf(lob.status, sizeof(lob.status), "Lost the host; reconnecting...");
    }
    if (lob.session.phase == LS_START) {
        logmsg("lobby: start (%d players)", lob.n);
        done = 1;
    }
}

static void lob_primary(void)
{
    DWORD now = GetTickCount();

    if (!lob.on)
        return;
    if (lob.host) {
        int i, compatible = 1;
        for (i = 0; i < lob.n; i++)
            if (strcmp(lob.m[i].ver, MOD_VERSION)) compatible = 0;
        if (ls_begin(&lob.session, now, lob.n > 1 ? LOB_COUNTDOWN : 600, compatible)) {
            lob.start_at = lob.session.deadline;
            lob_send_roster(now);
            logmsg("lobby: host started the countdown (%d players)", lob.n);
        }
    } else {
        lob.ready = !lob.ready;
    }
    changed = 1;
}

static void lobby_page(void)
{
    float y = 88, row_h;
    char line[160];
    DWORD now = GetTickCount();
    int i, others_unready = 0, mismatch = 0, chat_id, total_msg, start_msg;

    heading(PX, y, 24, lob.host ? "Lobby: you are hosting" : "Lobby", C_TEXT, DT_LEFT);
    if (button(ID, PX + PW - 168, y - 2, 168, 28, cfg.voice_mode ? "Voice: Activity" : "Voice: PTT (B)", 0)) {
        cfg.voice_mode = !cfg.voice_mode;
        changed = 1;
    }
    y = 120;
    row_h = lob.n > 8 ? 11.0f : 21.0f;
    for (i = 0; i < lob.n; i++) {
        const struct lob_member *m = &lob.m[i];
        struct ls_member_comm *cm = ls_comm_find(&lob.comm, m->id);
        int me = lob.host ? i == 0 : m->id == lob.id, same = !strcmp(m->ver, MOD_VERSION);
        int talking = me ? ls_local_talking(&lob.comm, now) : ls_member_talking(cm, now);
        DWORD col = ls_color_rgb((uint8_t)(i % LS_COLORS));
        char pill[48];

        fill(PX, y, PW, row_h - 1, me ? 0x2a2014 : C_TILE, 235);
        fill(PX, y, 5, row_h - 1, col, 255);
        snprintf(line, sizeof(line), "%s%s%s", m->name, me ? " (you)" : "", talking ? "  [SPEAKING]" : "");
        gdi_text(lob.n > 8 ? f_tiny : f_small, PX + 14, y + (lob.n > 8 ? -1 : 1), talking ? 0x5ae085 : C_TEXT, line, DT_LEFT);
        if (!me && cm && lob.n <= 8) {
            char vtxt[16];
            snprintf(vtxt, sizeof(vtxt), "%d%%", cm->volume);
            if (button(ID + 4100 + i * 2, PX + 214, y + 1, 52, row_h - 3, cm->muted ? "Muted" : "Mute", 0)) {
                cm->muted = !cm->muted;
                changed = 1;
            }
            if (button(ID + 4101 + i * 2, PX + 270, y + 1, 50, row_h - 3, vtxt, 0)) {
                cm->volume = (cm->volume + 50) % 250;
                changed = 1;
            }
        }
        if (i == 0)
            snprintf(pill, sizeof(pill), "host");
        else if (!same)
            snprintf(pill, sizeof(pill), "version %.16s", m->ver);
        else
            snprintf(pill, sizeof(pill), "%s  %d ms", m->ready ? "ready" : "not ready", m->rtt);
        gdi_text(lob.n > 8 ? f_tiny : f_small, PX + PW - 10, y + (lob.n > 8 ? -1 : 1),
                 !same ? C_BAD : m->ready || i == 0 ? C_BLUE : C_DIM, pill, DT_RIGHT);
        if (i > 0 && !m->ready)
            others_unready++;
        if (!same)
            mismatch++;
        y += row_h;
    }
    if (!lob.host && !lob.connected) {
        gdi_para(f_small, PX, y + 4, PW, lob.status[0] && strstr(lob.status, "Lost") ? C_BAD : C_DIM,
                 lob.status);
    }

    /* Pregame text chat history + input */
    fill(PX, 302, PW, 114, 0x0d1a20, 235);
    frame_rect(PX, 302, PW, 114, 1, 0x22343c, 255);
    total_msg = (int)lob.comm.history_total;
    start_msg = total_msg > 5 ? total_msg - 5 : 0;
    if (total_msg == 0) {
        gdi_text(f_small, PX + 12, 348, C_FAINT, "No messages yet. Type below to chat with the lobby.", DT_LEFT);
    } else {
        float cy = 307;
        for (i = start_msg; i < total_msg; i++) {
            const struct ls_chat_msg *msg = &lob.comm.history[(unsigned)i % LS_CHAT_HISTORY];
            char prefix[40];
            float pw;
            snprintf(prefix, sizeof(prefix), "%s: ", msg->name);
            pw = gdi_width(f_small, prefix);
            gdi_text(f_small, PX + 10, cy, ls_color_rgb(msg->color), prefix, DT_LEFT);
            snprintf(line, sizeof(line), "%s", msg->text);
            clip_text(line, PW - 20 - pw);
            gdi_text(f_small, PX + 10 + pw, cy, C_TEXT, line, DT_LEFT);
            cy += 21;
        }
    }
    chat_id = ID;
    field(chat_id, PX, 420, PW - 88, "Lobby chat", lob.chat_in, sizeof(lob.chat_in), 0, "Say something...");
    if ((button(ID, PX + PW - 80, 444, 80, 40, "Send", 0) || (activate && focus == chat_id)) && lob.chat_in[0]) {
        lob_send_chat(lob.chat_in);
        lob.chat_in[0] = 0;
        changed = 1;
    }

    y = LH - 224;
    if (lob.start_at) {
        int left = (int)(lob.start_at - now);
        snprintf(line, sizeof(line), "Starting in %d...", left > 0 ? (left + 999) / 1000 : 0);
        heading(PX, y, 24, line, C_ORANGE, DT_LEFT);
    } else if (lob.host) {
        snprintf(line, sizeof(line), "Everyone is ready.");
        if (lob.n < 2) {
            static char lan[32];
            if (!lan[0])
                lan_address(lan, sizeof(lan));
            snprintf(line, sizeof(line), "Nobody has joined yet. This PC: %s, port %s", lan,
                     cfg.listen[0] ? cfg.listen : "27900");
        } else if (others_unready) {
            snprintf(line, sizeof(line), "%d not ready yet (you can start anyway).", others_unready);
        }
        gdi_para(f_small, PX, y, PW, C_DIM, line);
    } else if (lob.connected) {
        gdi_para(f_small, PX, y, PW, C_DIM, "Connected. Press Ready, then wait for the host to start.");
    }
    if (mismatch)
        gdi_para(f_small, PX, y + 32, PW, C_BAD, "A different mod version is in the lobby: everyone should update.");
    if (lob.host) {
        if (button(ID, PX, LH - 160, PW, 64, lob.start_at ? "Starting..." : "Start game", 1))
            lob_primary();
    } else if (button(ID, PX, LH - 160, PW, 64, lob.ready ? "Ready (click to undo)" : "Ready", 1)) {
        lob_primary();
    }
    if (button(ID, PX, LH - 86, PW, 52, "Leave lobby", 0))
        lob_leave();
}

static void lg_sec(float x, float *y, float w, const char *title, DWORD col)
{
    fill(x + 8, *y, w - 16, 18, 0x15252d, 235);
    fill(x + 8, *y, 3, 18, col, 255);
    gdi_text(f_tiny_b, x + 16, *y + 2, col, title, DT_LEFT);
    *y += 21.0f;
}

static void lg_kv(float x, float *y, float kw, const char *k, const char *v, DWORD kc)
{
    gdi_text(f_tiny_b, x + 12, *y, kc, k, DT_LEFT);
    gdi_text(f_tiny, x + 12 + kw, *y, C_TEXT, v, DT_LEFT);
    *y += 16.4f;
}

static void lg_line(float x, float *y, const char *s, DWORD col)
{
    gdi_text(f_tiny, x + 12, *y, col, s, DT_LEFT);
    *y += 15.5f;
}

/* Full-window 3-column Mod Quick Guide overlay when F1 is held in the launcher */
static void draw_full_guide(void)
{
    float cw = 402, c1 = 18, c2 = 439, c3 = 860, y1 = 68, y2 = 68, y3 = 68, kw = 104;
    const char *preset = cfg.controls == 0 ? "Original" : cfg.controls == 2 ? "Custom" : "Modern";
    int orig = cfg.controls == 0;
    char hdr[96];

    fill(0, 0, LW, LH, 0x050b0e, 244);
    fill(18, 12, LW - 36, 44, 0x0d1b22, 248);
    frame_rect(18, 12, LW - 36, 44, 1, C_ORANGE, 220);
    fill(18, 12, 5, 44, C_ORANGE, 255);
    heading(32, 19, 24, "MDK2 HD CO-OP  -  MOD QUICK GUIDE", C_ORANGE, DT_LEFT);
    snprintf(hdr, sizeof(hdr), "Active preset: %s   |   Release F1 to return to the launcher", preset);
    gdi_text(f_small, LW - 32, 25, C_BLUE, hdr, DT_RIGHT);

    fill(c1, 62, cw, 620, 0x0a1317, 245);
    frame_rect(c1, 62, cw, 620, 1, 0x264452, 230);
    fill(c2, 62, cw, 620, 0x0a1317, 245);
    frame_rect(c2, 62, cw, 620, 1, 0x264452, 230);
    fill(c3, 62, cw, 620, 0x0a1317, 245);
    frame_rect(c3, 62, cw, 620, 1, 0x264452, 230);

    /* Column 1: Keybinds & Controls */
    snprintf(hdr, sizeof(hdr), "MOVEMENT & COMBAT (%s)", preset);
    lg_sec(c1, &y1, cw, hdr, C_ORANGE);
    lg_kv(c1, &y1, kw, "Move / Strafe", orig ? "Up / Down, Left / Right (or WASD)" : "W / S, A / D (or Arrow keys)", C_ORANGE);
    lg_kv(c1, &y1, kw, "Turn / Look", orig ? "Z / X turn, Home / End look" : "Mouse look; Z / C turn, Home / End", C_ORANGE);
    lg_kv(c1, &y1, kw, "Jump / Jet", orig ? "Right mouse (Kurt chute, Max jet)" : "Space (Kurt chute, Max jetpack)", C_ORANGE);
    lg_kv(c1, &y1, kw, "Fire / Melee", "Left mouse button (or use item)", C_ORANGE);
    lg_kv(c1, &y1, kw, "Sniper Mode", orig ? "Space toggles Kurt's sniper scope" : "Right mouse toggles Kurt's scope", C_ORANGE);
    lg_kv(c1, &y1, kw, "Sniper Zoom", orig ? "Up / W zoom in, Down / S zoom out" : "Wheel up/dn (or W / S) zoom", C_ORANGE);
    lg_kv(c1, &y1, kw, "Kurt Items", orig ? "Whl / 1,3 cycle; F/E open; - close" : "Q/E or Whl; F/Tab open; G close", C_ORANGE);
    lg_kv(c1, &y1, kw, "Max Weapons", orig ? "[ / ] or Wheel; E/Q equip or stow" : "Q/E or Wheel; F equip; G stow", C_ORANGE);
    lg_kv(c1, &y1, kw, "Doc Items", orig ? "] / [ U/D, - / = L/R, LMB use" : "G/F or Wheel U/D, Q/E L/R, LMB", C_ORANGE);
    lg_kv(c1, &y1, kw, "Minigames", "Stick 1: Move+Turn; Stick 2: Look+Strafe", C_ORANGE);

    y1 += 5.0f;
    lg_sec(c1, &y1, cw, "CO-OP KEYS & RADIAL WHEELS", C_BLUE);
    lg_kv(c1, &y1, kw, "F1 (hold)", "Open this Mod Quick Guide anywhere", C_BLUE);
    lg_kv(c1, &y1, kw, "R (hold)", "Hold beside downed teammate to revive", C_BLUE);
    lg_kv(c1, &y1, kw, "T", "Open text chat (Enter sends, Esc cancels)", C_BLUE);
    lg_kv(c1, &y1, kw, "V / MMB", "Place a 3D ping marker at crosshair", C_BLUE);
    lg_kv(c1, &y1, kw, "B (hold)", "Push-to-talk proximity voice chat", C_BLUE);
    lg_kv(c1, &y1, kw, "H (hold)", "Emote wheel: move mouse & release", C_BLUE);
    lg_kv(c1, &y1, kw, "J (hold)", "Character wheel: switch Kurt / Max / Doc", C_BLUE);
    lg_kv(c1, &y1, kw, "Backspace", "In cutscene: vote to skip (toggle)", C_BLUE);
    lg_kv(c1, &y1, kw, "Left mouse", "While bled out: spectate next teammate", C_BLUE);
    lg_kv(c1, &y1, kw, "F2", "Log marker & screenshot across all peers", C_BLUE);

    y1 += 5.0f;
    lg_sec(c1, &y1, cw, "GAMEPAD (XINPUT / DINPUT)", 0x5ae085);
    lg_kv(c1, &y1, kw, "LS / D-pad, RS", "Move & strafe (menus too); RS aims/looks", 0x5ae085);
    lg_kv(c1, &y1, kw, "A / B / Start", "A: Jump/jet/chute & select; B/Start: Esc", 0x5ae085);
    lg_kv(c1, &y1, kw, "RT / LT", "RT: Fire / use item; LT: Sniper mode", 0x5ae085);
    lg_kv(c1, &y1, kw, "RB / LB / Y", "RB/LB: Next/Prev item; Y: Open inv / equip", 0x5ae085);
    lg_kv(c1, &y1, kw, "X (hold) / Back", "Hold X: revive downed ally; Back: ping", 0x5ae085);

    /* Column 2: How Co-op Works */
    lg_sec(c2, &y2, cw, "CONNECTING & LOBBIES (2-16 PLAYERS)", C_ORANGE);
    lg_line(c2, &y2, "- Host runs the authoritative world; up to 15 clients join.", C_TEXT);
    lg_line(c2, &y2, "- Steam mode: Friends-only Steam lobbies (no port setup).", C_TEXT);
    lg_line(c2, &y2, "  Invite via Options > Co-op > Session or share Lobby ID.", C_DIM);
    lg_line(c2, &y2, "- Direct UDP: connects to Host IP:Port (default UDP 27900).", C_TEXT);
    lg_line(c2, &y2, "  Pre-game lobby shows connected players, ping & ready.", C_DIM);
    lg_line(c2, &y2, "- Play Solo disables co-op for a plain single-player run.", C_TEXT);

    y2 += 5.0f;
    lg_sec(c2, &y2, cw, "WORLD SYNC, DOWNED STATE & REVIVES", C_BLUE);
    lg_line(c2, &y2, "- Enemies, bosses, doors, switches, shots & pickups sync.", C_TEXT);
    lg_line(c2, &y2, "- At 0 HP you go down instead of dying and a bleedout timer", C_TEXT);
    lg_line(c2, &y2, "  starts (60s Story, 30s Normal, 20s Hard, 10s Nightmare).", C_DIM);
    lg_line(c2, &y2, "- Stand beside a downed teammate and hold Revive (R or pad X)", C_TEXT);
    lg_line(c2, &y2, "  for 2-5s to stand them back up with half health.", C_DIM);
    lg_line(c2, &y2, "- If the bleedout timer expires, you spectate living allies", C_TEXT);
    lg_line(c2, &y2, "  (LMB cycles camera) and return when clear or at next CP.", C_DIM);
    lg_line(c2, &y2, "- If all players are down or out at once, the CP reloads.", C_TEXT);

    y2 += 5.0f;
    lg_sec(c2, &y2, cw, "CUTSCENES & MINIGAMES", 0x5ae085);
    lg_line(c2, &y2, "- Cutscenes play in sync. Press Backspace to vote to skip;", C_TEXT);
    lg_line(c2, &y2, "  when a majority votes, everyone skips together.", C_DIM);
    lg_line(c2, &y2, "- An optional cutscene countdown bar shows time remaining.", C_TEXT);
    lg_line(c2, &y2, "- In minigames, teammates appear as translucent ghosts.", C_TEXT);

    y2 += 5.0f;
    lg_sec(c2, &y2, cw, "CO-OP SAVES, LEVEL SELECT & REJOINING", 0xc882ff);
    lg_line(c2, &y2, "- The host auto-saves 10s into each level, at checkpoints,", C_TEXT);
    lg_line(c2, &y2, "  and every 1 / 2 / 5 / 10 min (recording party & progress).", C_DIM);
    lg_line(c2, &y2, "- Resume any saved session from Saves tab or Options > Co-op.", C_TEXT);
    lg_line(c2, &y2, "- Start a new game at any Level & Checkpoint from Saves.", C_TEXT);
    lg_line(c2, &y2, "- Players who disconnect can rejoin at any time and are", C_TEXT);
    lg_line(c2, &y2, "  placed right into the host's current level and checkpoint.", C_DIM);

    /* Column 3: Features, Settings, Couch & Diagnostics */
    lg_sec(c3, &y3, cw, "HERO SWITCHING & DIFFICULTY", C_ORANGE);
    lg_line(c3, &y3, "- Hold J (Character wheel) to switch between Kurt, Max or Doc", C_TEXT);
    lg_line(c3, &y3, "  at any time during gameplay.", C_DIM);
    lg_line(c3, &y3, "- 'Back to level's hero' returns you to the stage's default hero", C_TEXT);
    lg_line(c3, &y3, "  when the next level loads (toggle in Settings / Gameplay).", C_DIM);
    lg_line(c3, &y3, "- 4 Difficulty Presets + Custom (host's setting applies to all):", C_TEXT);
    lg_line(c3, &y3, "  Story:     FF Off, 75% HP,  +0%/p,  50% dmg, 60s down, 2s rev", C_DIM);
    lg_line(c3, &y3, "  Normal:    FF Off, 100% HP, +50%/p, 100% dmg, 30s down, 3s rev", C_DIM);
    lg_line(c3, &y3, "  Hard:      FF Off, 125% HP, +75%/p, 150% dmg, 20s down, 4s rev", C_DIM);
    lg_line(c3, &y3, "  Nightmare: FF On,  150% HP, +100%/p, 200% dmg, 10s down, 5s rev", C_DIM);
    lg_line(c3, &y3, "  Custom:    Tune each setting in Esc > Options > Co-op > Gameplay", C_DIM);

    y3 += 5.0f;
    lg_sec(c3, &y3, cw, "HUD, VOICE CHAT, PING & EMOTES", C_BLUE);
    lg_line(c3, &y3, "- Party HUD shows every player's hero, HP bar, state & ping.", C_TEXT);
    lg_line(c3, &y3, "- 3D name tags & optional through-wall outlines track allies.", C_TEXT);
    lg_line(c3, &y3, "- Pings (V / MMB / pad Back) highlight a 3D spot for everyone.", C_TEXT);
    lg_line(c3, &y3, "- Proximity Voice Chat (hold B): 16 kHz audio with distance", C_TEXT);
    lg_line(c3, &y3, "  attenuation (Short, Medium, Long, or Everywhere).", C_DIM);
    lg_line(c3, &y3, "- Emote wheel (hold H): perform animations; moving cancels.", C_TEXT);

    y3 += 5.0f;
    lg_sec(c3, &y3, cw, "COUCH CO-OP & DIAGNOSTICS", 0x5ae085);
    lg_line(c3, &y3, "- Couch Co-op: run Couch.cmd to launch 2-4 tiled windows on", C_TEXT);
    lg_line(c3, &y3, "  one PC with per-window gamepad routing and Player 1 audio.", C_DIM);
    lg_line(c3, &y3, "- Press F2 in game to write a synchronized marker & screenshot", C_TEXT);
    lg_line(c3, &y3, "  across every connected player's log.", C_DIM);
    lg_line(c3, &y3, "- Help tab > Send diagnostics creates an anonymized Desktop zip.", C_TEXT);

    y3 += 5.0f;
    lg_sec(c3, &y3, cw, "DEVELOPER KEYS (WHEN ENABLED)", C_DIM);
    lg_kv(c3, &y3, kw, "F6 / F7 / F8", "Prev checkpoint / Next checkpoint / Next level", C_DIM);
    lg_kv(c3, &y3, kw, "F3 / F4 / F5", "200 lava damage / Teleport / Spawn Doganboy", C_DIM);
    lg_kv(c3, &y3, kw, "Num 1 / F9", "Spawn or damage enemy / BioWare debug menu", C_DIM);
    lg_kv(c3, &y3, kw, "PgUp/Dn, F10", "Emote browser (Num 0-9) / Test error & anims", C_DIM);

    gdi_text(f_small, LW / 2, 692, C_DIM,
             "Hold F1 in the launcher or at any time in game to view this Quick Guide   |   Esc > Options > Co-op changes all settings in game",
             DT_CENTER);
}

static void render(void)
{
    char line[160];

    nids = nhits = 0;
    memcpy(fb, bg, fw * fh * 4);

    /* left: under the logo */
    heading(360, 548, 52, "Online Co-op", C_ORANGE, DT_CENTER);
    snprintf(line, sizeof(line), "%s  -  up to 16 players", MOD_VERSION);
    gdi_text(f_small, 360, 614, C_DIM, line, DT_CENTER);
    gdi_text(f_small, 24, 690, C_FAINT, lob.on ? (lob.host ? "Hold F1: quick guide   Enter: start   Esc: leave the lobby" : "Hold F1: quick guide   Enter: ready   Esc: leave the lobby") :
             "Hold F1: quick guide   Enter: play   Tab: next   Esc: quit", DT_LEFT);

    /* right: tabs, page, play */
    fill(600 + 150, 0, 3, LH, C_BLUE, 60);
    {
        static const char *tabs[] = { "Play", "Saves", "Settings", "About", "Help" };
        float tx = PX;
        int t;

        for (t = 0; t < 5; t++) {
            if (tab(ID + 6000 + t, tx, 32, tabs[t], page == t))
                page = t, changed = 1;
            tx += (font.px ? title_width(tabs[t], 26) : gdi_width(f_label, tabs[t])) + 30;
        }
    }
    if (button(ID, LW - 52, 18, 34, 34, "x", 0))
        quit = 1;
    fill(PX, 80, PW, 1, 0x22343c, 255);

    if (page == PAGE_PLAY) {
        if (lob.on) lobby_page();
        else page_play();
    }
    else if (page == PAGE_PROFILES)
        page_profiles();
    else if (page == PAGE_SAVES)
        page_saves();
    else if (page == PAGE_SETTINGS)
        page_settings();
    else if (page == PAGE_HELP)
        page_help();
    else
        page_about();

    if (lob.on) {
        if (page != PAGE_PLAY) {
            DWORD now_bar = GetTickCount();
            int someone_talking = ls_local_talking(&lob.comm, now_bar), k;
            for (k = 0; k < lob.comm.nmembers && !someone_talking; k++)
                if (ls_member_talking(&lob.comm.members[k], now_bar)) someone_talking = 1;
            snprintf(line, sizeof(line), "%s | %d players%s%s | %u msg",
                     lob.host ? "Hosting" : lob.connected ? "Connected" : "Reconnecting", lob.n,
                     lob.start_at ? " | starting..." : lob.ready ? " | ready" : "",
                     someone_talking ? " | speaking" : "",
                     (unsigned)lob.comm.history_total);
            gdi_text(f_small, PX, LH - 118, C_BLUE, line, DT_LEFT);
            if (button(ID, PX, LH - 92, PW / 2 - 5, 52, lob.host ? "Start game" : lob.ready ? "Not ready" : "Ready", 1))
                lob_primary();
            if (button(ID, PX + PW / 2 + 5, LH - 92, PW / 2 - 5, 52, "Leave lobby", 0))
                lob_leave();
        }
        if (guide_open) draw_full_guide();
        return;
    }
    start_summary(line, sizeof(line));
    if (line[0])
        gdi_text(f_small, PX, LH - 118, C_ORANGE, line, DT_LEFT);
    if (button(ID, PX, LH - 92, PW, 64, cfg.mode == MODE_SOLO ? "Play solo" :
               cfg.mode == MODE_JOIN ? "Join" : sel >= 0 ? "Continue" : "Host", 1)) {
        if (lob_wants())
            lob_open();
        else
            done = 1;
    }
    if (guide_open)
        draw_full_guide();
}

/* draw until nothing changes (a control acting changes what the others show) */
static void pass(void)
{
    int i;
    for (i = 0; i < 3; i++) {
        changed = 0;
        field_focused = 0;
        render();
        clicked = activate = adjust = 0;
        if (!changed)
            break;
    }
    ntyped = 0;
}

/* ---- window ---- */

static HWND wnd;
static int headless;

static void present(void)
{
    HDC dc;
    if (headless || !wnd)
        return;
    dc = GetDC(wnd);
    BitBlt(dc, 0, 0, fw, fh, mem_dc, 0, 0, SRCCOPY);
    ReleaseDC(wnd, dc);
}

static int focus_valid(void)
{
    int i;
    for (i = 0; i < nids; i++)
        if (ids[i] == focus)
            return 1;
    return 0;
}

static void move_focus(int d)
{
    int i;
    if (!nids)
        return;
    for (i = 0; i < nids && ids[i] != focus; i++)
        ;
    focus = ids[i >= nids ? (d > 0 ? 0 : nids - 1) : (i + d + nids) % nids];
}

static void key(int vk)
{
    int shift = GetKeyState(VK_SHIFT) < 0, ctrl = GetKeyState(VK_CONTROL) < 0;
    if (vk == VK_F1) {
        if (!guide_open) {
            guide_open = 1;
            logmsg("launcher: quick guide shown");
        } else if (headless) {
            guide_open = 0;
        }
        pass();
        present();
        return;
    }
    if (guide_open && vk == VK_ESCAPE) {
        guide_open = 0;
        pass();
        present();
        return;
    }
    if (lob.on && ((page == PAGE_PLAY && vk == VK_RETURN && !field_focused) || vk == VK_ESCAPE)) {
        if (vk == VK_RETURN)
            lob_primary();
        else
            lob_leave();
        pass();
        present();
        return;
    }
    if (vk == VK_TAB || vk == VK_UP || vk == VK_DOWN)
        kb_focus = 1;
    switch (vk) {
    case VK_TAB:  move_focus(shift ? -1 : 1); break;
    case VK_DOWN: move_focus(1); break;
    case VK_UP:   move_focus(-1); break;
    case VK_LEFT: adjust = -1; break;
    case VK_RIGHT: adjust = 1; break;
    case VK_RETURN: case VK_SPACE:
        if (vk == VK_SPACE && field_focused) {
            if (ntyped < (int)sizeof(typed))
                typed[ntyped++] = ' ';  /* a name has spaces: typing it must not press or start anything */
            break;
        }
        if (vk == VK_SPACE && !focus_valid())
            break;                      /* Space only presses the focused button; Enter starts the game */
        if (!focus_valid()) {
            if (lob.on) lob_primary();
            else if (lob_wants()) lob_open();
            else done = 1;
        } else {
            activate = 1;
        }
        break;
    case VK_BACK:
        if (ntyped < (int)sizeof(typed))
            typed[ntyped++] = '\b';
        break;
    case VK_ESCAPE: quit = 1; break;
    case 'V':
        if (ctrl && OpenClipboard(wnd)) {
            HANDLE h = GetClipboardData(CF_TEXT);
            const char *s = h ? GlobalLock(h) : NULL;
            for (; s && *s && ntyped < (int)sizeof(typed); s++)
                if (*s >= 32 && *s < 127)
                    typed[ntyped++] = *s;
            if (h)
                GlobalUnlock(h);
            CloseClipboard();
        }
        break;
    }
    pass();
    present();
}

static int over_control(int x, int y)
{
    int i;
    for (i = 0; i < nhits; i++)
        if (x >= hits[i].x && x < hits[i].x + hits[i].w && y >= hits[i].y && y < hits[i].y + hits[i].h)
            return 1;
    return 0;
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        BitBlt(dc, 0, 0, fw, fh, mem_dc, 0, 0, SRCCOPY);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_NCHITTEST: {
        POINT p = { (short)LOWORD(lp), (short)HIWORD(lp) };
        ScreenToClient(h, &p);
        return over_control((int)(p.x / sc), (int)(p.y / sc)) ? HTCLIENT : HTCAPTION;
    }
    case WM_SETCURSOR:
        SetCursor(LoadCursor(NULL, over_control(mouse_x, mouse_y) ? IDC_HAND : IDC_ARROW));
        return TRUE;
    case WM_MOUSEMOVE:
        mouse_x = (int)((short)LOWORD(lp) / sc);
        mouse_y = (int)((short)HIWORD(lp) / sc);
        pass();
        present();
        return 0;
    case WM_NCMOUSEMOVE: {
        POINT p = { (short)LOWORD(lp), (short)HIWORD(lp) };
        ScreenToClient(h, &p);
        mouse_x = (int)(p.x / sc);
        mouse_y = (int)(p.y / sc);
        pass();
        present();
        break;
    }
    case WM_LBUTTONUP:
        mouse_x = (int)((short)LOWORD(lp) / sc);
        mouse_y = (int)((short)HIWORD(lp) / sc);
        clicked = 1;
        kb_focus = 0;
        pass();
        present();
        return 0;
    case WM_KEYDOWN:
        key((int)wp);
        return 0;
    case WM_KEYUP:
        if (wp == VK_F1 && guide_open) {
            guide_open = 0;
            pass();
            present();
            return 0;
        }
        break;
    case WM_CHAR:
        if (wp >= 32 && wp < 127 && wp != ' ' && ntyped < (int)sizeof(typed) && GetKeyState(VK_CONTROL) >= 0) {
            typed[ntyped++] = (char)wp;
            pass();
            present();
        }
        return 0;
    case WM_TIMER:
        if (wp == 3) {
            diag_poll();
            lob_poll();
            if (changed || page == PAGE_PROFILES || page == PAGE_ABOUT || (lob.on && lob.start_at)) {   /* a roster change or the countdown redraws */
                pass();
                present();
            }
            return 0;
        }
        caret_on = !caret_on;
        pass();
        present();
        return 0;
    case WM_CLOSE:
        quit = 1;
        return 0;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

static void save_shot(const char *name)
{
    char path[MAX_PATH];
    BYTE hdr[18] = { 0 };
    FILE *f;
    int y;

    snprintf(path, MAX_PATH, "%s\\%s.tga", coop_dir, name);
    hdr[2] = 2;
    hdr[12] = fw & 0xff; hdr[13] = fw >> 8;
    hdr[14] = fh & 0xff; hdr[15] = fh >> 8;
    hdr[16] = 32;
    hdr[17] = 0x28;                 /* top-left origin, 8 alpha bits */
    if (!(f = fopen(path, "wb")))
        return;
    fwrite(hdr, 1, 18, f);
    for (y = 0; y < fh; y++) {
        int x;
        for (x = 0; x < fw; x++) {
            DWORD p = fb[y * fw + x] | 0xff000000;
            fwrite(&p, 4, 1, f);
        }
    }
    fclose(f);
    logmsg("launcher: shot %s", path);
}

/* [test] launcher=1: commands from coop\launcher.txt */
static void test_commands(void)
{
    char path[MAX_PATH], line[256];
    FILE *f;

    snprintf(path, MAX_PATH, "%s\\launcher.txt", coop_dir);
    if (!(f = fopen(path, "r")))
        return;
    while (fgets(line, sizeof(line), f)) {
        int a, b;
        char *s;
        line[strcspn(line, "\r\n")] = 0;
        logmsg("launcher: test %s", line);
        if (sscanf(line, "key %d", &a) == 1) {
            key(a);
        } else if (sscanf(line, "click %d %d", &a, &b) == 2) {
            mouse_x = a;
            mouse_y = b;
            clicked = 1;
            kb_focus = 0;
            pass();
        } else if (!strncmp(line, "text ", 5)) {
            for (s = line + 5; *s && ntyped < (int)sizeof(typed); s++)
                typed[ntyped++] = *s;
            pass();
        } else if (sscanf(line, "page %d", &a) == 1 && a >= PAGE_PLAY && a <= PAGE_PROFILES) {
            page = a;
            pass();
        } else if (!strncmp(line, "say ", 4)) {
            lob_send_chat(line + 4);
            pass();
        } else if (sscanf(line, "voice %d", &a) == 1 && a > 0) {
            lv_tone_until = GetTickCount() + (DWORD)a;
            lv_next_tone_ms = 0;
            lob_poll();
            pass();
        } else if (sscanf(line, "mute %d", &a) == 1 && a >= 0 && a < lob.comm.nmembers) {
            lob.comm.members[a].muted = !lob.comm.members[a].muted;
            pass();
        } else if (sscanf(line, "vol %d %d", &a, &b) == 2 && a >= 0 && a < lob.comm.nmembers) {
            lob.comm.members[a].volume = b < 0 ? 0 : b > 200 ? 200 : b;
            pass();
        } else if (sscanf(line, "vmode %d", &a) == 1) {
            cfg.voice_mode = a != 0;
            pass();
        } else if (!strcmp(line, "update check")) {
            update_init();
            update_start_check(1);
        } else if (!strcmp(line, "update apply")) {
            update_init();
            update_start_apply();
        } else if (!strcmp(line, "update rollback")) {
            update_init();
            update_do_rollback();
        } else if (!strcmp(line, "dump")) {
            char state_path[MAX_PATH];
            FILE *state;
            DWORD now_dump = GetTickCount();
            snprintf(state_path, sizeof(state_path), "%s\\launcher-state.ini", coop_dir);
            state = fopen(state_path, "w");
            if (state) {
                int i, total_c = (int)lob.comm.history_total, start_c = total_c > LS_CHAT_HISTORY ? total_c - LS_CHAT_HISTORY : 0;
                update_init();
                EnterCriticalSection(&update_lock);
                fprintf(state, "[launcher]\npage=%d\nlobby=%d\nphase=%d\nplayers=%d\nready=%d\nconnected=%d\ncountdown=%d\n"
                               "chat_total=%u\nlocal_talking=%d\nvoice_mode=%d\nvoice_in_dev=%d\nvoice_out_dev=%d\n"
                               "update_state=%d\nupdate_err=%d\nupdate_rollback=%d\nupdate_tag=%s\n",
                        page, lob.on, lob.session.phase, lob.n, lob.ready, lob.connected, lob.start_at != 0,
                        (unsigned)lob.comm.history_total, ls_local_talking(&lob.comm, now_dump),
                        cfg.voice_mode, cfg.voice_in_dev - 1, cfg.voice_out_dev - 1,
                        update_state, update_last_err, update_rollback_ready, update_rel.tag);
                LeaveCriticalSection(&update_lock);
                for (i = 0; i < lob.n; i++) {
                    struct ls_member_comm *cm = ls_comm_find(&lob.comm, lob.m[i].id);
                    fprintf(state, "[player%d]\nready=%d\nmuted=%d\nvolume=%d\nvoice_frames=%u\ntalking=%d\n",
                            i, lob.m[i].ready, cm ? cm->muted : 0, cm ? cm->volume : 100,
                            cm ? cm->voice_frames : 0u, ls_member_talking(cm, now_dump));
                }
                for (i = start_c; i < total_c; i++) {
                    const struct ls_chat_msg *msg = &lob.comm.history[(unsigned)i % LS_CHAT_HISTORY];
                    fprintf(state, "[chat%d]\nname=%s\ntext=%s\ncolor=%u\nseq=%u\n",
                            i - start_c, msg->name, msg->text, (unsigned)msg->color, (unsigned)msg->seq);
                }
                fclose(state);
            }
        } else if (!strncmp(line, "shot ", 5)) {
            caret_on = 1;
            pass();
            save_shot(line + 5);
        }
    }
    fclose(f);
    DeleteFileA(path);
}

static int canvas_make(void)
{
    BITMAPINFO bi;
    HDC screen = GetDC(NULL);

    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = fw;
    bi.bmiHeader.biHeight = -fh;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    mem_dc = CreateCompatibleDC(screen);
    dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, (void **)&fb, NULL, 0);
    ReleaseDC(NULL, screen);
    if (!mem_dc || !dib || !(bg = malloc(fw * fh * 4)))
        return 0;
    dib_old = SelectObject(mem_dc, dib);
    SetBkMode(mem_dc, TRANSPARENT);
    return 1;
}

static void canvas_free(void)
{
    HFONT *f[] = { &f_body, &f_small, &f_label, &f_big, &f_tiny, &f_tiny_b };
    int i;
    for (i = 0; i < 6; i++)
        if (*f[i]) {
            DeleteObject(*f[i]);
            *f[i] = NULL;
        }
    if (mem_dc) {
        SelectObject(mem_dc, dib_old);
        DeleteDC(mem_dc);
    }
    if (dib)
        DeleteObject(dib);
    free(bg);
    free(title.px);
    free(font.px);
    title.px = font.px = NULL;
}

static void run(void)
{
    WNDCLASSA wc;
    RECT work;
    char exe[MAX_PATH];
    MSG msg;
    HMODULE dwm;
    /* the game is not DPI aware; the launcher's thread is, while it runs, so text stays sharp */
    HANDLE (WINAPI *dpi_context)(HANDLE) = (void *)GetProcAddress(GetModuleHandleA("user32.dll"),
                                                                  "SetThreadDpiAwarenessContext");
    HANDLE dpi_old = dpi_context && !headless ? dpi_context((HANDLE)-4) : NULL;

    SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0);
    sc = (work.right - work.left) * 0.8f / LW;
    if ((work.bottom - work.top) * 0.85f / LH < sc)
        sc = (work.bottom - work.top) * 0.85f / LH;
    if (sc > 2.0f) sc = 2.0f;
    if (sc < 0.5f) sc = 0.5f;
    if (headless)
        sc = 1.0f;
    fw = px(LW);
    fh = px(LH);
    if (!canvas_make()) {
        logmsg("launcher: no canvas: starting the game");
        canvas_free();
        if (dpi_old)
            dpi_context(dpi_old);
        return;
    }
    fonts_make();
    if (!dds_load("Titlescreen_PC.dds", &title))
        logmsg("launcher: no title art");
    if (!font_load())
        logmsg("launcher: no menu font");
    draw_background();
    focus = -1;
    pass();
    /* the Play button has the focus at first: Enter starts the game */
    focus = ids[nids - 1];
    pass();

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "mdk2coop_launcher";
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    wc.hIcon = ExtractIconA(wc.hInstance, exe, 0);
    RegisterClassA(&wc);
    wnd = CreateWindowExA(WS_EX_APPWINDOW, wc.lpszClassName, "MDK2 HD Co-op", WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX,
                          work.left + ((work.right - work.left) - fw) / 2,
                          work.top + ((work.bottom - work.top) - fh) / 2, fw, fh, NULL, NULL, wc.hInstance, NULL);
    if (!wnd) {
        logmsg("launcher: no window: starting the game");
        canvas_free();
        if (dpi_old)
            dpi_context(dpi_old);
        return;
    }
    if ((dwm = LoadLibraryA("dwmapi.dll"))) {
        /* Windows 11: rounded corners */
        HRESULT (WINAPI *set_attr)(HWND, DWORD, const void *, DWORD) = (void *)GetProcAddress(dwm, "DwmSetWindowAttribute");
        int round = 2;
        if (set_attr)
            set_attr(wnd, 33, &round, sizeof(round));
    }
    if (!headless) {
        ShowWindow(wnd, SW_SHOW);
        SetForegroundWindow(wnd);
        present();
        SetTimer(wnd, 1, 530, NULL);
        SetTimer(wnd, 3, 50, NULL);
    } else {
        SetTimer(wnd, 2, 50, NULL);
    }
    logmsg("launcher: shown at %dx%d%s", fw, fh, headless ? " (hidden, test)" : "");
    while (!done && !quit && GetMessageA(&msg, NULL, 0, 0) > 0) {
        if (headless && msg.message == WM_TIMER) {
            test_commands();
            lob_poll();
            diag_poll();
            if (changed && (lob.on || page == PAGE_HELP))
                pass();
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    if (profile_loaded) InterlockedExchange(&profile_stop, 1);
    lob_leave();
    DestroyWindow(wnd);
    wnd = NULL;
    UnregisterClassA(wc.lpszClassName, wc.hInstance);
    if (wc.hIcon)
        DestroyIcon(wc.hIcon);
    canvas_free();
    if (dpi_old)
        dpi_context(dpi_old);
}

/* Before the game's first window (input.c) */
void launcher_run(void)
{
    char test_ini[MAX_PATH];
    int shift = GetAsyncKeyState(VK_SHIFT) < 0;

    settings_read();
    snprintf(test_ini, MAX_PATH, "%s\\coop.ini", coop_dir);
    headless = test_background && GetPrivateProfileIntA("test", "launcher", 0, test_ini);
    if (strstr(GetCommandLineA(), "+connect_lobby")) {
        logmsg("launcher: skipped, Steam started the game to join a lobby");
        return;
    }
    if (test_background && !headless)
        return;
    if (couch_active) {
        logmsg("launcher: skipped, couch game");
        return;
    }
    if (!cfg.show && !(shift && !headless)) {
        logmsg("launcher: off ([launcher] show=0; hold Shift at start to show it)");
        return;
    }
    run();
    if (quit) {
        logmsg("launcher: quit");
        ExitProcess(0);
    }
    if (memcmp(diff, diff_read, sizeof(diff))) {
        int i;
        for (i = 0; i < NDIFF; i++)
            put_int("game", diff_keys[i], diff[i]);
    }
    put_int("save", "interval", intervals[cfg.save_interval]);
    put("controls", "preset", ctrl_ids[cfg.controls]);
    put_int("hud", "show", cfg.hud);
    put_int("launcher", "show", cfg.show);
    {
        int i;
        for (i = 0; i < NSWITCHES; i++)
            put_int(switches[i].section, switches[i].key, *switches[i].v);
    }
    put_int("voice", "volume", voice_volumes[cfg.voice_volume]);
    put_int("voice", "range", voice_ranges[cfg.voice_range]);
    put_int("voice", "mode", cfg.voice_mode);
    put_int("voice", "in_dev", cfg.voice_in_dev - 1);
    put_int("voice", "out_dev", cfg.voice_out_dev - 1);
    if (video_dirty)
        video_save();
    sound_save();
    settings_apply();
    logmsg("launcher: play, %s%s", mode_names[cfg.mode],
           cfg.mode == MODE_SOLO ? " (co-op off)" : cfg.steam ? " over Steam" : " over UDP");
}
