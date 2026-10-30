/* In-game route recorder/player. Playback injects controls; positions are assertions only. */
#include "bridge.h"
#include "route.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

enum { BOT_IDLE, BOT_RECORD, BOT_REPLAY, BOT_PASS, BOT_FAIL };
static int mode, have_state, blocked;
static int required_peers;
static struct route_state observed;
static struct route recording;
static struct route_player player;
static FILE *output;
static char name[64], reason[160] = "idle";
static double started, status_at;
static unsigned int frame_ms, recorded_frames, last_ms;

static double number(int n)
{
    unsigned int o = lua_getparam(n);
    return o && lua_isnumber(o) ? lua_getnumber(o) : 0;
}

static const char *mode_name(void)
{
    const char *names[] = { "idle", "recording", "running", "pass", "fail" };
    return names[mode];
}

static void status(void)
{
    char path[MAX_PATH], final[MAX_PATH];
    FILE *f;
    snprintf(path, sizeof(path), "%s\\route_status.tmp", coop_dir);
    snprintf(final, sizeof(final), "%s\\route_status.txt", coop_dir);
    if (!(f = fopen(path, "w"))) return;
    fprintf(f, "status %s\nroute %s\nreason %s\nms %u\nlevel %d\ncheckpoint %d\nlife %d\nplayers %d\nposition %.3f %.3f %.3f\n",
            mode_name(), name, reason, frame_ms, observed.level, observed.cp, revive_life(),
            players_connected(), observed.pos[0], observed.pos[1], observed.pos[2]);
    fclose(f);
    MoveFileExA(path, final, MOVEFILE_REPLACE_EXISTING);
    status_at = now_ms();
}

static void finish(int result, const char *why)
{
    mode = result;
    snprintf(reason, sizeof(reason), "%s", why);
    if (output) { fclose(output); output = NULL; }
    input_replay(NULL);
    route_free(&recording);
    logmsg("route %s: %s (%s)", name, mode_name(), reason);
    status();
}

static int route_name(const char *s)
{
    size_t i, n = strlen(s);
    if (!n || n >= sizeof(name)) return 0;
    for (i = 0; i < n; i++) if (!isalnum((unsigned char)s[i]) && s[i] != '-' && s[i] != '_') return 0;
    return 1;
}

/* Called before update_scene, so each mouse delta is delivered once during that update. */
void bot_before_frame(void)
{
    struct route_input input;
    if (mode != BOT_RECORD && mode != BOT_REPLAY) return;
    frame_ms = (unsigned int)(now_ms() - started);
    if (mode == BOT_REPLAY) {
        route_input_at(&player, frame_ms, &input);
        input_replay(&input);
    }
}

void bot_level_loaded(int level)
{
    if ((mode == BOT_RECORD || mode == BOT_REPLAY) && level == observed.level)
        finish(BOT_FAIL, "checkpoint reloaded during route");
}

/* coop_route_tick(player, level, checkpoint, blocked): after the normal co-op tick. */
static void tick(void)
{
    BYTE *gob = tolua_getusertype(1, NULL);
    struct route_input input;
    char error[160];
    int result, i;
    input_capture(&input);
    have_state = gob_alive(gob);
    blocked = (int)number(4);
    observed.level = (int)number(2);
    observed.cp = (int)number(3);
    if (have_state) {
        observed.type = gob_type(gob);
        memcpy(observed.pos, gob + GOB_POSITION, sizeof(observed.pos));
        memcpy(observed.rot, gob + GOB_ORIENTATION, sizeof(observed.rot));
    }
    if (mode == BOT_RECORD || mode == BOT_REPLAY) {
        if (!have_state || revive_life() != LIFE_ALIVE || blocked || players_connected() < required_peers) {
            finish(BOT_FAIL, "player missing, down, dead, paused or disconnected"); return;
        }
        if (mode == BOT_RECORD) {
            struct route_frame v;
            for (i = 0; i < ROUTE_KEYS; i++)
                if ((i == 1 || (i >= 59 && i <= 68) || i == 87 || i == 88) &&
                    (input.keys[i/8] & (1 << (i%8)))) {
                    finish(BOT_FAIL, "menu or developer key pressed while recording"); return;
                }
            if (++recorded_frames > ROUTE_MAX_FRAMES || frame_ms > ROUTE_MAX_MS) {
                finish(BOT_FAIL, "recording exceeded one hour or frame limit"); return;
            }
            v.ms = frame_ms; v.input = input; v.state = observed;
            if (!route_write_frame(output, &v)) { finish(BOT_FAIL, "cannot write route"); return; }
            last_ms = frame_ms;
        } else {
            result = route_check(&player, frame_ms, &observed, error, sizeof(error));
            if (result < 0) { finish(BOT_FAIL, error); return; }
            if (result > 0) { finish(BOT_PASS, "recorded progression and endpoint reached"); return; }
        }
    }
    if ((test_background || mode != BOT_IDLE) && now_ms() - status_at > 250) status();
}

