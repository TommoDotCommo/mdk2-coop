/*
 * mdk2-coop bridge: shared declarations.
 *
 * Addresses are for Steam build 3902 (mdk2hd.exe sha256 bf5faa81...7f43); see
 * games/mdk2-hd/NOTES.md in coop-anything for how each was found.
 */
#ifndef BRIDGE_H
#define BRIDGE_H

#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include "presence.h"

/* ---- engine addresses per game build ----
 * Steam build 3902 and the Overhaul Games (non-Steam) build share nearly all code at the same
 * addresses; the Lua runtime, a few list helpers and the data tables sit at different ones.
 * select_build() (dinput8.c) fills ADDR from the build it recognises. Addresses that are the
 * same in both builds stay literal in the files that use them. */
struct build_addrs {
    const char *name;
    DWORD builtin_entry, builtin_name;      /* table entry of the "copytagmethods" builtin and its name */
    DWORD lua_pushnumber, lua_pushstring, lua_pushnil, lua_getparam, lua_isnumber, lua_getnumber;
    DWORD luaL_check_lstr, lua_dobuffer, lua_pushusertag, tolua_tag, tolua_getusertype, tolua_function;
    DWORD update_scene, list_insert;
    DWORD class_table, gob_table, room_count, room_array, player_handle, main_scene;
};
extern struct build_addrs ADDR;
const char *build_name(void);

/* ---- game Lua 3.1 API (cdecl, statically linked in mdk2hd.exe) ---- */

typedef void (*lua_CFunction)(void);

#define lua_pushnumber      ((void (*)(double))ADDR.lua_pushnumber)
#define lua_pushstring      ((void (*)(const char *))ADDR.lua_pushstring)
#define lua_pushnil         ((void (*)(void))ADDR.lua_pushnil)
#define lua_getparam        ((unsigned int (*)(int))ADDR.lua_getparam)
#define lua_isnumber        ((int (*)(unsigned int))ADDR.lua_isnumber)
#define lua_getnumber       ((double (*)(unsigned int))ADDR.lua_getnumber)
/* luaL_check_lstr(narg, &len): string argument, raises a Lua error if missing */
#define luaL_check_lstr     ((const char *(*)(int, long *))ADDR.luaL_check_lstr)
/* lua_dobuffer(buf, len, chunkname): run Lua source, 0 on success */
#define lua_dobuffer        ((int (*)(const char *, int, const char *))ADDR.lua_dobuffer)
/* lua_pushusertag(ptr, tag): push a C object; tolua_tag("OMGOB") gives a gob's tag */
#define lua_pushusertag     ((void (*)(void *, int))ADDR.lua_pushusertag)
#define tolua_tag           ((int (*)(const char *))ADDR.tolua_tag)
/* tolua_getusertype(narg, def): C object behind a Lua argument */
#define tolua_getusertype   ((void *(*)(int, void *))ADDR.tolua_getusertype)
/* tolua_function(module, name, fn): module NULL registers a global */
#define tolua_function      ((void (*)(const char *, const char *, lua_CFunction))ADDR.tolua_function)

/* ---- engine ---- */

/* om anim state for (gob, anim id): 40-byte block, NULL if the model lacks it */
#define om_anim_state       ((DWORD *(*)(void *, int))0x44c640)
#define ANIM_ID_MAX         160     /* exe names 146 ANIM_* constants */
/* native behind mdkGobSetPositionXYZ (re-files the gob into the right room) */
#define gob_set_position    ((void (*)(void *, float, float, float))0x40dda0)
/* native behind omGobDelete */
#define gob_delete          ((void (*)(void *))0x449710)

