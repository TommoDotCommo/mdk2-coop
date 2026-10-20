/*
 * Lua functions the bridge adds to the game, and the damage hook.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "bridge.h"

/* Central damage function: every hit, from scripts and from shots, goes through it.
 * deal_damage(from, target, damage, damagetype, part); first 5 bytes are
 * push ebp; mov ebp,[esp+0xc], a clean place for a jump. */
#define DEAL_DAMAGE_ADDR    0x40d590
static const BYTE deal_damage_prologue[5] = { 0x55, 0x8b, 0x6c, 0x24, 0x0c };
deal_damage_t deal_damage_orig;     /* trampoline into the original */
static int applying_hit;            /* host: inside EV_HIT, damage from the puppet is real */

static double num_param(int n)
{
    unsigned int o = lua_getparam(n);
    return (o && lua_isnumber(o)) ? lua_getnumber(o) : 0.0;
}

/* ---- damage ---- */

/* Client: is this attacker one of the host's (an actor or a projectile)? */
static int from_host_world(BYTE *from)
{
    int t;

    if (!from || IsBadReadPtr(from + GOB_INFO, 4) || !*(BYTE **)(from + GOB_INFO))
        return 0;
    t = gob_type(from);
    return (t >= ACTOR_TYPE_MIN && t <= ACTOR_TYPE_MAX) || (t >= PROJECTILE_TYPE_MIN && t <= PROJECTILE_TYPE_MAX);
}

/* Host: applies everything, tells the client what it applied, and turns hits on the
 * client's puppet into HURT for the client's own player.
 * Client: only its own player takes damage locally; its hits on anything else go to
 * the host as HIT and come back as APPLY. */
void deal_damage_hook(void *from, void *target, int damage, int type, int part)
{
    char fromname[EV_NAME], name[EV_NAME];

    gob_netname(from, fromname, sizeof(fromname));
    gob_netname(target, name, sizeof(name));
    logmsg("damage %s -> %s %d type 0x%x part %d", fromname, name, damage, type, part);

    if (sock == INVALID_SOCKET || !have_remote) {
        deal_damage_orig(from, target, damage, type, part);
        return;
    }
    if (role_host) {
        /* the puppet's own attacks are replays of the client's (shots, anims); the client
         * sends its real hits as HIT */
        if ((from == puppet && !applying_hit) || gob_is_foreign(from))
            return;
        if (target == puppet) {
            event_queue(EV_HURT, "@player", damage, type, part);
            return;
        }
        deal_damage_orig(from, target, damage, type, part);
        if (target != local_player && name[0])
            event_queue(EV_APPLY, name, damage, type, part);
        return;
    }
    if (target == local_player) {
        /* enemies and their shots are mirrors of the host's; the host sends their hits as HURT */
        if (from_host_world(from))
            return;
        deal_damage_orig(from, target, damage, type, part);
        return;
    }
    /* our gun, or a shot we fired (grenades, sniper rounds); mirrored shots are the host's */
    if ((from == local_player || (gob_is_projectile(from) && !gob_is_foreign(from))) && target != puppet && name[0])
        event_queue(EV_HIT, name, damage, type, part);
}

/* ---- per frame ---- */

/* coop_send(gob): send our player (send_hz) and, on the host, the actors (ent_hz) */
static void l_coop_send(void)
{
    BYTE *gob = tolua_getusertype(1, NULL);

    if (!gob)
        return;
    local_player = gob;
    net_send_player(gob);
    if (role_host)
        ents_host_tick();
}

/* coop_recv() -> x, y, z of the peer for this frame, or nil before the first packet.
 * On the client this also moves the mirrored actors. */
static void l_coop_recv(void)
{
    net_poll();
    if (!have_remote)
        return;
    if (!role_host) {
        ents_client_frame();
        ents_client_cleanup();
    }
    shots_frame();
    track_update(&player_track);
    smooth_track(player_track.view.pos);
    lua_pushnumber(player_track.view.pos[0]);
    lua_pushnumber(player_track.view.pos[1]);
    lua_pushnumber(player_track.view.pos[2]);
}

