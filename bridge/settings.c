/*
 * Settings: coop.ini, read and written from the game's Lua (which has no file access) for
 * the Options > Co-op menu. Writes go through WritePrivateProfileString, which changes the
 * one key and keeps the rest of the file (the player installer writes the [net] keys).
 *
 *   [net]      role, listen_port, peer_host, peer_port   (role and ports apply at the next start)
 *   [game]     friendly_fire=0|1, enemy_scale=<% more enemy hp per extra player>  (the host's count),
 *              enemy_hp=<% of normal>, dmg_taken=<%>, dmg_dealt=<%>, down_secs, revive_secs
 *              (difficulty: the host's values rule; clients get them as EV_RULES)
 *   [voice]    enabled, key, volume, range   (voice.c; range is the host's)
 *   [save]     interval=<seconds between co-op saves>
 *   [controls] preset=original|modern|custom, devkeys=0|1;  [controls_custom] <action>=<key>,<key>
 *
 * The player name lives in coop\player.ini with the player id.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "bridge.h"

int cfg_friendly_fire;
int cfg_enemy_scale = 50;
int cfg_enemy_hp = 100;
int rule_dmg_taken = 100;
int rule_dmg_dealt = 100;
int rule_down_secs = 30;
int rule_revive_secs = 3;

static int clamp(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* a damage number scaled by a percentage; a hit that did anything still does at least 1 */
int scale_damage(int damage, int percent)
{
    int d;

    if (damage <= 0 || percent == 100)
        return damage;
    d = damage * percent / 100;
    return d < 1 && percent > 0 ? 1 : d;
}

static char ini_path[MAX_PATH];

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

static void settings_load(void)
{
    voice_reload(ini_path);
    cfg_friendly_fire = GetPrivateProfileIntA("game", "friendly_fire", 0, ini_path) != 0;
    cfg_enemy_scale = GetPrivateProfileIntA("game", "enemy_scale", 50, ini_path);
    if (cfg_enemy_scale < 0)
        cfg_enemy_scale = 0;
    cfg_enemy_hp = clamp(GetPrivateProfileIntA("game", "enemy_hp", 100, ini_path), 10, 1000);
    if (role_host) {
        rule_dmg_taken = clamp(GetPrivateProfileIntA("game", "dmg_taken", 100, ini_path), 0, 1000);
        rule_dmg_dealt = clamp(GetPrivateProfileIntA("game", "dmg_dealt", 100, ini_path), 10, 1000);
        rule_down_secs = clamp(GetPrivateProfileIntA("game", "down_secs", 30, ini_path), 5, 600);
        rule_revive_secs = clamp(GetPrivateProfileIntA("game", "revive_secs", 3, ini_path), 1, 30);
    }
}

/* the rules go out as an event: percentages in type, times in extra[0] */
void rules_fill(struct event *e)
{
    event_init(e, EV_RULES);
    e->type = rule_dmg_taken | (rule_dmg_dealt << 16);
    e->extra[0] = rule_down_secs | (rule_revive_secs << 16);
}

void rules_event(const struct event *e)
{
    if (role_host)
        return;
    rule_dmg_taken = clamp(e->type & 0xffff, 0, 1000);
    rule_dmg_dealt = clamp((e->type >> 16) & 0xffff, 10, 1000);
    rule_down_secs = clamp(e->extra[0] & 0xffff, 5, 600);
    rule_revive_secs = clamp((e->extra[0] >> 16) & 0xffff, 1, 30);
    logmsg("rules from the host: damage taken %d%%, dealt %d%%, down %d s, revive %d s", rule_dmg_taken,
           rule_dmg_dealt, rule_down_secs, rule_revive_secs);
}

void rules_broadcast(void)
{
    struct event e;

    if (!role_host)
        return;
    rules_fill(&e);
    event_send(&e, TO_ALL);
}

/* once the role is known (net_open): the host's own rules */
void settings_start(void)
{
    settings_load();
}