/* OMGOB layout */
#define GOB_NAME            0x04    /* char[], name given to mdkCreateObjectLua / mdkRegisterObject */
#define GOB_POSITION        0x18    /* float x, y, z */
#define GOB_ORIENTATION     0x24    /* float quaternion w, x, y, z; engine setter copies it raw */
#define GOB_ANIM            0x60    /* anim controller: +0x24 states, +0x28/+0x2c current/previous */
#define GOB_INFO            0x84    /* -> game object: short type +0, damage filter +0x10, hp +0x12 */
#define GOB_TARGET          0x88    /* -> current target OMGOB (actor AI / shots) */
#define INFO_CLASS          0xb4    /* game object field; & 0x3f = index into CLASS_TABLE */
#define CLASS_TABLE         ((BYTE **)ADDR.class_table)
#define CLASS_SLOTS         14
#define CLASS_THINK         1       /* slot +0x4: per-frame AI/update, think(gob); +0x0 is used instead
                                       for the player-controlled gob (caller around 0x42b3f0) */
#define CLASS_ANIMEVENT     3       /* slot +0x0c: native animation keys (including strikes) */
#define CLASS_COLLISION     7       /* slot +0x1c: native gob collision callback */
#define CLASS_ANIMLOOP      11      /* slot +0x2c: animation looped, (gob, ?, ?) (caller 0x42b51e) */

/* Engine object table: GOB_SLOTS entries of { gob *, u16 generation, u16 pad };
 * a handle is (generation << 16) | slot (see 0x449560, mdkGetGob) */
#define GOB_TABLE           ((DWORD *)ADDR.gob_table)
#define GOB_SLOTS           0x800

/* Creatures and enemies: OBJ_BIF (200) .. OBJ_CONEHEADCIV5 (254) */
#define ACTOR_TYPE_MIN      200
#define ACTOR_TYPE_MAX      259
/* Shots: OBJ_BIFSHOT (400) .. OBJ_TURSHOT10 (459) */
#define PROJECTILE_TYPE_MIN 400
#define PROJECTILE_TYPE_MAX 499

/* ---- game.c ---- */

const char *gob_name(BYTE *gob);
void gob_netname(BYTE *gob, char *out, int size);
void gob_peername(BYTE *gob, char *out, int size);
BYTE *gob_find_peername(const char *name);
int gob_alive(BYTE *gob);
int gob_doomed(BYTE *gob);
void gob_kill(BYTE *gob);
int gob_type(BYTE *gob);
int gob_class(BYTE *gob);
int gob_hp(BYTE *gob);
int gob_max_hp(BYTE *gob);
void gob_set_max_hp(BYTE *gob, int hp);
int gob_is_actor(BYTE *gob);
int gob_is_projectile(BYTE *gob);
int gob_is_mirrored(BYTE *gob);
void gob_set_mirrored(BYTE *gob, int on);
int gob_is_foreign(BYTE *gob);
void gob_sets_clear(void);
int gob_is_own_shot(BYTE *gob);
void gob_set_own_shot(BYTE *gob, int on);
void gob_set_hp(BYTE *gob, int hp);
int gob_purge_named(const char *prefix);
void gob_set_foreign(BYTE *gob, int on);
int gob_slot(BYTE *gob);
unsigned int gob_handle(int slot);
BYTE *gob_by_handle(unsigned int handle);
void push_gob(BYTE *gob);
void class_filter_think(int cls);
void *hook_code(BYTE *fn, const BYTE *expect, int len, void *hook);

/* ---- net.c ---- */

#include "snapshot.h"

enum { EV_HIT = 1, EV_APPLY, EV_HURT, EV_SPAWN, EV_DESPAWN, EV_FIRE, EV_PICKUP, EV_LEVEL, EV_MARK, EV_STATE, EV_SAVE, EV_READY,
       EV_REVIVE, EV_RESPAWN, EV_DIED, EV_CHAT, EV_PING, EV_TYPING, EV_RULES, EV_MOVIE, EV_DIAG, EV_PUZZLE };
#define EV_NAME 32
/* FIRE part: 0 replays create_object arguments; 1 restores a launched class 5 shot. */
#define FIRE_LAUNCHED 1

