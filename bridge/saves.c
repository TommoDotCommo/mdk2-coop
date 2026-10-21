/*
 * Co-op saves. The host calls a save (coop.SaveNow in Lua: every few minutes and after
 * each checkpoint); every game in the level then makes a full instant save of its own
 * world (the game's quicksave: every gob, the Save table, its player's inventory and hp)
 * under the same number. Loading save N in every game brings the whole session back to
 * that moment, each player with their own things.
 *
 * Each game keeps a record of its co-op saves in <game dir>\coop\saves.ini, per host
 * (section h<host id>), in two alternating slots by save number (a, b), so a save that
 * dies half-written leaves the one before it. Save files are save\c<id><a|b>.sav in the
 * game's save folder: the game cuts save names after 16 characters, and every copy on one
 * PC shares the folder, so the name carries 4 hex digits of this copy's player id.
 *
 * The host also records how far the session has got (each level start and checkpoint, in
 * [h<id>] p_level / p_cp, with the newest save number at that moment in p_after): where the
 * game cannot save (Max's tutorial at the start of level 2), resuming the newest save would
 * go back a whole level, and the checkpoint is the better place to resume.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "bridge.h"

struct save_rec {
    unsigned int no;
    int level, cp;
    char file[32];
};

static double num_param(int n)
{
    unsigned int o = lua_getparam(n);
    return (o && lua_isnumber(o)) ? lua_getnumber(o) : 0.0;
}

static void ini_path(char *path)
{
    snprintf(path, MAX_PATH, "%s\\saves.ini", coop_dir);
}

static int rec_read(unsigned int host, char slot, struct save_rec *r)
{
    char path[MAX_PATH], sect[16], key[16];

    ini_path(path);
    snprintf(sect, sizeof(sect), "h%08x", host);
    snprintf(key, sizeof(key), "%c_no", slot);
    r->no = (unsigned int)GetPrivateProfileIntA(sect, key, 0, path);
    snprintf(key, sizeof(key), "%c_level", slot);
    r->level = (int)GetPrivateProfileIntA(sect, key, 0, path);
    snprintf(key, sizeof(key), "%c_cp", slot);
    r->cp = (int)GetPrivateProfileIntA(sect, key, 0, path);
    snprintf(key, sizeof(key), "%c_file", slot);
    GetPrivateProfileStringA(sect, key, "", r->file, sizeof(r->file), path);
    return r->no > 0 && r->file[0];
}

static void rec_write(unsigned int host, const struct save_rec *r)
{
    char path[MAX_PATH], sect[16], key[16], val[32];
    char slot = (r->no & 1) ? 'a' : 'b';

    ini_path(path);
    snprintf(sect, sizeof(sect), "h%08x", host);
    /* the number last: a record cut short by a crash has the old number and is never matched */
    snprintf(key, sizeof(key), "%c_no", slot);
    WritePrivateProfileStringA(sect, key, "0", path);
    snprintf(key, sizeof(key), "%c_level", slot);
    snprintf(val, sizeof(val), "%d", r->level);
    WritePrivateProfileStringA(sect, key, val, path);
    snprintf(key, sizeof(key), "%c_cp", slot);
    snprintf(val, sizeof(val), "%d", r->cp);
    WritePrivateProfileStringA(sect, key, val, path);
    snprintf(key, sizeof(key), "%c_file", slot);
    WritePrivateProfileStringA(sect, key, r->file, path);
    snprintf(key, sizeof(key), "%c_no", slot);
    snprintf(val, sizeof(val), "%u", r->no);
    WritePrivateProfileStringA(sect, key, val, path);
}

/* The record of save no from host; no 0: the newest, in level if level > 0 */
static int rec_find(unsigned int host, unsigned int no, int level, struct save_rec *out)
{
    struct save_rec r[2];
    int have[2], i, best = -1;

    have[0] = rec_read(host, 'a', &r[0]);
    have[1] = rec_read(host, 'b', &r[1]);
    for (i = 0; i < 2; i++) {
        if (!have[i])
            continue;
        if (no && r[i].no == no) {
            best = i;
            break;
        }
        if (!no && (level <= 0 || r[i].level == level) && (best < 0 || r[i].no > r[best].no))
            best = i;
    }
    if (best < 0)
        return 0;
    *out = r[best];
    return 1;
}

static unsigned int highest_no(unsigned int host)
{
    struct save_rec r;
    unsigned int n = 0;

    if (rec_read(host, 'a', &r) && r.no > n)
        n = r.no;
    if (rec_read(host, 'b', &r) && r.no > n)
        n = r.no;
    return n;
}

/* coop_progress(level, checkpoint): host has got this far */
static void l_coop_progress(void)
{
    char path[MAX_PATH], sect[16], val[16];

    ini_path(path);
    snprintf(sect, sizeof(sect), "h%08x", my_id);
    snprintf(val, sizeof(val), "%d", (int)num_param(1));
    WritePrivateProfileStringA(sect, "p_level", val, path);
    snprintf(val, sizeof(val), "%d", (int)num_param(2));
    WritePrivateProfileStringA(sect, "p_cp", val, path);
    snprintf(val, sizeof(val), "%u", highest_no(my_id));
    WritePrivateProfileStringA(sect, "p_after", val, path);
}

