/*
 * Lua functions the bridge adds to the game, and the damage hook.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "bridge.h"
#include "pad.h"

/* Central damage function: every hit, from scripts and from shots, goes through it.
 * deal_damage(from, target, damage, damagetype, part); first 5 bytes are
 * push ebp; mov ebp,[esp+0xc], a clean place for a jump. */
#define DEAL_DAMAGE_ADDR    0x40d590
static const BYTE deal_damage_prologue[5] = { 0x55, 0x8b, 0x6c, 0x24, 0x0c };
deal_damage_t deal_damage_orig;     /* trampoline into the original */
int local_max_hp;                   /* our player's highest hp seen this level: revive to this */
static char apply_from[EV_NAME];    /* attacker named in the event coop_event last returned */
static int apply_hp;                /* the host's hp for the target after an APPLY, -1 unknown */
static int apply_slot;              /* and the slot of the player who sent it */
static int applying_hit;            /* host: inside EV_HIT, damage from that player's puppet is real */

static double num_param(int n)
{
    unsigned int o = lua_getparam(n);
    return (o && lua_isnumber(o)) ? lua_getnumber(o) : 0.0;
}

/* ---- damage ---- */

/* Damage to our own player. With teammates, a hit that would kill puts us down instead
 * (revive.c), and nothing reaches us while we are down or dead: dying would reload the
 * checkpoint in this game only. */