/* coop_face(gob): give the gob the peer's orientation and animation for this frame */
static void l_coop_face(void)
{
    BYTE *gob = tolua_getusertype(1, NULL);

    if (!gob || !have_remote)
        return;
    puppet = gob;
    memcpy(gob + GOB_ORIENTATION, player_track.view.quat, sizeof(player_track.view.quat));
    if (sync_anim)
        anim_apply(gob, &player_track.view);
}

/* ---- events ---- */

/* coop_event() -> kind, name, damage, type, part, netid, objtype, x, y, z
 * for the next event from the peer, or nil. DESPAWN, FIRE and PICKUP are handled here. */
static void l_coop_event(void)
{
    struct event e;

    for (;;) {
        if (!event_next(&e))
            return;
        if (e.kind == EV_DESPAWN)
            ent_despawn(e.netid);
        else if (e.kind == EV_FIRE)
            shots_replay(&e);
        else if (e.kind == EV_PICKUP)
            pickups_remove(e.name);
        else
            break;
    }
    lua_pushnumber(e.kind);
    lua_pushstring(e.name);
    lua_pushnumber(e.damage);
    lua_pushnumber(e.type);
    lua_pushnumber(e.part);
    lua_pushnumber(e.netid);
    lua_pushnumber(e.objtype);
    lua_pushnumber(e.pos[0]);
    lua_pushnumber(e.pos[1]);
    lua_pushnumber(e.pos[2]);
}

/* coop_apply(kind, gob, damage, type, part) carries out a damage event on the gob Lua found;
 * coop_apply(EV_SPAWN, gob, netid) binds the gob to the host's actor */
static void l_coop_apply(void)
{
    int kind = (int)num_param(1);
    BYTE *gob = tolua_getusertype(2, NULL);
    int damage = (int)num_param(3), type = (int)num_param(4), part = (int)num_param(5);

    if (!gob)
        return;
    switch (kind) {
    case EV_HIT:        /* host: the client's Kurt hit this; goes through the hook so the client gets APPLY */
        applying_hit = 1;
        deal_damage_hook(puppet, gob, damage, type, part);
        applying_hit = 0;
        break;
    case EV_APPLY:      /* client: the host applied this */
    case EV_HURT:       /* client: the host's world hurt our player */
        deal_damage_orig(NULL, gob, damage, type, part);
        break;
    case EV_SPAWN:
        ent_bind((unsigned int)num_param(3), gob);
        return;
    }
    logmsg("event %d on %s: %d type 0x%x part %d -> hp %d", kind, gob_name(gob), damage, type, part, gob_hp(gob));
}

/* coop_gob(name) -> gob for an "n<id>" network name, or nil. Host: the gob with that
 * handle; client: the gob bound to that host actor. */
static void l_coop_gob(void)
{
    long len;
    const char *name = luaL_check_lstr(1, &len);
    unsigned int id;
    char *end;

    if (name[0] != 'n') {
        lua_pushnil();
        return;
    }
    id = strtoul(name + 1, &end, 16);
    if (*end || end == name + 1) {
        lua_pushnil();
        return;
    }
    push_gob(role_host ? gob_by_handle(id) : ent_gob(id));
}

/* coop_role() -> "host" or "client" */
static void l_coop_role(void)
{
    lua_pushstring(role_host ? "host" : "client");
}

/* ---- dev tools ---- */