void settings_init(const char *ini)
{
    snprintf(ini_path, sizeof(ini_path), "%s", ini);
    settings_load();
    logmsg("settings: friendly fire %d, enemy hp +%d%% per extra player", cfg_friendly_fire, cfg_enemy_scale);
    logmsg("settings: enemy hp %d%%, damage taken %d%%, dealt %d%%, down %d s, revive %d s", cfg_enemy_hp,
           rule_dmg_taken, rule_dmg_dealt, rule_down_secs, rule_revive_secs);
}

/* coop_get(section, key, default) -> the value in coop.ini, else default */
static void l_coop_get(void)
{
    char val[128];
    char section[32], key[32];

    snprintf(section, sizeof(section), "%s", str_param(1));
    snprintf(key, sizeof(key), "%s", str_param(2));
    GetPrivateProfileStringA(section, key, str_param(3), val, sizeof(val), ini_path);
    lua_pushstring(val);
}

/* coop_set(section, key, value): write one key of coop.ini; [game] takes effect at once */
static void l_coop_set(void)
{
    char section[32], key[32], val[128];

    snprintf(section, sizeof(section), "%s", str_param(1));
    snprintf(key, sizeof(key), "%s", str_param(2));
    snprintf(val, sizeof(val), "%s", str_param(3));
    WritePrivateProfileStringA(section, key, val, ini_path);
    settings_load();
    if (!strcmp(section, "game"))
        rules_broadcast();
    logmsg("setting [%s] %s = %s", section, key, val);
}

/* coop_player_info(slot) -> name, ping ms (0 unknown) of that player (us included), or nil */
static void l_coop_player_info(void)
{
    int slot = (int)num_param(1);

    if (slot != my_slot && !player_connected(slot))
        return;
    lua_pushstring(player_name(slot));
    lua_pushnumber(player_ping(slot));
}

/* coop_name() -> our name; coop_set_name(name) changes it (the others see it at once) */
static void l_coop_name(void)
{
    lua_pushstring(my_name);
}

static void l_coop_set_name(void)
{
    net_set_name(str_param(1));
}

/* coop_set_host(address, port) -> 1 if a client now talks to the host there (also kept in
 * coop.ini), nil if the address is not an IPv4 number or this is the host */
static void l_coop_set_host(void)
{
    char host[64], port[16];

    snprintf(host, sizeof(host), "%s", str_param(1));
    snprintf(port, sizeof(port), "%d", (int)num_param(2));
    if (!net_retarget(host, atoi(port)))
        return;
    WritePrivateProfileStringA("net", "peer_host", host, ini_path);
    WritePrivateProfileStringA("net", "peer_port", port, ini_path);
    lua_pushnumber(1);
}

static void l_coop_transport(void) { lua_pushstring(net_steam ? "steam" : "udp"); }
static void l_coop_steam_status(void) { lua_pushstring(steam_status()); }
static void l_coop_steam_lobby(void) { lua_pushstring(steam_lobby_id()); }
static void l_coop_steam_join(void) { if (net_steam && steam_join_lobby(str_param(1))) lua_pushnumber(1); }
static void l_coop_steam_host(void) { if (net_steam && steam_host_lobby()) lua_pushnumber(1); }
static void l_coop_steam_leave(void) { if (net_steam && steam_leave_lobby()) lua_pushnumber(1); }
static void l_coop_steam_invite(void) { if (net_steam && steam_invite()) lua_pushnumber(1); }

void settings_lua_init(void)
{
    tolua_function(NULL, "coop_get", l_coop_get);
    tolua_function(NULL, "coop_set", l_coop_set);
    tolua_function(NULL, "coop_player_info", l_coop_player_info);
    tolua_function(NULL, "coop_name", l_coop_name);
    tolua_function(NULL, "coop_set_name", l_coop_set_name);
    tolua_function(NULL, "coop_set_host", l_coop_set_host);
    tolua_function(NULL, "coop_transport", l_coop_transport);
    tolua_function(NULL, "coop_steam_status", l_coop_steam_status);
    tolua_function(NULL, "coop_steam_lobby", l_coop_steam_lobby);
    tolua_function(NULL, "coop_steam_join", l_coop_steam_join);
    tolua_function(NULL, "coop_steam_host", l_coop_steam_host);
    tolua_function(NULL, "coop_steam_leave", l_coop_steam_leave);
    tolua_function(NULL, "coop_steam_invite", l_coop_steam_invite);
}