/* coop_progress_get() -> level, checkpoint, newest save number then; nil if none */
static void l_coop_progress_get(void)
{
    char path[MAX_PATH], sect[16];
    int level;

    ini_path(path);
    snprintf(sect, sizeof(sect), "h%08x", my_id);
    level = (int)GetPrivateProfileIntA(sect, "p_level", 0, path);
    if (level < 1)
        return;
    lua_pushnumber(level);
    lua_pushnumber((int)GetPrivateProfileIntA(sect, "p_cp", 0, path));
    lua_pushnumber((int)GetPrivateProfileIntA(sect, "p_after", 0, path));
}

/* ---- Lua ---- */

/* coop_id() -> this copy's player id; coop_host_id() -> the host's (ours on the host), nil
 * while unknown */
static void l_coop_id(void)
{
    lua_pushnumber(my_id);
}

static void l_coop_host_id(void)
{
    if (host_id())
        lua_pushnumber(host_id());
}

/* coop_save_file(no) -> the file save no goes to here */
static void l_coop_save_file(void)
{
    char file[32];
    unsigned int no = (unsigned int)num_param(1);

    snprintf(file, sizeof(file), "save\\c%04x%c.sav", my_id & 0xffff, (no & 1) ? 'a' : 'b');
    lua_pushstring(file);
}

/* coop_save_next() -> the number for the host's next save (after any it made before a restart) */
static void l_coop_save_next(void)
{
    lua_pushnumber(highest_no(my_id) + 1);
}

/* coop_saved(host, no, level, checkpoint, file): this game has written save no of that
 * host's session */
static void l_coop_saved(void)
{
    struct save_rec r;
    long len;
    unsigned int host = (unsigned int)num_param(1);

    r.no = (unsigned int)num_param(2);
    r.level = (int)num_param(3);
    r.cp = (int)num_param(4);
    snprintf(r.file, sizeof(r.file), "%s", luaL_check_lstr(5, &len));
    rec_write(host, &r);
    if (role_host)
        net_set_last_save(r.no);
    logmsg("co-op save %u written: level %d checkpoint %d, %s (host %08x)", r.no, r.level, r.cp, r.file, host);
}

/* coop_save_find(host, no, level) -> file, no, level, checkpoint of save no of that host's
 * session; no 0: the newest (in level, if level > 0); nil if there is none */
static void l_coop_save_find(void)
{
    struct save_rec r;

    if (!rec_find((unsigned int)num_param(1), (unsigned int)num_param(2), (int)num_param(3), &r))
        return;
    lua_pushstring(r.file);
    lua_pushnumber(r.no);
    lua_pushnumber(r.level);
    lua_pushnumber(r.cp);
}

/* coop_save_announce(no, level, checkpoint): host asks every client to make save no */
static void l_coop_save_announce(void)
{
    struct event e;

    event_init(&e, EV_SAVE);
    e.netid = (unsigned int)num_param(1);
    e.type = (int)num_param(2);
    e.damage = (short)num_param(3);
    e.extra[1] = my_id;
    event_send(&e, TO_ALL);
}

/* coop_all_ready() -> 1 if every connected player is in our level and epoch (a save now
 * catches them all) */
static void l_coop_all_ready(void)
{
    int s;

    for (s = 0; s < MAX_PLAYERS; s++)
        if (player_connected(s) && !player_in_level(s))
            return;
    lua_pushnumber(1);
}

/* coop_ready(): client has loaded a save: the host sends its world again (enemies, taken items) */
static void l_coop_ready(void)
{
    struct event e;

    if (role_host)
        return;
    event_init(&e, EV_READY);
    event_send(&e, 0);
}

/* coop_purge_puppets() -> how many puppets an instant load brought back were deleted */
static void l_coop_purge_puppets(void)
{
    lua_pushnumber(gob_purge_named("coop_p"));
}

/* coop_host_status() -> ms since the host was last heard, host's level (-1 unknown); nil on the host */
static void l_coop_host_status(void)
{
    if (role_host)
        return;
    lua_pushnumber(host_silent_ms());
    lua_pushnumber(host_level());
}

/* coop_time() -> seconds on a steady clock */
static void l_coop_time(void)
{
    lua_pushnumber(now_ms() / 1000.0);
}

/* coop_netblock(ms): tests: cut this game off the network for ms */
static void l_coop_netblock(void)
{
    net_block((int)num_param(1));
}

void saves_lua_init(void)
{
    tolua_function(NULL, "coop_id", l_coop_id);
    tolua_function(NULL, "coop_host_id", l_coop_host_id);
    tolua_function(NULL, "coop_save_file", l_coop_save_file);
    tolua_function(NULL, "coop_save_next", l_coop_save_next);
    tolua_function(NULL, "coop_saved", l_coop_saved);
    tolua_function(NULL, "coop_save_find", l_coop_save_find);
    tolua_function(NULL, "coop_save_announce", l_coop_save_announce);
    tolua_function(NULL, "coop_all_ready", l_coop_all_ready);
    tolua_function(NULL, "coop_ready", l_coop_ready);
    tolua_function(NULL, "coop_purge_puppets", l_coop_purge_puppets);
    tolua_function(NULL, "coop_host_status", l_coop_host_status);
    tolua_function(NULL, "coop_netblock", l_coop_netblock);
    tolua_function(NULL, "coop_time", l_coop_time);
    tolua_function(NULL, "coop_progress", l_coop_progress);
    tolua_function(NULL, "coop_progress_get", l_coop_progress_get);
}