struct event {
    unsigned int id;
    unsigned char kind;
    signed char part;
    short damage;
    int type;
    unsigned int netid;         /* SPAWN / DESPAWN: host handle of the gob; LEVEL: the new epoch */
    short objtype;              /* SPAWN / FIRE: OBJ_* type */
    unsigned short epoch;       /* sender's level epoch; events from another one are dropped */
    float pos[3];               /* SPAWN / FIRE: where it is */
    float quat[4];              /* FIRE: rotation */
    DWORD extra[4];             /* FIRE: create_object's last four arguments (launched: [0] owner);
                                   LEVEL: [0] co-op save to load (0 none), [1] host's player id */
    char name[EV_NAME];         /* target gob name; "@p<n>" = player n's character */
    union {
        char from[EV_NAME];     /* APPLY / HURT: attacker, "@p<n>" or gob name */
        struct {               /* FIRE_LAUNCHED: pointer-free class 5 state, keeping events 128 B */
            float velocity[3];
            DWORD state[5];    /* active, age, flags and timers (shot state +4..+0x14) */
        } shot;
    };
};

/* Players: slot 0 is the host, clients get 1.. from the host when they first send.
 * Logs and gob names use the player number, slot + 1 ("@p2", "coop_p2"). */
#define MAX_PLAYERS         16
#define PLAYER_NAME         16
#define TO_ALL              (-1)

extern int role_host;
extern unsigned int my_id;      /* this game copy's player id (coop\player.ini), kept across restarts */
extern char my_name[PLAYER_NAME];
extern int my_slot;             /* our slot; -1 on a client until the host has told us */
extern int cur_level;           /* level this game is in (coop_level) */
extern int cur_cp;
extern unsigned short cur_epoch;    /* level epoch: the host counts its announced level loads */
extern int have_remote;         /* some other player is connected */
extern int sync_anim;
extern int ent_hz;
extern int net_steam;           /* [net] transport=steam; absent means legacy UDP */
extern BYTE *local_player;
extern float local_pos[3];
extern BYTE *puppets[MAX_PLAYERS];  /* other players' puppets by slot (coop_face) */

double now_ms(void);
void net_open(const char *ini);
void net_poll(void);
int net_ready(void);
void net_steam_session(int host, uint64_t peer);
void net_steam_receive(const void *buf, int len, uint64_t peer);
void net_steam_left(uint64_t peer);
void net_send_player(BYTE *gob);
void net_level_reset(int announce);
void net_level_event(const struct event *e);
void net_send(const void *buf, int len);
void net_send_to(int slot, const void *buf, int len);
int player_connected(int slot);
int player_in_level(int slot);
const char *player_name(int slot);
int player_ping(int slot);
int player_netq(int slot, int *rtt, double *jitter, unsigned int *rx, unsigned int *lost);
int player_addr(int slot, char *out, int size);
void player_health(int slot, int *hp, int *hp_max);
extern int local_max_hp;
void net_set_name(const char *name);
int net_retarget(const char *host, int port);
unsigned int host_id(void);
double host_silent_ms(void);
int host_level(void);
int host_changed_take(void);
void net_block(int ms);
void net_set_last_save(unsigned int no);
int player_type(int slot);
int players_connected(void);
struct snap *player_view(int slot, int update);
void event_init(struct event *e, int kind);
void event_send(const struct event *e, int to);
void event_relay(const struct event *e, int from);
void event_queue(int kind, const char *name, int damage, int type, int part);
void event_queue_from(int to, int kind, const char *name, BYTE *from, int damage, int type, int part);
void event_queue_applied(const char *name, BYTE *from, int damage, int type, int part, int hp);
void event_queue_spawn(int to, int kind, unsigned int netid, int objtype, int hp, const float *pos, const char *name);
int event_next(struct event *out, int *from);
void event_unget(int from);
int event_must_wait(const struct event *e);
void snap_capture(BYTE *gob, struct snap *s);
int snap_encode(const struct snap *s, BYTE *out, int max);
int snap_decode(const BYTE *in, int len, struct snap *s);
void track_add(struct track *t, const struct snap *s);
void track_update(struct track *t, double clock_offset);
double host_clock_offset(void);
void anim_apply(BYTE *gob, const struct snap *s);
BYTE *canopy_gob(BYTE *gob);
void canopy_capture(BYTE *gob, struct canopy_state *s);
void canopy_apply(BYTE *gob, const struct canopy_state *s);
void presence_lua_init(void);
void presence_dump(FILE *f);
void ai_init(void);
void ai_before_scene(void);
void ai_create_enter(const float *pos);
void ai_create_leave(BYTE *gob, const float *pos);
void ai_hook_return(BYTE *gob, DWORD *ret_slot);
DWORD ai_leave(void);
DWORD ai_run(BYTE *gob, DWORD original, DWORD a, DWORD b, DWORD c);
int presence_damage_allowed(void *caller);
int presence_hit(BYTE *from, BYTE *target, int applied);
int net_join_cancelled(void);
void net_retry_join(void);
void net_test_legacy(int on);
int net_version_mismatch(void);

