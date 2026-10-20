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
static int max_hp;                  /* our player's highest hp seen this level: revive to this */
static char apply_from[EV_NAME];    /* attacker named in the event coop_event last returned */
static int applying_hit;            /* host: inside EV_HIT, damage from the puppet is real */

static double num_param(int n)
{
    unsigned int o = lua_getparam(n);
    return (o && lua_isnumber(o)) ? lua_getnumber(o) : 0.0;
}

/* ---- damage ---- */

/* Damage to our own player. With a partner, a hit that would kill revives us at the
 * partner instead: dying would reload the checkpoint in this game only. */
static void damage_local_player(void *from, int damage, int type, int part)
{
    int hp = gob_hp(local_player);
    BYTE *info;

    if (damage >= hp && hp > 0 && (!have_remote || !puppet))
        logmsg("lethal hit, no revive: remote %d puppet %p", have_remote, (void *)puppet);
    if (!have_remote || !puppet || hp <= 0 || damage < hp) {
        deal_damage_orig(from, local_player, damage, type, part);
        return;
    }
    /* none of the hit is applied: some damage types start the death sequence whatever the
     * hp (lava, 0x40, still reloaded the checkpoint when 99 of 500 got through) */
    info = *(BYTE **)(local_player + GOB_INFO);
    if (info && max_hp > 0)
        *(short *)(info + 0x12) = (short)max_hp;
    gob_set_position(local_player, ((float *)(puppet + GOB_POSITION))[0], ((float *)(puppet + GOB_POSITION))[1],
                     ((float *)(puppet + GOB_POSITION))[2]);
    logmsg("down: %d damage (type 0x%x) on %d hp; revived at the partner with %d hp", damage, type, hp, max_hp);
}

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
            event_queue_from(EV_HURT, "@player", from, damage, type, part);
            return;
        }
        if (target == local_player) {
            damage_local_player(from, damage, type, part);
            return;
        }
        deal_damage_orig(from, target, damage, type, part);
        if (target != local_player && name[0])
            event_queue_from(EV_APPLY, name, from, damage, type, part);
        if (name[0] && gob_is_actor(target))
            logmsg("hp %s %d", name, gob_hp(target));
        return;
    }
    if (target == local_player) {
        /* enemies and their shots are mirrors of the host's; the host sends their hits as HURT */
        if (from_host_world(from))
            return;
        damage_local_player(from, damage, type, part);
        return;
    }
    /* our gun, or a shot we fired (grenades, sniper rounds); mirrored shots are the host's */
    if ((from == local_player || (gob_is_projectile(from) && !gob_is_foreign(from))) && target != puppet && name[0])
        event_queue(EV_HIT, name, damage, type, part);
}

/* ---- per frame ---- */

/* coop_send(gob or nil): send our player (or, with no player yet, just events) at
 * send_hz and, on the host, the actors at ent_hz */
static void l_coop_send(void)
{
    BYTE *gob = tolua_getusertype(1, NULL);
    int hp;

    local_player = gob;
    if (gob && (hp = gob_hp(gob)) > max_hp)
        max_hp = hp;
    net_send_player(gob);
    if (gob)
        rooms_check_fall();
    if (gob && role_host)
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
    doors_frame();
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
    if (puppet != gob) {
        /* the puppet is driven from the partner's game: skip its class logic like a mirror */
        gob_set_mirrored(gob, 1);
        class_filter_think(gob_class(gob));
    }
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
        else if (e.kind == EV_MARK)
            logmsg("lua: MARK %d (from the partner)", e.damage);
        else if (e.level != (unsigned char)cur_level && e.kind != EV_LEVEL)
            logmsg("event %d %s from level %d dropped (we are in %d)", e.kind, e.name, e.level, cur_level);
        else if (e.kind == EV_PICKUP)
            pickups_remove(e.name);
        else
            break;
    }
    memcpy(apply_from, e.from, sizeof(apply_from));
    apply_from[EV_NAME - 1] = 0;
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

/* The attacker of the event being applied: what the host named, else the puppet (some
 * damage handlers look at the attacker and crash on NULL) */
static BYTE *apply_attacker(void)
{
    BYTE *from = gob_find_peername(apply_from);

    if (!gob_alive(from))
        from = gob_alive(puppet) ? puppet : local_player;
    return from;
}

/* coop_apply(kind, gob, damage, type, part) carries out a damage event on the gob Lua found;
 * coop_apply(EV_SPAWN, gob, netid) binds the gob to the host's actor */