/* coop_animdump(gob): log position and active anim states */
static void l_coop_animdump(void)
{
    BYTE *gob = tolua_getusertype(1, NULL);
    BYTE *ctl;
    int id, i;

    if (!gob || !(ctl = *(BYTE **)(gob + GOB_ANIM)))
        return;
    logmsg("animdump %s pos (%.2f, %.2f, %.2f) cur=%p prev=%p", gob_name(gob),
           ((float *)(gob + GOB_POSITION))[0], ((float *)(gob + GOB_POSITION))[1],
           ((float *)(gob + GOB_POSITION))[2], *(void **)(ctl + 0x28), *(void **)(ctl + 0x2c));
    for (id = 0; id < ANIM_ID_MAX; id++) {
        DWORD *st = om_anim_state(gob, id);
        char line[256];
        int n;

        if (!st || !st[0])
            continue;
        n = snprintf(line, sizeof(line), "  anim %3d:", id);
        for (i = 0; i < ANIM_STATE_DWORDS; i++)
            n += snprintf(line + n, sizeof(line) - n, " %08lx", (unsigned long)st[i]);
        logmsg("%s", line);
    }
}

/* coop_listgobs(): log every live gob that has a game object (type, class, hp, position) */
static void l_coop_listgobs(void)
{
    int i, n = 0;

    for (i = 0; i < GOB_SLOTS; i++) {
        BYTE *gob = (BYTE *)GOB_TABLE[i * 2];
        float *pos;
        if (!gob || !*(BYTE **)(gob + GOB_INFO))
            continue;
        pos = (float *)(gob + GOB_POSITION);
        logmsg("  gob[%d] %-20s type %4d class %2d hp %4d at (%.0f, %.0f, %.0f)", i, gob_name(gob), gob_type(gob),
               gob_class(gob), gob_hp(gob), pos[0], pos[1], pos[2]);
        n++;
    }
    logmsg("listgobs: %d gobs with game objects, %d synced actors", n, ent_count());
}

/* coop_log(msg): write a line to bridge.log */
static void l_coop_log(void)
{
    long len;
    logmsg("lua: %s", luaL_check_lstr(1, &len));
}

/* ---- startup ---- */

/* Run <game dir>\coop\coop.lua. The game reads settings.lua into an 8 KB stack
 * buffer (overflowing it kills the game), so settings.lua only calls coop_init()
 * and the mod's Lua lives here. */
static void run_mod_script(void)
{
    char path[MAX_PATH], *buf;
    FILE *f;
    long len;

    snprintf(path, MAX_PATH, "%s\\coop.lua", coop_dir);
    if (!(f = fopen(path, "rb"))) {
        logmsg("no %s", path);
        return;
    }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc(len + 1);
    if (buf && fread(buf, 1, len, f) == (size_t)len) {
        buf[len] = 0;
        logmsg("running coop.lua (%ld bytes): %d", len, lua_dobuffer(buf, (int)len, "coop.lua"));
    }
    free(buf);
    fclose(f);
}

/* coop_init(): called once from Lua; registers the rest, opens the socket, runs coop.lua */
void l_coop_init(void)
{
    static int done;
    char ini[MAX_PATH];

    if (done)
        return;
    done = 1;
    tolua_function(NULL, "coop_send", l_coop_send);
    tolua_function(NULL, "coop_recv", l_coop_recv);
    tolua_function(NULL, "coop_face", l_coop_face);
    tolua_function(NULL, "coop_event", l_coop_event);
    tolua_function(NULL, "coop_apply", l_coop_apply);
    tolua_function(NULL, "coop_gob", l_coop_gob);
    tolua_function(NULL, "coop_role", l_coop_role);
    tolua_function(NULL, "coop_animdump", l_coop_animdump);
    tolua_function(NULL, "coop_listgobs", l_coop_listgobs);
    tolua_function(NULL, "coop_log", l_coop_log);
    snprintf(ini, MAX_PATH, "%s\\coop.ini", coop_dir);
    net_open(ini);
    deal_damage_orig = hook_code((BYTE *)DEAL_DAMAGE_ADDR, deal_damage_prologue, 5, deal_damage_hook);
    logmsg("coop_init: lua functions registered, damage hook %s", deal_damage_orig ? "installed" : "NOT installed");
    shots_init();
    pickups_init();
    run_mod_script();
}