/* ---- game.c: players ---- */
int puppet_slot(BYTE *gob);
BYTE *puppet_nearest(const float *pos);

/* ---- entities.c ---- */

void ents_host_tick(void);
void ents_receive(const void *buf, int len);
void ents_client_frame(void);
void ents_client_cleanup(void);
void ent_bind(unsigned int netid, BYTE *gob);
void ent_despawn(unsigned int netid, int died);
void ents_init(void);
BYTE *ent_gob(unsigned int netid);
int ent_count(void);
void ents_reset(void);
void ents_resend(int to);
int ent_is_synced(BYTE *gob);
void ents_keep_spawner(BYTE *gob);
void ents_dump(FILE *f);
int ents_is_packet(const void *buf, int len);

/* ---- revive.c ---- */

enum { LIFE_ALIVE = 0, LIFE_DOWNED, LIFE_DEAD };
void revive_frame(void);
void revive_level_reset(void);
int revive_life(void);
int revive_reviving(void);
int revive_hit(int damage, int type);
void revive_unreachable(const char *why);
void revive_event(const struct event *e, int from);
void revive_checkpoint(void);
const char *revive_tick(void);
int revive_take_forced(void);
int player_life(int slot);
int player_reviving(int slot);

/* ---- emote.c ---- */

int emote_play(int id, int looping);
void emote_frame(void);
int emote_active(void);
void emote_lua_init(void);
int emote_wheel_info(int *n, int *sel, float *vx, float *vy, const char **labels, const char **mid);
int emote_wheel_open(void);

/* ---- input.c / hud.c / rooms.c / entities.c, for revive.c ---- */

int input_down(int code, int vk);
enum { GK_CHAT, GK_PING, GK_REVIVE, GK_VOICE, GK_EMOTE, GK_CHAR, GK_COUNT };    /* keys Lua sets: coop_game_keys */
int gamekey_down(int which);
int input_lua_key(int k, int *code, int *vk);
int gamekey_vk(int which);
void input_lua_init(void);
void hud_status(const char *line1, const char *line2, float progress);

/* ---- movie.c: cutscene events and banner ---- */
void movie_event(struct event *e, int from);
void movie_lua_init(void);
int rooms_in_zone(const float *zone_pos, const float *p);
int ents_alive_in_zone(const float *zone_pos);

/* ---- netstats.c ---- */

enum { NS_PLAYER, NS_RELAY, NS_ENT, NS_VOICE, NS_OTHER, NS_KINDS };
void netstats_sent(int kind, int len);

/* ---- steam.c: uses the game's Steam connection, never replaces steam_api.dll ---- */

void steam_open(const char *ini);
void steam_poll(void);
int steam_ready(void);
int steam_send(uint64_t peer, const void *buf, int len);
int steam_peer_allowed(uint64_t peer);
int steam_host_lobby(void);
int steam_join_lobby(const char *id);
int steam_leave_lobby(void);
int steam_invite(void);
const char *steam_status(void);
const char *steam_lobby_id(void);

