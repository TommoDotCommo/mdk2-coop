/*
 * Cutscenes: the Lua side (coop_dev.lua, "Cutscenes") keeps the games' movies together: they start
 * on a shared go, skipping is a vote, and the next level waits until everyone has seen the end.
 * This file is its wire and its screen: EV_MOVIE events (reliable, in order, like every event) and
 * a banner the HUD draws over the picture.
 *
 * EV_MOVIE fields, as coop_movie_send sets them: part = the operation, damage and type = two
 * numbers, pos[0..2] = three more, extra[0..1] = two more, name = a short word. On arrival
 * netid is the sender's slot (the host stamps clients' events; events from the host say 0).
 */
#include <string.h>
#include "bridge.h"

static double num_param(int n)
{
    unsigned int o = lua_getparam(n);
    return (o && lua_isnumber(o)) ? lua_getnumber(o) : 0.0;
}

/* Called as an EV_MOVIE leaves l_coop_event for Lua: the sender's slot goes in netid */
void movie_event(struct event *e, int from)
{
    e->netid = (unsigned int)from;
}

/* coop_movie_send(op, a, b, to, x, y, z, e0, e1, name): to < 0 = everyone (a client: the host).
 * Returns nothing; with no one else connected the event goes nowhere. */
static void l_coop_movie_send(void)
{
    struct event e;
    unsigned int o = lua_getparam(10);
    int to = lua_getparam(4) ? (int)num_param(4) : TO_ALL;

    if (!have_remote)
        return;
    event_init(&e, EV_MOVIE);
    e.part = (signed char)num_param(1);
    e.damage = (short)num_param(2);
    e.type = (int)num_param(3);
    e.pos[0] = (float)num_param(5);
    e.pos[1] = (float)num_param(6);
    e.pos[2] = (float)num_param(7);
    e.extra[0] = (DWORD)(int)num_param(8);
    e.extra[1] = (DWORD)(int)num_param(9);
    if (o) {                    /* always a string from coop_dev.lua */
        long len;
        snprintf(e.name, EV_NAME, "%s", luaL_check_lstr(10, &len));
    }
    event_send(&e, to < 0 ? TO_ALL : to);
}

/* coop_ms() -> the real-time clock in milliseconds (game speed does not scale it) */
static void l_coop_ms(void)
{
    lua_pushnumber(now_ms());
}

/* coop_player_in_level(slot) -> 1 if that player is in the level (and epoch) we are in */
static void l_coop_player_in_level(void)
{
    if (player_in_level((int)num_param(1)))
        lua_pushnumber(1);
}

void movie_lua_init(void)
{
    tolua_function(NULL, "coop_movie_send", l_coop_movie_send);
    tolua_function(NULL, "coop_ms", l_coop_ms);
    tolua_function(NULL, "coop_player_in_level", l_coop_player_in_level);
}
