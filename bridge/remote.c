/*
 * Script handlers for what happened to another player. The level scripts' handlers run only in the game where
 * the event happens, and script state (door locks, counters, "the mortars are gone") is never synced, so a
 * handler another player triggered has to be run here too:
 *   - a pickup: the other game removes its copy of the item, but the item's OnPickedUp (pickups.c) never ran
 *   - entering a room: the room's OnEnterRoom (rooms.c) ran for the player who entered, not for the others
 * Both queue the handler here and Lua (coop.HandlerTick) runs it with the other player's puppet as "the player"
 * (mdkGetPlayerGob), because some handlers act on the player. A handler that locks a door behind the one who
 * triggered it closes an area: a player left outside is brought to them (coop.CheckStranded).
 */
#include <stdio.h>
#include <string.h>
#include "bridge.h"

#define PEND 64

static struct pending {
    char name[EV_NAME];
    char event[16];
    int slot, arg, kill;
} pend[PEND];
static unsigned int pend_head, pend_tail;

/* name: the gob's Lua global; event: "OnEnterRoom"; slot: who it happened to; arg: the handler's second argument;
 * kill: remove that item afterwards (a pickup) */
int test_no_remote;         /* coop_test_noremote(1) (background test mode): queue nothing: the old behaviour, for tools/qa_remote.sh */

void remote_queue(const char *name, const char *event, int slot, int arg, int kill)
{
    struct pending *p;

    if (test_no_remote) {
        if (kill)
            pickups_finish(name);       /* the old behaviour removed the copy and ran nothing */
        return;
    }

    if (pend_head - pend_tail >= PEND) {
        logmsg("remote handlers: queue full, dropping %s %s", event, name);
        if (kill)
            pickups_finish(name);
        return;
    }
    p = &pend[pend_head++ % PEND];
    snprintf(p->name, sizeof(p->name), "%s", name);
    snprintf(p->event, sizeof(p->event), "%s", event);
    p->slot = slot;
    p->arg = arg;
    p->kill = kill;
}

void remote_reset(void)
{
    pend_head = pend_tail = 0;
}

static double num_param_remote(int n)
{
    unsigned int o = lua_getparam(n);
    return (o && lua_isnumber(o)) ? lua_getnumber(o) : 0.0;
}

/* coop_next_handler() -> name, event, slot, arg, kill; nil when the queue is empty */
static void l_coop_next_handler(void)
{
    struct pending *p;

    if (pend_tail == pend_head) {
        lua_pushnil();
        return;
    }
    p = &pend[pend_tail++ % PEND];
    lua_pushstring(p->name);
    lua_pushstring(p->event);
    lua_pushnumber(p->slot);
    lua_pushnumber(p->arg);
    lua_pushnumber(p->kill);
}

/* coop_pickup_finish(name): the handler has run: remove the item here */
static void l_coop_pickup_finish(void)
{
    long len;

    pickups_finish(luaL_check_lstr(1, &len));
}

/* coop_stranded(slot) -> 1 if our player is alive and outside the zone of that player (the rooms around them and
 * the rooms visible from those: the same rooms the room manager wakes), 0 otherwise */
static void l_coop_stranded(void)
{
    int slot = (int)num_param_remote(1);

    if (slot < 0 || slot >= MAX_PLAYERS || !gob_alive(puppets[slot]) || !gob_alive(local_player) ||
        revive_life() != LIFE_ALIVE) {
        lua_pushnumber(0);
        return;
    }
    lua_pushnumber(!rooms_in_zone((float *)(puppets[slot] + GOB_POSITION), (float *)(local_player + GOB_POSITION)));
}

/* coop_door_locked(door) -> 1 if that proximity door is locked (tests) */
static void l_coop_door_locked(void)
{
    BYTE *gob = tolua_getusertype(1, NULL);

    lua_pushnumber(gob_alive(gob) ? door_locked(gob) : -1);
}

/* coop_notice(text): a line in everyone's chat area from the game itself (here: ours only) */
static void l_coop_notice(void)
{
    long len;

    chat_notice(luaL_check_lstr(1, &len));
}

/* coop_player_name(slot) -> that player's name */
static void l_coop_player_name(void)
{
    lua_pushstring(player_name((int)num_param_remote(1)));
}

/* coop_near_type(type1, type2, radius) -> 1 if a gob of either OBJ type is within radius of our player (ladders:
 * a player on one is not moved) */
static void l_coop_near_type(void)
{
    int t1 = (int)num_param_remote(1), t2 = (int)num_param_remote(2), i;
    float r = (float)num_param_remote(3), *p;

    if (!gob_alive(local_player)) {
        lua_pushnumber(0);
        return;
    }
    p = (float *)(local_player + GOB_POSITION);
    for (i = 0; i < GOB_SLOTS; i++) {
        BYTE *gob = (BYTE *)GOB_TABLE[i * 2];
        float *q, dx, dy, dz;
        int t;

        if (!gob || gob == local_player)
            continue;
        t = gob_type(gob);
        if (t != t1 && t != t2)
            continue;
        q = (float *)(gob + GOB_POSITION);
        dx = q[0] - p[0]; dy = q[1] - p[1]; dz = q[2] - p[2];
        if (dx * dx + dy * dy + dz * dz <= r * r) {
            lua_pushnumber(1);
            return;
        }
    }
    lua_pushnumber(0);
}

/* coop_room_center(name) -> x, y, z: the middle of that room's bounding box (tests send a player there) */
static void l_coop_room_center(void)
{
    long len;
    float c[3];
    int found;

    rooms_center(luaL_check_lstr(1, &len), c, &found);
    if (!found) {
        lua_pushnil();
        return;
    }
    lua_pushnumber(c[0]);
    lua_pushnumber(c[1]);
    lua_pushnumber(c[2]);
}

/* coop_test_noremote(on): tests only */
static void l_coop_test_noremote(void)
{
    if (test_background)
        test_no_remote = (int)num_param_remote(1);
}

void remote_lua_init(void)
{
    tolua_function(NULL, "coop_next_handler", l_coop_next_handler);
    tolua_function(NULL, "coop_pickup_finish", l_coop_pickup_finish);
    tolua_function(NULL, "coop_stranded", l_coop_stranded);
    tolua_function(NULL, "coop_door_locked", l_coop_door_locked);
    tolua_function(NULL, "coop_notice", l_coop_notice);
    tolua_function(NULL, "coop_player_name", l_coop_player_name);
    tolua_function(NULL, "coop_near_type", l_coop_near_type);
    tolua_function(NULL, "coop_room_center", l_coop_room_center);
    tolua_function(NULL, "coop_test_noremote", l_coop_test_noremote);
}