static void l_coop_apply(void)
{
    int kind = (int)num_param(1);
    BYTE *gob = tolua_getusertype(2, NULL);
    int damage = (int)num_param(3), type = (int)num_param(4), part = (int)num_param(5);

    if (!gob_alive(gob)) {
        logmsg("event %d: gob %p is gone", kind, (void *)gob);
        return;
    }
    switch (kind) {
    case EV_HIT:        /* host: the client's Kurt hit this; goes through the hook so the client gets APPLY */
        applying_hit = 1;
        deal_damage_hook(puppet, gob, damage, type, part);
        applying_hit = 0;
        break;
    case EV_APPLY:      /* client: the host applied this */
        deal_damage_orig(apply_attacker(), gob, damage, type, part);
        break;
    case EV_HURT:       /* client: the host's world hurt our player */
        if (gob == local_player)
            damage_local_player(apply_attacker(), damage, type, part);
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

/* coop_level(level, checkpoint, announce): a level has started here. Resets everything
 * that pointed into the old level; with announce, asks the peer to load it too. */
static void l_coop_level(void)
{
    int level = (int)num_param(1), cp = (int)num_param(2), announce = (int)num_param(3);

    cur_level = level;
    ents_reset();
    shots_reset();
    doors_reset();
    rooms_level_reset();
    net_level_reset();
    puppet = NULL;
    local_player = NULL;
    max_hp = 0;
    if (announce)
        event_queue(EV_LEVEL, "", cp, level, 0);
    logmsg("level %d checkpoint %d started%s", level, cp, announce ? ", peer asked to follow" : "");
}

/* coop_nearest_enemy() -> the actor nearest our player (on the client: a mirrored one), or nil */
static void l_coop_nearest_enemy(void)
{
    BYTE *best = NULL;
    float bestd = 0.0f, *me;
    int i;

    if (!gob_alive(local_player)) {
        lua_pushnil();
        return;
    }
    me = (float *)(local_player + GOB_POSITION);
    for (i = 0; i < GOB_SLOTS; i++) {
        BYTE *g = (BYTE *)GOB_TABLE[i * 2];
        float *p, d;
        if (!g || !gob_is_actor(g) || gob_hp(g) <= 0 || (!role_host && !gob_is_mirrored(g)))
            continue;
        p = (float *)(g + GOB_POSITION);
        d = (p[0] - me[0]) * (p[0] - me[0]) + (p[1] - me[1]) * (p[1] - me[1]) + (p[2] - me[2]) * (p[2] - me[2]);
        if (!best || d < bestd) {
            best = g;
            bestd = d;
        }
    }
    push_gob(best);
}

/* coop_alive(gob) -> 1 if the gob still exists (a Lua global can outlive its gob) */
static void l_coop_alive(void)
{
    if (gob_alive(tolua_getusertype(1, NULL)))
        lua_pushnumber(1);
}

/* coop_mark(n): put marker n in the partner's log too */
static void l_coop_mark(void)
{
    event_queue(EV_MARK, "", (int)num_param(1), 0, 0);
}

/* coop_peer_type() -> OBJ type of the peer's player, or nil before it is known */
static void l_coop_peer_type(void)
{
    if (peer_player_type)
        lua_pushnumber(peer_player_type);
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
    tolua_function(NULL, "coop_level", l_coop_level);
    tolua_function(NULL, "coop_peer_type", l_coop_peer_type);
    tolua_function(NULL, "coop_mark", l_coop_mark);
    tolua_function(NULL, "coop_alive", l_coop_alive);
    tolua_function(NULL, "coop_nearest_enemy", l_coop_nearest_enemy);
    tolua_function(NULL, "coop_animdump", l_coop_animdump);
    tolua_function(NULL, "coop_listgobs", l_coop_listgobs);
    tolua_function(NULL, "coop_log", l_coop_log);
    snprintf(ini, MAX_PATH, "%s\\coop.ini", coop_dir);
    net_open(ini);
    deal_damage_orig = hook_code((BYTE *)DEAL_DAMAGE_ADDR, deal_damage_prologue, 5, deal_damage_hook);
    logmsg("coop_init: lua functions registered, damage hook %s", deal_damage_orig ? "installed" : "NOT installed");
    shots_init();
    pickups_init();
    rooms_init();
    frame_init();
    run_mod_script();
}