/* coop_route("record"|"replay", name), coop_route("stop"|"status").
 * Files stay under coop/, with no path or Lua execution in a route. */
static void command(void)
{
    long len;
    const char *action = luaL_check_lstr(1, &len), *requested;
    char path[MAX_PATH], temp[MAX_PATH], error[160];
    FILE *f;
    struct route_input discarded;
    if (!strcmp(action, "status")) { status(); lua_pushstring(mode_name()); return; }
    if (!strcmp(action, "stop")) {
        if (mode == BOT_RECORD) {
            int ok = fprintf(output, "END %u\n", last_ms) > 0;
            if (fclose(output) != 0) ok = 0;
            output = NULL;
            snprintf(temp, sizeof(temp), "%s\\%s.route.partial", coop_dir, name);
            snprintf(path, sizeof(path), "%s\\%s.route", coop_dir, name);
            f = fopen(temp, "r");
            if (!ok || !f) { if (f) fclose(f); finish(BOT_FAIL, "cannot finalize recording"); return; }
            ok = route_load(f, &recording, error, sizeof(error));
            fclose(f);
            if (!ok) { finish(BOT_FAIL, error); return; }
            if (!MoveFileA(temp, path)) { finish(BOT_FAIL, "cannot save recording (name exists?)"); return; }
            finish(BOT_IDLE, "recording saved");
        } else if (mode == BOT_REPLAY) finish(BOT_FAIL, "replay cancelled");
        return;
    }
    if (strcmp(action, "record") && strcmp(action, "replay")) { lua_pushnil(); return; }
    if (mode == BOT_RECORD || mode == BOT_REPLAY) { lua_pushnil(); return; }
    requested = luaL_check_lstr(2, &len);
    if (!route_name(requested)) { finish(BOT_FAIL, "invalid route name"); return; }
    snprintf(name, sizeof(name), "%s", requested);
    if (!have_state || blocked || observed.level < 1 || observed.cp < 0 || revive_life() != LIFE_ALIVE) {
        finish(BOT_FAIL, "route must start with a living player in a running checkpoint"); return;
    }
    snprintf(path, sizeof(path), "%s\\%s.route", coop_dir, name);
    snprintf(temp, sizeof(temp), "%s\\%s.route.partial", coop_dir, name);
    if (!strcmp(action, "record")) {
        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES || GetFileAttributesA(temp) != INVALID_FILE_ATTRIBUTES) {
            finish(BOT_FAIL, "recording name already exists"); return;
        }
        output = fopen(temp, "w");
        if (!output || !route_write_header(output, &observed)) { finish(BOT_FAIL, "cannot open recording"); return; }
        mode = BOT_RECORD;
        recorded_frames = last_ms = 0;
    } else {
        if (!test_background) { finish(BOT_FAIL, "replay requires background test mode"); return; }
        f = fopen(path, "r");
        if (!f) { finish(BOT_FAIL, "cannot open route"); return; }
        int ok = route_load(f, &recording, error, sizeof(error));
        fclose(f);
        if (!ok || !route_begin(&player, &recording, &observed, error, sizeof(error))) {
            finish(BOT_FAIL, error); return;
        }
        mode = BOT_REPLAY;
        memset(&discarded, 0, sizeof(discarded));
        input_replay(&discarded);
    }
    input_capture(&discarded);
    started = now_ms();
    required_peers = players_connected();
    frame_ms = 0;
    snprintf(reason, sizeof(reason), "%s", mode == BOT_RECORD ? "recording controls and progression" : "playing recorded controls");
    logmsg("route %s: %s", name, mode_name());
    status();
    lua_pushnumber(1);
}