/* ---- voice.c: proximity voice chat ---- */

void voice_init(const char *ini);
void voice_reload(const char *ini);
void voice_frame(void);
void voice_lua_init(void);
int voice_is_packet(const void *buf, int len);
void voice_receive(int from, const void *buf, int len);
int voice_talking(int slot);        /* slot -1: us */

/* ---- state.c ---- */

void state_dump(const char *tag);

/* ---- shots.c ---- */

void shots_init(void);
void shots_frame(void);
void shots_dump(void);
void shots_test_sniper(void);
BYTE *shots_test_latest(int type);
void shots_replay(const struct event *e);
void shots_reset(void);

/* ---- settings.c ---- */

extern int cfg_friendly_fire;   /* [game] friendly_fire: players' hits hurt other players */
extern int cfg_enemy_scale;     /* [game] enemy_scale: % more enemy hp per extra player (host) */
extern int cfg_enemy_hp;        /* [game] enemy_hp: % of an enemy's normal hp (host) */
/* The rules every player plays by: the host's [game] values, sent to the clients */
extern int rule_dmg_taken;      /* % of the damage the world deals to players */
extern int rule_dmg_dealt;      /* % of the damage players deal to the world */
extern int rule_down_secs;      /* bleed-out time while downed */
extern int rule_revive_secs;    /* revive key hold time */
int scale_damage(int damage, int percent);
void rules_fill(struct event *e);
void rules_event(const struct event *e);
void rules_broadcast(void);
void settings_start(void);
void settings_init(const char *ini);
void settings_lua_init(void);

/* ---- saves.c ---- */

void saves_lua_init(void);

/* A co-op session as saves.ini records it (saves.c; the launcher lists them too) */
#define SESSION_SECT 24
struct coop_session {
    char sect[SESSION_SECT];            /* section of saves.ini */
    unsigned int host, sid;
    int level, cp;                      /* how far it has got */
    long started, updated;              /* unix time */
    int peak, nmembers;                 /* most players at once; everyone who has played */
    char host_name[PLAYER_NAME], me[PLAYER_NAME];
    char members[MAX_PLAYERS][PLAYER_NAME];
};
int sessions_list(struct coop_session *out, int max, unsigned int host);
void session_remove(const char *sect);
void level_info_get(int l, const char **place, const char **time, const char **hero);

/* ---- hud.c ---- */

void hud_frame(HDC dc);
struct image { int w, h; DWORD *px; };      /* 0xAARRGGBB, top row first */
int art_font(struct image *img, float adv[256]);   /* the game's menu font (launcher.c); 0 without mdk2.tar */
void hud_font_use(int i);
int hud_font_h(void);
int hud_text_w(const char *s);
void hud_text(float x, float y, int h, const char *s, float r, float g, float b);
void hud_box(float x, float y, float w, float bh, int h, float r, float g, float b, float a);
int ctrlview_on(void);
void ctrlview_draw(int w, int h);
int guide_on(void);
void guide_draw(int w, int h);
void ctrlview_lua_init(void);
void hud_lua_init(void);
BYTE *hud_camera(void);

/* ---- chat.c: text chat and pings ---- */

void chat_frame(int allowed);
void chat_notice(const char *text);         /* a line from the game, no speaker */
void chat_event(struct event *e, int from);
int chat_is_open(void);
int chat_typing(int slot);
int chat_last(int slot, const char **text, double *age_ms);
const char *chat_input(void);
int chat_blocks_keys(void);
int chat_line(int i, int *slot, const char **text, double *age_ms);
int ping_get(int slot, float *pos, double *age_ms);
float ping_life_left(double age_ms);
void chat_lua_init(void);

/* ---- frame.c ---- */

void frame_init(void);
extern int emitter_guard;      /* frame.c: reset negative emitter ring indexes (coop_emitter_guard) */