static void damage_local_player(void *from, int damage, int type, int part)
{
    /* a hit from another player (friendly fire) comes in without an attacker and as an
     * enemy's gun damage (0x2): the player's damage filter drops its own gun's type (0x1),
     * and the game ignores a player character hitting the player */
    if (from && (from == local_player || puppet_slot(from) >= 0)) {
        from = NULL;
        if (type == 0x1)
            type = 0x2;
    }
    /* none of an absorbed hit is applied: some damage types start the death sequence
     * whatever the hp (lava, 0x40, still reloaded the checkpoint when 99 of 500 got through) */
    if (revive_hit(damage, type))
        return;
    deal_damage_orig(from, local_player, damage, type, part);
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

/* Which player an attacker is (their character, or on the host its own player's shots):
 * its slot, or -1 for anything else */
static int attacker_slot(BYTE *from)
{
    if (!from)
        return -1;
    if (from == local_player || gob_is_own_shot(from))
        return my_slot;
    return puppet_slot(from);
}

/* Host: applies everything, tells the clients what it applied, and turns hits on a
 * client's puppet into HURT for that client's own player. Players hurt each other only
 * with friendly fire on ([game] friendly_fire); hurting yourself always counts.
 * Client: only its own player takes damage locally; its hits on anything else go to
 * the host as HIT and come back as APPLY. */
void deal_damage_hook(void *from, void *target, int damage, int type, int part)
{
    char fromname[EV_NAME], name[EV_NAME];
    int target_slot = puppet_slot(target);

    gob_netname(from, fromname, sizeof(fromname));
    gob_netname(target, name, sizeof(name));
    logmsg("damage %s -> %s %d type 0x%x part %d", fromname, name, damage, type, part);

    if (!net_ready() || !have_remote) {
        deal_damage_orig(from, target, damage, type, part);
        return;
    }
    if (role_host) {
        /* a puppet's own attacks are replays of its client's (shots, anims); the client
         * sends its real hits as HIT */
        int from_slot = attacker_slot(from);
        if ((puppet_slot(from) >= 0 && !applying_hit) || gob_is_foreign(from))
            return;
        if (from_slot >= 0 && (target_slot >= 0 || target == local_player) && !cfg_friendly_fire &&
            from_slot != (target_slot >= 0 ? target_slot : my_slot)) {
            logmsg("friendly fire off: player %d's hit on %s dropped", from_slot + 1, name);
            return;
        }
        /* difficulty: what the world does to players, and what players do to the world */
        {
            int scaled = damage;

            if (from_slot < 0 && (target_slot >= 0 || target == local_player))
                scaled = scale_damage(damage, rule_dmg_taken);
            else if (from_slot >= 0 && target_slot < 0 && target != local_player)
                scaled = scale_damage(damage, rule_dmg_dealt);
            if (scaled != damage) {
                logmsg("difficulty: %s -> %s damage %d -> %d", fromname, name, damage, scaled);
                damage = scaled;
            }
        }
        if (target_slot >= 0) {
            snprintf(name, sizeof(name), "@p%d", target_slot + 1);
            event_queue_from(target_slot, EV_HURT, name, from, damage, type, part);
            return;
        }
        if (target == local_player) {
            damage_local_player(from, damage, type, part);
            return;
        }
        deal_damage_orig(from, target, damage, type, part);
        if (target != local_player && name[0])
            event_queue_applied(name, from, damage, type, part, gob_hp(target));
        if (name[0] && gob_is_actor(target))
            logmsg("hp %s %d", name, gob_hp(target));
        return;
    }
    if (target == local_player) {
        /* enemies and their shots are mirrors of the host's; the host sends their hits as HURT */
        if (from_host_world(from))
            return;
        if (attacker_slot(from) < 0)        /* lava and the like: ours to scale; the host's attackers come scaled */
            damage = scale_damage(damage, rule_dmg_taken);
        damage_local_player(from, damage, type, part);
        return;
    }
    /* our gun, or a shot we fired (grenades, sniper rounds); mirrored shots are the host's.
     * Another player goes by player number: the host's friendly fire setting decides */
    if (!(from == local_player || (gob_is_projectile(from) && !gob_is_foreign(from))))
        return;
    if (target_slot >= 0)
        snprintf(name, sizeof(name), "@p%d", target_slot + 1);
    if (name[0])
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
    if (gob && (hp = gob_hp(gob)) > local_max_hp)
        local_max_hp = hp;
    /* before our state is captured: the downed pose (the game's player control puts its own
     * animation back every frame) and a spectator's ride */
    emote_frame();
    revive_frame();
    net_send_player(gob);
    if (gob)
        rooms_check_fall();
    if (gob && role_host)
        ents_host_tick();
}

/* coop_recv(): read the network; on the client this also moves the mirrored actors */
static void l_coop_recv(void)
{
    net_poll();
    voice_frame();
    if (!have_remote)
        return;
    if (!role_host) {
        ents_client_frame();
        ents_client_cleanup();
    }
    shots_frame();
    doors_frame();
}

/* coop_peer(slot) -> x, y, z of that player for this frame, or nil until it has sent one
 * in this level (until then its view still holds where it was before the level change, and
 * a puppet put there wakes that part of the level: level 2's gun room ran its OnCreate on
 * the host only) */
static void l_coop_peer(void)
{
    struct snap *v = player_view((int)num_param(1), 1);

    if (!v)
        return;
    lua_pushnumber(v->pos[0]);
    lua_pushnumber(v->pos[1]);
    lua_pushnumber(v->pos[2]);
}

/* coop_face(gob, slot): give the gob that player's orientation and animation for this frame;
 * coop_face(nil, slot): that player's puppet is gone */
static void l_coop_face(void)
{
    BYTE *gob = tolua_getusertype(1, NULL);
    int slot = (int)num_param(2);
    struct snap *v = player_view(slot, 0);

    if (slot < 0 || slot >= MAX_PLAYERS)
        return;
    if (!gob) {
        puppets[slot] = NULL;
        return;
    }
    if (!v)
        return;
    if (puppets[slot] != gob) {
        /* the puppet is driven from its player's game: skip its class logic like a mirror */
        gob_set_mirrored(gob, 1);
        class_filter_think(gob_class(gob));
    }
    puppets[slot] = gob;
    memcpy(gob + GOB_ORIENTATION, v->quat, sizeof(v->quat));
    if (sync_anim)
        anim_apply(gob, v);
}

/* ---- events ---- */

/* coop_event() -> kind, name, damage, type, part, netid, objtype, x, y, z, extra1, extra2
 * for the next event from the others, or nil. DESPAWN, FIRE, PICKUP, MARK, STATE and READY
 * are handled here. LEVEL: part 1 = the host restarted (load it even if we seem to be there
 * already), 2 = we are in that very level already (same epoch: a link that dropped and came
 * back), extra1 = co-op save to load (0 none), extra2 = the host's player id. */
static void l_coop_event(void)
{
    struct event e;
    int from;

    for (;;) {
        if (!event_next(&e, &from))
            return;
        if (event_must_wait(&e)) {
            event_unget(from);
            return;
        }
        if (e.kind == EV_CHAT || e.kind == EV_PING || e.kind == EV_TYPING) {
            chat_event(&e, from);       /* the host stamps the sender, then passes it on */
            event_relay(&e, from);
            continue;
        }
        if (e.kind == EV_MOVIE)
            movie_event(&e, from);      /* Lua gets it, with the sender's slot in netid */
        /* the host passes the clients' shots, pickups and markers on to the others */
        if (e.kind == EV_FIRE || e.kind == EV_PICKUP || e.kind == EV_MARK)
            event_relay(&e, from);
        if (e.kind == EV_DESPAWN)
            ent_despawn(e.netid);
        else if (e.kind == EV_FIRE)
            shots_replay(&e);
        else if (e.kind == EV_MARK)
            logmsg("lua: MARK %d (from player %d)", e.damage, from + 1);
        else if (e.kind == EV_RULES)
            rules_event(&e);
        else if (e.kind == EV_STATE)
            state_dump(e.name);     /* whatever level we are in: a mismatch is what it is for */
        else if (e.kind == EV_READY && role_host) {
            logmsg("player %d loaded a save: sending the world again", from + 1);
            ents_resend(from);
            pickups_resend(from);
        }
        else if (e.epoch != cur_epoch && e.kind != EV_LEVEL)
            logmsg("event %d %s from epoch %u dropped (we are in %u)", e.kind, e.name, e.epoch, cur_epoch);
        else if (e.kind == EV_PICKUP)
            pickups_remove(e.name);
        else if (e.kind == EV_REVIVE || e.kind == EV_RESPAWN || e.kind == EV_DIED)
            revive_event(&e, from);
        else
            break;
    }
    if (e.kind == EV_LEVEL) {
        int same = cur_epoch == (unsigned short)e.netid && cur_level == e.type;
        net_level_event(&e);
        e.part = host_changed_take() || e.extra[2] ? 1 : same ? 2 : 0;
    }
    apply_slot = from;
    apply_hp = (int)e.extra[0] - 1;
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
    lua_pushnumber(e.extra[0]);
    lua_pushnumber(e.extra[1]);
}

/* The attacker of the event being applied: what the host named, else the sender's puppet
 * (some damage handlers look at the attacker and crash on NULL) */
static BYTE *apply_attacker(void)
{
    BYTE *from = gob_find_peername(apply_from);

    if (!gob_alive(from))
        from = gob_alive(puppets[apply_slot]) ? puppets[apply_slot] : local_player;
    return from;
}

/* coop_apply(kind, gob, damage, type, part) carries out a damage event on the gob Lua found;
 * coop_apply(EV_SPAWN, gob, netid, hp) binds the gob to the host's actor, with the host's hp */
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
    case EV_HIT:        /* host: a client's Kurt hit this; goes through the hook so the clients get APPLY */
        applying_hit = 1;
        deal_damage_hook(puppets[apply_slot], gob, damage, type, part);
        applying_hit = 0;
        break;
    case EV_APPLY:      /* client: the host applied this */
        deal_damage_orig(apply_attacker(), gob, damage, type, part);
        /* a mirror keeps its old hp through the hit (the game's handler leaves it alone, and
         * the host may have ignored the hit): take the host's, death comes from its despawn */
        if (apply_hp > 0 && gob_is_actor(gob) && gob_hp(gob) != apply_hp)
            gob_set_hp(gob, apply_hp);
        break;
    case EV_HURT:       /* client: the host's world hurt our player */
        if (gob == local_player)
            damage_local_player(apply_attacker(), damage, type, part);
        break;
    case EV_SPAWN:
        ent_bind((unsigned int)num_param(3), gob);
        if ((int)num_param(4) > 0 && *(BYTE **)(gob + GOB_INFO) && gob_hp(gob) != (int)num_param(4)) {
            logmsg("ent %s hp %d -> %d (host's)", gob_name(gob), gob_hp(gob), (int)num_param(4));
            *(short *)(*(BYTE **)(gob + GOB_INFO) + 0x12) = (short)num_param(4);
        }
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

    if (name[0] == '@') {
        push_gob(gob_find_peername(name));
        return;
    }
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

/* coop_level(level, checkpoint, announce, save): a level has started here. Resets
 * everything that pointed into the old level; with announce (host), asks the clients to load
 * it too, from their copy of co-op save `save` if that is not 0. */
static void l_coop_level(void)
{
    int level = (int)num_param(1), cp = (int)num_param(2), announce = (int)num_param(3);
    unsigned int save = (unsigned int)num_param(4);
    struct event e;

    bot_level_loaded(level);
    cur_level = level;
    cur_cp = cp;
    gob_sets_clear();
    ents_reset();
    shots_reset();
    doors_reset();
    revive_level_reset();
    pickups_reset();
    rooms_level_reset();
    net_level_reset(announce);
    memset(puppets, 0, sizeof(puppets));
    local_player = NULL;
    local_max_hp = 0;
    if (announce) {
        event_init(&e, EV_LEVEL);
        e.damage = (short)cp;
        e.type = level;
        e.netid = cur_epoch;
        e.extra[0] = save;
        e.extra[1] = my_id;
        e.extra[2] = revive_take_forced();      /* everyone was down: load it even if already there */
        event_send(&e, TO_ALL);
        if (save)
            net_set_last_save(save);
    }
    logmsg("level %d checkpoint %d started (epoch %u)%s", level, cp, cur_epoch, announce ? ", peer asked to follow" : "");
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

/* coop_revive_tick() -> the next action for Lua ("down", "hide", "up", "die", "wipe") or nil */
static void l_coop_revive_tick(void)
{
    const char *a = revive_tick();
    if (a)
        lua_pushstring((char *)a);
    else
        lua_pushnil();
}

/* coop_life(slot) -> 0 alive, 1 downed, 2 dead, 3 dead and spectating (its puppet is parked) */
static void l_coop_life(void)
{
    int s = (int)num_param(1);
    lua_pushnumber(s == my_slot ? revive_life() : player_life(s));
}

/* coop_revive_checkpoint(): host, a checkpoint was reached: the dead come back */
static void l_coop_revive_checkpoint(void)
{
    revive_checkpoint();
}

/* coop_emote(id, loop) -> 1 if it plays: hold animation id on our player until we move (id < 0 stops);
 * coop_emoting() -> the id playing, or -1 */
static void l_coop_emote(void)
{
    unsigned int o = lua_getparam(2);

    lua_pushnumber(emote_play((int)num_param(1), o && lua_isnumber(o) ? lua_getnumber(o) != 0.0 : 1));
}

static void l_coop_emoting(void)
{
    lua_pushnumber(emote_active());
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

/* coop_press(code, ms): hold a key (DirectInput scancode) or mouse button (256 left, 257 right)
 * for ms; only in background test mode, where the real devices are ignored */
static void l_coop_press(void)
{
    if (test_background)
        input_press((int)num_param(1), (int)num_param(2));
}

/* coop_pad_fake(lx, ly, rx, ry, buttons, lt, rt): stand-in gamepad state (sticks -1..1, triggers
 * 0..255, XInput button bits); with no argument the real pad again. Background test mode only. */
static void l_coop_pad_fake(void)
{
    struct pad_raw raw;

    if (!test_background && !couch_active)
        return;
    if (!lua_getparam(1)) {
        pad_fake(NULL);
        return;
    }
    raw.lx = (int)(num_param(1) * 32767);
    raw.ly = (int)(num_param(2) * 32767);
    raw.rx = (int)(num_param(3) * 32767);
    raw.ry = (int)(num_param(4) * 32767);
    raw.buttons = (unsigned)num_param(5);
    raw.lt = (int)num_param(6);
    raw.rt = (int)num_param(7);
    pad_fake(&raw);
}

/* coop_couch(): this player's slot (1 = first) and the number of local players, or nothing when
 * this is not a couch game */
static void l_coop_couch(void)
{
    int slot, count;

    if (input_couch(&slot, &count)) {
        lua_pushnumber(slot + 1);
        lua_pushnumber(count);
    }
}

/* coop_shot(name): screenshot of the next frame to <game dir>\coop\<name>.tga (background
 * test mode; chScreenShot run from a script catches an empty buffer) */
static void l_coop_shot(void)
{
    long len;
    const char *name = luaL_check_lstr(1, &len);
    if (test_background)
        input_shot(name);
}

/* coop_state(tag): write coop\state_<tag>.txt here and, from the host, in the client too */
static void l_coop_state(void)
{
    long len;
    const char *tag = luaL_check_lstr(1, &len);

    state_dump(tag);
    if (role_host)
        event_queue(EV_STATE, tag, 0, 0, 0);
}

/* coop_keep_spawner(gob): this spawner makes items, the client keeps it running */
static void l_coop_keep_spawner(void)
{
    BYTE *gob = tolua_getusertype(1, NULL);
    if (gob)
        ents_keep_spawner(gob);
}

/* coop_peer_type(slot) -> OBJ type of that player's character, or nil before it is known */
static void l_coop_peer_type(void)
{
    int t = player_type((int)num_param(1));

    if (t)
        lua_pushnumber(t);
}

/* coop_hp(gob) -> its hp (mdkGobGetHealth gives nonsense for some gobs), nil if it is gone */
static void l_coop_hp(void)
{
    BYTE *gob = tolua_getusertype(1, NULL);

    if (gob_alive(gob))
        lua_pushnumber(gob_hp(gob));
}

/* coop_slot() -> our slot (0 = host), or nil on a client the host has not answered yet */
static void l_coop_slot(void)
{
    if (my_slot >= 0)
        lua_pushnumber(my_slot);
}

/* coop_connected(slot) -> 1 if that other player is in the game */
static void l_coop_connected(void)
{
    if (player_connected((int)num_param(1)))
        lua_pushnumber(1);
}

/* coop_players() -> how many other players are connected, and the number of slots */
static void l_coop_players(void)
{
    lua_pushnumber(players_connected());
    lua_pushnumber(MAX_PLAYERS);
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
    int id, i, cur;

    if (!gob || !(ctl = *(BYTE **)(gob + GOB_ANIM)))
        return;
    for (cur = 0; cur < ANIM_ID_MAX && (!*(void **)(ctl + 0x28) ||
                                         (void *)om_anim_state(gob, cur) != *(void **)(ctl + 0x28)); cur++)
        ;
    logmsg("animdump %s pos (%.2f, %.2f, %.2f) cur=%p (anim %d) prev=%p", gob_name(gob),
           ((float *)(gob + GOB_POSITION))[0], ((float *)(gob + GOB_POSITION))[1],
           ((float *)(gob + GOB_POSITION))[2], *(void **)(ctl + 0x28), cur < ANIM_ID_MAX ? cur : -1,
           *(void **)(ctl + 0x2c));
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

static void l_coop_shots_dump(void)
{
    shots_dump();
}

static void l_coop_test_sniper(void)
{
    shots_test_sniper();
}

static void l_coop_test_shot(void)
{
    push_gob(shots_test_latest((int)num_param(1)));
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
    if (!coop_enabled) {
        logmsg("coop_init: solo start (launcher): co-op off");
        return;
    }
    tolua_function(NULL, "coop_send", l_coop_send);
    tolua_function(NULL, "coop_recv", l_coop_recv);
    tolua_function(NULL, "coop_peer", l_coop_peer);
    tolua_function(NULL, "coop_face", l_coop_face);
    tolua_function(NULL, "coop_slot", l_coop_slot);
    tolua_function(NULL, "coop_hp", l_coop_hp);
    tolua_function(NULL, "coop_connected", l_coop_connected);
    tolua_function(NULL, "coop_players", l_coop_players);
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
    tolua_function(NULL, "coop_emote", l_coop_emote);
    tolua_function(NULL, "coop_emoting", l_coop_emoting);
    tolua_function(NULL, "coop_revive_tick", l_coop_revive_tick);
    tolua_function(NULL, "coop_life", l_coop_life);
    tolua_function(NULL, "coop_revive_checkpoint", l_coop_revive_checkpoint);
    tolua_function(NULL, "coop_listgobs", l_coop_listgobs);
    tolua_function(NULL, "coop_shots_dump", l_coop_shots_dump);
    tolua_function(NULL, "coop_test_sniper", l_coop_test_sniper);
    tolua_function(NULL, "coop_test_shot", l_coop_test_shot);
    tolua_function(NULL, "coop_log", l_coop_log);
    tolua_function(NULL, "coop_press", l_coop_press);
    tolua_function(NULL, "coop_shot", l_coop_shot);
    tolua_function(NULL, "coop_pad_fake", l_coop_pad_fake);
    tolua_function(NULL, "coop_couch", l_coop_couch);
    tolua_function(NULL, "coop_state", l_coop_state);
    tolua_function(NULL, "coop_keep_spawner", l_coop_keep_spawner);
    snprintf(ini, MAX_PATH, "%s\\coop.ini", coop_dir);
    settings_init(ini);
    net_open(ini);
    settings_start();
    voice_init(ini);
    deal_damage_orig = hook_code((BYTE *)DEAL_DAMAGE_ADDR, deal_damage_prologue, 5, deal_damage_hook);
    logmsg("coop_init: lua functions registered, damage hook %s", deal_damage_orig ? "installed" : "NOT installed");
    shots_init();
    pickups_init();
    rooms_init();
    frame_init();
    saves_lua_init();
    settings_lua_init();
    hud_lua_init();
    ctrlview_lua_init();
    input_lua_init();
    emote_lua_init();
    chat_lua_init();
    movie_lua_init();
    voice_lua_init();
    bot_lua_init();
    run_mod_script();
}