/* coop_note(name, text): the campaign bot's results, written whole to coop/note_<name>.txt for
 * tools/campaign_bot.py (name: the same characters as a route name) */
static void note(void)
{
    long len;
    const char *key = luaL_check_lstr(1, &len), *text = luaL_check_lstr(2, &len);
    char path[MAX_PATH], final[MAX_PATH];
    FILE *f;
    size_t i, n = strlen(key);
    if (!n || n >= 64) return;
    for (i = 0; i < n; i++)
        if (!isalnum((unsigned char)key[i]) && key[i] != '_' && key[i] != '-') return;
    snprintf(path, sizeof(path), "%s\\note_%s.tmp", coop_dir, key);
    snprintf(final, sizeof(final), "%s\\note_%s.txt", coop_dir, key);
    if (!(f = fopen(path, "w"))) return;
    fputs(text, f);
    fclose(f);
    MoveFileExA(path, final, MOVEFILE_REPLACE_EXISTING);
}

/* coop_nearest_enemy(x, y, z, range [, k]): the k-th nearest (default 1) living actor that is not a
 * player, within range, or nil. For the campaign bot's gunner, which skips the ones it gave up on. */
#define NEAREST_MAX 8
static void nearest_enemy(void)
{
    float at[3] = { (float)number(1), (float)number(2), (float)number(3) };
    double range = number(4), dist[NEAREST_MAX];
    int k = (int)number(5), i, n = 0, j;
    BYTE *list[NEAREST_MAX];
    if (k < 1) k = 1;
    if (k > NEAREST_MAX) k = NEAREST_MAX;
    for (i = 0; i < GOB_SLOTS; i++) {
        BYTE *g = (BYTE *)GOB_TABLE[i * 2];
        float *p;
        double d;
        if (!g || g == local_player || puppet_slot(g) >= 0 || !gob_alive(g) || !gob_is_actor(g) || gob_hp(g) <= 0)
            continue;
        p = (float *)(g + GOB_POSITION);
        d = (p[0] - at[0]) * (p[0] - at[0]) + (p[1] - at[1]) * (p[1] - at[1]) + (p[2] - at[2]) * (p[2] - at[2]);
        if (d > range * range) continue;
        /* keep the k nearest, sorted */
        if (n < k) n++;
        else if (d >= dist[n - 1]) continue;
        for (j = n - 1; j > 0 && dist[j - 1] > d; j--) { dist[j] = dist[j - 1]; list[j] = list[j - 1]; }
        dist[j] = d;
        list[j] = g;
    }
    push_gob(n >= k ? list[k - 1] : NULL);
}

/* coop_quat(gob) -> w, x, y, z: a gob's orientation. coop_set_quat(gob, w, x, y, z): set it whole, so
 * the campaign bot can face an exact bearing upright (mdkGobPointToXYZ turns from the current
 * orientation, and its small rolls built up until Max lay on his side, then upside down). */
static void quat_get(void)
{
    BYTE *g = tolua_getusertype(1, NULL);
    float *q;
    int i;
    if (!gob_alive(g)) return;
    q = (float *)(g + GOB_ORIENTATION);
    for (i = 0; i < 4; i++) lua_pushnumber(q[i]);
}

static void quat_set(void)
{
    BYTE *g = tolua_getusertype(1, NULL);
    float *q, v[4], n;
    int i;
    if (!gob_alive(g)) return;
    for (i = 0; i < 4; i++) v[i] = (float)number(i + 2);
    n = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2] + v[3] * v[3]);
    if (n < 0.5f) return;
    q = (float *)(g + GOB_ORIENTATION);
    for (i = 0; i < 4; i++) q[i] = v[i] / n;
}

void bot_lua_init(void)
{
    tolua_function(NULL, "coop_quat", quat_get);
    tolua_function(NULL, "coop_set_quat", quat_set);
    tolua_function(NULL, "coop_nearest_enemy", nearest_enemy);
    tolua_function(NULL, "coop_note", note);
    tolua_function(NULL, "coop_route", command);
    tolua_function(NULL, "coop_route_tick", tick);
}
