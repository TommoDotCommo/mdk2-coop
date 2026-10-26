/*
 * Co-op saves. The host calls a save (coop.SaveNow in Lua: every few minutes and after
 * each checkpoint); every game in the level then makes a full instant save of its own
 * world (the game's quicksave: every gob, the Save table, its player's inventory and hp)
 * under the same number. Loading save N in every game brings the whole session back to
 * that moment, each player with their own things.
 *
 * A session is one co-op playthrough: the host starts one with each new game and its id
 * (sid, 1 to 1295) travels in the save event. Each game keeps a record of its sessions in
 * <game dir>\coop\saves.ini, one section per host and session (h<host id>_<sid>; sid 0, from
 * before sessions, is h<host id>), with the two newest saves in alternating slots by save
 * number (a, b: a save that dies half-written leaves the one before it), and what the launcher
 * and Load Game show about it: who has played in it (names), how many at most, when, and how
 * far it has got. Save files are save\c<id><sid><a|b>.sav in the game's save folder: the
 * game cuts the whole path after 15 characters, and every copy on one PC shares the folder, so
 * the name carries 2 base-36 digits of this copy's player id and 2 of the session. The newest
 * MAX_SESSIONS sessions are kept.
 *
 * The host also records how far the session has got (each level start and checkpoint, in
 * p_level / p_cp, with the newest save number at that moment in p_after): where the
 * game cannot save (Max's tutorial at the start of level 2), resuming the newest save would
 * go back a whole level, and the checkpoint is the better place to resume.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "bridge.h"

#define MAX_SESSIONS 12

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

static char b36(unsigned int v)
{
    return "0123456789abcdefghijklmnopqrstuvwxyz"[v % 36];
}

static void sect_name(char *sect, unsigned int host, unsigned int sid)
{
    if (sid)
        snprintf(sect, SESSION_SECT, "h%08x_%04x", host, sid & 0xffff);
    else
        snprintf(sect, SESSION_SECT, "h%08x", host);
}

static int ini_int(const char *sect, const char *key, int dflt)
{
    char path[MAX_PATH];

    ini_path(path);
    return (int)GetPrivateProfileIntA(sect, key, dflt, path);
}

static void ini_str(const char *sect, const char *key, char *out, int size)
{
    char path[MAX_PATH];

    ini_path(path);
    GetPrivateProfileStringA(sect, key, "", out, size, path);
}

static void ini_put(const char *sect, const char *key, const char *val)
{
    char path[MAX_PATH];

    ini_path(path);
    WritePrivateProfileStringA(sect, key, val, path);
}

static void ini_put_int(const char *sect, const char *key, long v)
{
    char val[24];

    snprintf(val, sizeof(val), "%ld", v);
    ini_put(sect, key, val);
}

static int rec_read(const char *sect, char slot, struct save_rec *r)
{
    char key[16];

    snprintf(key, sizeof(key), "%c_no", slot);
    r->no = (unsigned int)ini_int(sect, key, 0);
    snprintf(key, sizeof(key), "%c_level", slot);
    r->level = ini_int(sect, key, 0);
    snprintf(key, sizeof(key), "%c_cp", slot);
    r->cp = ini_int(sect, key, 0);
    snprintf(key, sizeof(key), "%c_file", slot);
    ini_str(sect, key, r->file, sizeof(r->file));
    return r->no > 0 && r->file[0];
}

static void rec_write(const char *sect, const struct save_rec *r)
{
    char slot = (r->no & 1) ? 'a' : 'b';
    char key[16];

    /* the number last: a record cut short by a crash has the old number and is never matched */
    snprintf(key, sizeof(key), "%c_no", slot);
    ini_put(sect, key, "0");
    snprintf(key, sizeof(key), "%c_level", slot);
    ini_put_int(sect, key, r->level);
    snprintf(key, sizeof(key), "%c_cp", slot);
    ini_put_int(sect, key, r->cp);
    snprintf(key, sizeof(key), "%c_file", slot);
    ini_put(sect, key, r->file);
    snprintf(key, sizeof(key), "%c_no", slot);
    ini_put_int(sect, key, r->no);
}

/* The newest record of a session (0 if none) */
static int rec_newest(const char *sect, struct save_rec *out)
{
    struct save_rec r[2];
    int have[2];

    have[0] = rec_read(sect, 'a', &r[0]);
    have[1] = rec_read(sect, 'b', &r[1]);
    if (!have[0] && !have[1])
        return 0;
    *out = (have[0] && (!have[1] || r[0].no > r[1].no)) ? r[0] : r[1];
    return 1;
}

