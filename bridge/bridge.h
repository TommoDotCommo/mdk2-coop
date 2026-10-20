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

/* ---- game Lua 3.1 API (cdecl, statically linked in mdk2hd.exe) ---- */

typedef void (*lua_CFunction)(void);

#define lua_pushnumber      ((void (*)(double))0x476bb0)
#define lua_pushstring      ((void (*)(const char *))0x476c40)
#define lua_pushnil         ((void (*)(void))0x476b80)
#define lua_getparam        ((unsigned int (*)(int))0x476690)
#define lua_isnumber        ((int (*)(unsigned int))0x476a20)
#define lua_getnumber       ((double (*)(unsigned int))0x476a60)
/* luaL_check_lstr(narg, &len): string argument, raises a Lua error if missing */
#define luaL_check_lstr     ((const char *(*)(int, long *))0x478100)
/* lua_dobuffer(buf, len, chunkname): run Lua source, 0 on success */
#define lua_dobuffer        ((int (*)(const char *, int, const char *))0x478de0)
/* lua_pushusertag(ptr, tag): push a C object; tolua_tag("OMGOB") gives a gob's tag */
#define lua_pushusertag     ((void (*)(void *, int))0x476ce0)
#define tolua_tag           ((int (*)(const char *))0x477980)
/* tolua_getusertype(narg, def): C object behind a Lua argument */
#define tolua_getusertype   ((void *(*)(int, void *))0x475700)
/* tolua_function(module, name, fn): module NULL registers a global */
#define tolua_function      ((void (*)(const char *, const char *, lua_CFunction))0x4776b0)

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
#define INFO_CLASS          0xb4    /* game object field; & 0x3f = index into CLASS_TABLE */
#define CLASS_TABLE         ((BYTE **)0x4f64a8)
#define CLASS_SLOTS         14
#define CLASS_THINK         1       /* slot +0x4: per-frame AI/update (profiled on the Doganboy) */

/* Engine object table: GOB_SLOTS entries of { gob *, u16 generation, u16 pad };
 * a handle is (generation << 16) | slot (see 0x449560, mdkGetGob) */
#define GOB_TABLE           ((DWORD *)0x524e10)
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
int gob_type(BYTE *gob);
int gob_class(BYTE *gob);
int gob_hp(BYTE *gob);
int gob_is_actor(BYTE *gob);
int gob_slot(BYTE *gob);
unsigned int gob_handle(int slot);
BYTE *gob_by_handle(unsigned int handle);
void push_gob(BYTE *gob);
void class_skip_think(int cls);
void *hook5(BYTE *fn, const BYTE *expect, void *hook);

/* ---- net.c ---- */

#define ANIM_STATE_DWORDS   10      /* flags, mode, times, rate, weight: plain data, no pointers */
#define ANIM_TIME0          3       /* [3] and [4]: normalised play position, 0..1 */
#define ANIM_TIME1          4
#define ANIM_WEIGHT         6
#define SNAP_ANIMS          8
#define NO_ANIM             0xff

struct anim_entry {
    unsigned char id;
    DWORD state[ANIM_STATE_DWORDS];
};

/* One sampled state of a gob: what a snapshot carries and what the puppet shows */
struct snap {
    unsigned int time;          /* sender clock, ms */
    float pos[3];
    float quat[4];              /* w, x, y, z */
    unsigned char cur, prev, nanims;
    struct anim_entry anims[SNAP_ANIMS];
};

#define TRACK_SNAPS 16

struct track {
    struct snap snaps[TRACK_SNAPS];     /* ordered by sender time, oldest first */
    int n;
    struct snap view;                   /* interpolated state for this frame */
};

enum { EV_HIT = 1, EV_APPLY, EV_HURT, EV_SPAWN, EV_DESPAWN };
#define EV_NAME 32

struct event {
    unsigned int id;
    unsigned char kind;
    signed char part;
    short damage;
    int type;
    unsigned int netid;         /* SPAWN / DESPAWN: host handle of the gob */
    short objtype;              /* SPAWN: OBJ_* type */
    float pos[3];               /* SPAWN: where it is */
    char name[EV_NAME];         /* target gob name; "@player" = the receiver's own player */
};

extern int role_host;
extern int have_remote;
extern int sync_anim;
extern int ent_hz;
extern SOCKET sock;
extern BYTE *local_player;
extern BYTE *puppet;
extern struct track player_track;

double now_ms(void);
void net_open(const char *ini);
void net_poll(void);
void net_send_player(BYTE *gob);
void net_send(const void *buf, int len);
void event_queue(int kind, const char *name, int damage, int type, int part);
void event_queue_spawn(int kind, unsigned int netid, int objtype, const float *pos, const char *name);
int event_next(struct event *out);
void snap_capture(BYTE *gob, struct snap *s);
void track_add(struct track *t, const struct snap *s);
void track_update(struct track *t);
void anim_apply(BYTE *gob, const struct snap *s);
void smooth_track(const float *pos);

/* ---- entities.c ---- */

void ents_host_tick(void);
void ents_receive(const void *buf, int len);
void ents_client_frame(void);
void ents_client_cleanup(void);
void ent_bind(unsigned int netid, BYTE *gob);
void ent_despawn(unsigned int netid);
BYTE *ent_gob(unsigned int netid);
int ent_count(void);

/* ---- damage (lua.c) ---- */

typedef void (*deal_damage_t)(void *, void *, int, int, int);
extern deal_damage_t deal_damage_orig;
void deal_damage_hook(void *from, void *target, int damage, int type, int part);

/* ---- dinput8.c ---- */

extern char coop_dir[MAX_PATH];
void logmsg(const char *fmt, ...);

#endif