/* ---- rooms.c ---- */

void rooms_init(void);
void rooms_level_reset(void);
void rooms_check_fall(void);

/* ---- doors.c ---- */

void doors_frame(void);
void doors_reset(void);

/* ---- pickups.c ---- */

void pickups_init(void);
void pickups_remove(const char *name, int from, int type);
void pickups_reset(void);
void pickups_resend(int to);

/* ---- damage (lua.c) ---- */

typedef void (*deal_damage_t)(void *, void *, int, int, int);
extern deal_damage_t deal_damage_orig;
void deal_damage_hook(void *from, void *target, int damage, int type, int part);

/* ---- input.c ---- */

extern int test_background;
extern int couch_active;
int input_couch(int *slot, int *count);
void input_pad_frame(void);
void input_init(void);
void input_wrap_dinput(void *di);
void input_press(int code, int hold_ms);
void input_mouse_hold(int on);                  /* emote wheel: the game gets no mouse movement while on */
void input_mouse_take(long *dx, long *dy);      /* and the movement held back since the last call */
void input_shot(const char *name);
struct route_input;
void input_capture(struct route_input *out);
void input_replay(const struct route_input *input);

/* ---- bot.c: recorded-route progression tests ---- */
void bot_lua_init(void);
void bot_before_frame(void);
void bot_level_loaded(int level);

/* ---- dinput8.c ---- */

/* coop_dir: player installs use %LOCALAPPDATA%\mdk2-coop\coop; test/couch copies use
 * <game dir>\coop when writable, falling back to the user profile otherwise.
 * game_dir: the game's own folder; coop.lua is always read from <game_dir>\coop. */
extern char coop_dir[MAX_PATH], game_dir[MAX_PATH];

/* ---- launcher.c ---- */

extern int coop_enabled;
void launcher_run(void);
void logmsg(const char *fmt, ...);

/* ---- remote.c: handlers for what happened to another player ---- */

void remote_queue(const char *name, const char *event, int slot, int arg, int kill);
void remote_reset(void);
void puzzle_resend(int to);
void puzzle_event(const struct event *e, int from);
void pickups_event(const struct event *e, int from);
int door_locked(BYTE *door);                /* doors.c */
void remote_lua_init(void);
void pickups_finish(const char *name);
void pickups_frame(void);                   /* pickups.c: pickups that arrived before their item did */      /* pickups.c: remove the item the peer picked up, after its handler ran */
void rooms_center(const char *name, float *out, int *found);
void rooms_remote_frame(void);              /* rooms.c: a teammate entered a room: run its OnEnterRoom here too */
int rooms_suppress_enter(BYTE *room);       /* rooms.c: the engine is about to run OnEnterRoom; a teammate already did */

/* ---- scripttrace.c: which script handlers ran ---- */

void script_trace(BYTE *gob, int event);    /* the engine asked has_lua_event(gob, event) and got yes */
void script_trace_reset(void);              /* per level */
void script_trace_dump(void);

/* ---- diag.c: session logs, flight recorder, desync check, diagnostics bundle ---- */

void diag_log_open(void);                   /* from DllMain, once bridge.log is open */
void diag_log_line(const char *text);       /* every logmsg line: session log and flight recorder */
void diag_log_close(void);
void diag_crash(void);                      /* the crash handler: marker file and a flight dump */
void diag_tick(void);                       /* per frame */
void diag_event(const struct event *e, int from);
void diag_mark(unsigned int n, int from);   /* an F2 mark: ours (from 0) or player `from`'s: save the last minute */
void diag_test(int skew, int deaf);          /* tests: the host reports skew more enemies; deaf: a client ignores the host's report */
int diag_bundle_start(void);                /* the launcher's button: 1 started, 0 could not */
int diag_bundle_poll(char *msg, int size);  /* 0 idle, 1 running, 2 done (msg: the zip), 3 failed (msg: why) */

#endif