/* The section names of saves.ini that are sessions (of host, if host is not 0) */
static int sections(unsigned int host, char names[][SESSION_SECT], int max)
{
    char path[MAX_PATH], *all, *p;
    int n = 0, size = 16384, len;

    ini_path(path);
    all = malloc(size);
    if (!all)
        return 0;
    len = (int)GetPrivateProfileSectionNamesA(all, size, path);
    for (p = all; len > 0 && *p && n < max; p += strlen(p) + 1) {
        unsigned int h = 0, sid = 0;
        int got = sscanf(p, "h%8x_%4x", &h, &sid);

        if (got < 1 || p[0] != 'h' || (host && h != host) || strlen(p) >= SESSION_SECT)
            continue;
        strcpy(names[n++], p);
    }
    free(all);
    return n;
}

/* The record of save no from host (any session); no 0: the newest of session sid, in level if level > 0 */
static int rec_find(unsigned int host, unsigned int no, unsigned int sid, int level, struct save_rec *out)
{
    struct save_rec r[2];
    char names[MAX_SESSIONS * 2][SESSION_SECT], one[SESSION_SECT];
    int n, i, k, found = 0;

    if (no) {
        n = sections(host, names, MAX_SESSIONS * 2);
    } else {
        sect_name(one, host, sid);
        strcpy(names[0], one);
        n = 1;
    }
    for (i = 0; i < n; i++) {
        for (k = 0; k < 2; k++) {
            if (!rec_read(names[i], k ? 'b' : 'a', &r[k]))
                continue;
            if (no && r[k].no == no) {
                *out = r[k];
                return 1;
            }
            if (!no && (level <= 0 || r[k].level == level) && (!found || r[k].no > out->no)) {
                *out = r[k];
                found = 1;
            }
        }
    }
    return found;
}

/* Highest save number any session of host has: numbers are never reused, so a number names
 * one save of one host */
static unsigned int highest_no(unsigned int host)
{
    char names[MAX_SESSIONS * 2][SESSION_SECT];
    struct save_rec r;
    unsigned int n = 0;
    int count = sections(host, names, MAX_SESSIONS * 2), i;

    for (i = 0; i < count; i++) {
        if (rec_read(names[i], 'a', &r) && r.no > n)
            n = r.no;
        if (rec_read(names[i], 'b', &r) && r.no > n)
            n = r.no;
    }
    return n;
}

/* Where session sect is up to: its newest save, unless the host's progress (a level start or
 * checkpoint) is newer than that save */
static void session_point(const char *sect, int *level, int *cp)
{
    struct save_rec r;
    int pl = ini_int(sect, "p_level", 0);

    *level = *cp = 0;
    if (rec_newest(sect, &r)) {
        *level = r.level;
        *cp = r.cp;
        if (pl > 0 && r.no <= (unsigned int)ini_int(sect, "p_after", 0)) {
            *level = pl;
            *cp = ini_int(sect, "p_cp", 0);
        }
    } else if (pl > 0) {
        *level = pl;
        *cp = ini_int(sect, "p_cp", 0);
    }
}

/* The session of host a section is */
static unsigned int sect_sid(const char *sect)
{
    unsigned int h = 0, sid = 0;

    sscanf(sect, "h%8x_%4x", &h, &sid);
    return sid;
}

static void load_session(const char *sect, unsigned int host, struct coop_session *s)
{
    char key[16];
    int i;

    memset(s, 0, sizeof(*s));
    snprintf(s->sect, sizeof(s->sect), "%s", sect);
    s->host = host;
    s->sid = sect_sid(sect);
    session_point(sect, &s->level, &s->cp);
    s->updated = ini_int(sect, "updated", 0);
    s->started = ini_int(sect, "started", 0);
    s->peak = ini_int(sect, "peak", 0);
    s->nmembers = ini_int(sect, "m_n", 0);
    if (s->nmembers > MAX_PLAYERS)
        s->nmembers = MAX_PLAYERS;
    ini_str(sect, "host_name", s->host_name, sizeof(s->host_name));
    ini_str(sect, "me", s->me, sizeof(s->me));
    for (i = 0; i < s->nmembers; i++) {
        snprintf(key, sizeof(key), "m%d", i);
        ini_str(sect, key, s->members[i], sizeof(s->members[i]));
    }
}

static void delete_session(const char *sect)
{
    char path[MAX_PATH], file[MAX_PATH], name[32];
    struct save_rec r;
    int k;

    for (k = 0; k < 2; k++) {
        if (rec_read(sect, k ? 'b' : 'a', &r)) {
            /* the saves are beside coop\ : <game dir>\save\ */
            snprintf(name, sizeof(name), "%s", r.file);
            snprintf(file, sizeof(file), "%s\\%s", game_dir, name);
            DeleteFileA(file);
        }
    }
    ini_path(path);
    WritePrivateProfileStringA(sect, NULL, NULL, path);
}

/* The sessions this game knows, newest first (host != 0: that host's only). Returns the count. */
int sessions_list(struct coop_session *out, int max, unsigned int host)
{
    char names[MAX_SESSIONS * 2][SESSION_SECT];
    struct coop_session *tmp;
    int n = sections(host, names, MAX_SESSIONS * 2), i, j, count = 0;

    tmp = malloc(sizeof(*tmp) * (n ? n : 1));
    if (!tmp)
        return 0;
    for (i = 0; i < n; i++) {
        unsigned int h = 0;

        sscanf(names[i], "h%8x", &h);
        load_session(names[i], h, &tmp[count]);
        if (tmp[count].level > 0)
            count++;
    }
    for (i = 1; i < count; i++) {           /* newest first, insertion sort */
        struct coop_session s = tmp[i];

        for (j = i - 1; j >= 0 && tmp[j].updated < s.updated; j--)
            tmp[j + 1] = tmp[j];
        tmp[j + 1] = s;
    }
    for (i = 0; i < count && i < max; i++)
        out[i] = tmp[i];
    free(tmp);
    return i;
}

void session_remove(const char *sect)
{
    if (sect[0] == 'h')
        delete_session(sect);
}

/* This game has written a save of session sid: remember who is playing and when */
static void session_note(unsigned int host, unsigned int sid)
{
    char sect[SESSION_SECT], key[16], names[MAX_PLAYERS][PLAYER_NAME];
    int i, s, count, present = 0;

    sect_name(sect, host, sid);
    count = ini_int(sect, "m_n", 0);
    if (count > MAX_PLAYERS)
        count = MAX_PLAYERS;
    for (i = 0; i < count; i++) {
        snprintf(key, sizeof(key), "m%d", i);
        ini_str(sect, key, names[i], sizeof(names[i]));
    }
    for (s = 0; s < MAX_PLAYERS; s++) {
        const char *name = player_name(s);

        if (!name[0])
            continue;
        present++;
        for (i = 0; i < count && strcmp(names[i], name); i++)
            ;
        if (i == count && count < MAX_PLAYERS) {
            snprintf(names[count], PLAYER_NAME, "%s", name);
            snprintf(key, sizeof(key), "m%d", count);
            ini_put(sect, key, name);
            count++;
            ini_put_int(sect, "m_n", count);
        }
    }
    if (present > ini_int(sect, "peak", 0))
        ini_put_int(sect, "peak", present);
    if (!ini_int(sect, "started", 0))
        ini_put_int(sect, "started", (long)time(NULL));
    ini_put_int(sect, "updated", (long)time(NULL));
    ini_put(sect, "host_name", role_host ? my_name : player_name(0));
    ini_put(sect, "me", my_name);
}

/* Keep the newest MAX_SESSIONS sessions, and never the one that was just written */
static void sessions_prune(const char *keep)
{
    struct coop_session list[MAX_SESSIONS * 2];
    int n = sessions_list(list, MAX_SESSIONS * 2, 0), i;

    for (i = MAX_SESSIONS; i < n; i++)
        if (strcmp(list[i].sect, keep))
            delete_session(list[i].sect);
}

/* coop_progress(level, checkpoint, sid): host has got this far in session sid */
static void l_coop_progress(void)
{
    char sect[SESSION_SECT];

    if (!(unsigned int)num_param(3))        /* no session (yet): the level load that starts one comes next */
        return;
    sect_name(sect, my_id, (unsigned int)num_param(3));
    ini_put_int(sect, "p_level", (int)num_param(1));
    ini_put_int(sect, "p_cp", (int)num_param(2));
    ini_put_int(sect, "p_after", highest_no(my_id));
    ini_put_int(sect, "updated", (long)time(NULL));
}

/* coop_progress_get(sid) -> level, checkpoint, newest save number then; nil if none */
static void l_coop_progress_get(void)
{
    char sect[SESSION_SECT];
    int level;

    sect_name(sect, my_id, (unsigned int)num_param(1));
    level = ini_int(sect, "p_level", 0);
    if (level < 1)
        return;
    lua_pushnumber(level);
    lua_pushnumber(ini_int(sect, "p_cp", 0));
    lua_pushnumber(ini_int(sect, "p_after", 0));
}

/* coop_level_info(level) -> place, time, character of that level ("" when unknown) */
static void l_coop_level_info(void)
{
    const char *place, *time, *hero;

    level_info_get((int)num_param(1), &place, &time, &hero);
    lua_pushstring((char *)place);
    lua_pushstring((char *)time);
    lua_pushstring((char *)hero);
}

/* coop_session_new() -> the id of a new session of this host (1 to 1295, not one in use) */
static void l_coop_session_new(void)
{
    char sect[SESSION_SECT];
    unsigned int sid = (GetTickCount() ^ (my_id * 2654435761u) ^ (unsigned int)time(NULL)) % 1295;

    do {
        sid = sid % 1295 + 1;
        sect_name(sect, my_id, sid);
    } while (ini_int(sect, "p_level", 0) || ini_int(sect, "a_no", 0) || ini_int(sect, "b_no", 0));
    lua_pushnumber(sid);
}

/* coop_session_info(i) -> sid, level, checkpoint, updated, players (most at once), names
 * (comma separated), of the i-th newest session this game hosted (1 = newest); nil past the end */
static void l_coop_session_info(void)
{
    struct coop_session list[MAX_SESSIONS * 2];
    char names[MAX_PLAYERS * (PLAYER_NAME + 2)];
    int n = sessions_list(list, MAX_SESSIONS * 2, my_id), i = (int)num_param(1) - 1, k;

    if (i < 0 || i >= n)
        return;
    names[0] = 0;
    for (k = 0; k < list[i].nmembers; k++) {
        if (k)
            strcat(names, ", ");
        strcat(names, list[i].members[k]);
    }
    lua_pushnumber(list[i].sid);
    lua_pushnumber(list[i].level);
    lua_pushnumber(list[i].cp);
    lua_pushnumber(list[i].updated);
    lua_pushnumber(list[i].peak);
    lua_pushstring(names);
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

/* coop_save_file(no, sid) -> the file save no of session sid goes to here */
static void l_coop_save_file(void)
{
    char file[32];
    unsigned int no = (unsigned int)num_param(1);

    unsigned int id = my_id % 1296, sid = (unsigned int)num_param(2) % 1296;

    snprintf(file, sizeof(file), "save\\c%c%c%c%c%c.sav", b36(id / 36), b36(id), b36(sid / 36), b36(sid),
             (no & 1) ? 'a' : 'b');
    lua_pushstring(file);
}

/* coop_save_next() -> the number for the host's next save (after any it made before a restart) */
static void l_coop_save_next(void)
{
    lua_pushnumber(highest_no(my_id) + 1);
}

/* coop_saved(host, no, level, checkpoint, file, sid): this game has written save no of that
 * host's session sid */
static void l_coop_saved(void)
{
    struct save_rec r;
    long len;
    unsigned int host = (unsigned int)num_param(1), sid = (unsigned int)num_param(6);
    char sect[SESSION_SECT];

    r.no = (unsigned int)num_param(2);
    r.level = (int)num_param(3);
    r.cp = (int)num_param(4);
    snprintf(r.file, sizeof(r.file), "%s", luaL_check_lstr(5, &len));
    sect_name(sect, host, sid);
    rec_write(sect, &r);
    session_note(host, sid);
    sessions_prune(sect);
    if (role_host)
        net_set_last_save(r.no);
    logmsg("co-op save %u written: level %d checkpoint %d, %s (host %08x)", r.no, r.level, r.cp, r.file, host);
}

/* coop_save_find(host, no, level, sid) -> file, no, level, checkpoint of save no of that host
 * (any session); no 0: the newest of session sid (in level, if level > 0); nil if there is none */
static void l_coop_save_find(void)
{
    struct save_rec r;

    if (!rec_find((unsigned int)num_param(1), (unsigned int)num_param(2), (unsigned int)num_param(4),
                  (int)num_param(3), &r))
        return;
    lua_pushstring(r.file);
    lua_pushnumber(r.no);
    lua_pushnumber(r.level);
    lua_pushnumber(r.cp);
}

/* coop_save_announce(no, level, checkpoint, sid): host asks every client to make save no of session sid */
static void l_coop_save_announce(void)
{
    struct event e;

    event_init(&e, EV_SAVE);
    e.netid = (unsigned int)num_param(1);
    e.type = (int)num_param(2);
    e.damage = (short)num_param(3);
    e.extra[0] = (unsigned int)num_param(4);
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
    tolua_function(NULL, "coop_session_new", l_coop_session_new);
    tolua_function(NULL, "coop_level_info", l_coop_level_info);
    tolua_function(NULL, "coop_session_info", l_coop_session_info);
}
