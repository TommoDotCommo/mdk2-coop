/*
 * Rooms. Each frame the room manager (0x4302a0) marks every room inactive, then
 * mark_rooms(arg, camera, 1) (0x430020) re-activates the room whose bounding box holds
 * the camera plus the rooms visible from it and makes it current; inactive rooms go
 * into stasis (gob flag 0x8) and nothing in them runs. Redirecting that one call so it
 * also activates the rooms around the other players' puppets (without entering them:
 * no OnEnterRoom, music or current room change) keeps the world running wherever a
 * player is. Each game also activates the rooms at its own player (the others do, for
 * its puppet), so all wake the same rooms.
 *
 * OnEnterRoom (the level scripts lock doors behind the player, start waves, set hints) fires in the engine only
 * for the game's own camera, so a room a teammate entered never ran it here. rooms_remote_frame watches which
 * room each puppet is in and, when one enters a room nobody was in, queues the room's OnEnterRoom for remote.c
 * (run here with that puppet as the player); the engine's own call for our player is held back
 * (rooms_suppress_enter) when a teammate already ran it, so a room's handler runs once per entry, wherever
 * the entry was and on every game.
 */
#include <string.h>
#include "bridge.h"

#define MARK_ROOMS_ADDR     0x430020
#define MARK_ROOMS_CALL     ((BYTE *)0x430314)      /* call mark_rooms(arg, camera, 1) */

/* Room list: ROOM_COUNT entries at ROOM_ARRAY, each a struct with
 * [0] room gob, +0x4 number of visible rooms, +0xc array of them, +0x24 inactive,
 * +0x2c / +0x38 bounding box min / max. point_in_box(pos, min, max) = 0x453fc0. */
#define ROOM_COUNT      (*(int *)ADDR.room_count)
#define ROOM_ARRAY      (*(BYTE ***)ADDR.room_array)
#define ROOM_INACTIVE   0x24
#define point_in_box    ((int (*)(float *, float *, float *))0x453fc0)
#define FALL_MARGIN     30.0f       /* this far below the lowest room counts as out of the world */
#define FLOOR_DELAY_MS  5000.0      /* compute the level's lowest point this long after it starts */

typedef void (*mark_rooms_t)(void *, BYTE *, int);

/* Just the activation part of mark_rooms: mark_rooms itself also fires OnEnterRoom,
 * switches music and reads camera-only fields (it crashed when given the puppet) */
static void activate_rooms_at(float *pos)
{
    int i, j;

    for (i = 0; i < ROOM_COUNT; i++) {
        BYTE *room = ROOM_ARRAY[i];
        BYTE **vis;
        int nvis;

        if (!room || !point_in_box(pos, (float *)(room + 0x2c), (float *)(room + 0x38)))
            continue;
        *(DWORD *)(room + ROOM_INACTIVE) = 0;
        nvis = *(int *)(room + 0x4);
        vis = *(BYTE ***)(room + 0xc);
        for (j = 0; vis && j < nvis; j++)
            if (vis[j])
                *(DWORD *)(vis[j] + ROOM_INACTIVE) = 0;
    }
}

/* revive.c: is p in a dead player's zone, the rooms holding zone_pos and the rooms visible
 * from them (the same rooms the manager wakes around a player) */
int rooms_in_zone(const float *zone_pos, const float *p)
{
    int i, j;

    if (ROOM_COUNT <= 0 || IsBadReadPtr(ROOM_ARRAY, 4))
        return 0;
    for (i = 0; i < ROOM_COUNT; i++) {
        BYTE *room = ROOM_ARRAY[i];
        BYTE **vis;
        int nvis;

        if (!room || IsBadReadPtr(room, 0x44) ||
            !point_in_box((float *)zone_pos, (float *)(room + 0x2c), (float *)(room + 0x38)))
            continue;
        if (point_in_box((float *)p, (float *)(room + 0x2c), (float *)(room + 0x38)))
            return 1;
        nvis = *(int *)(room + 0x4);
        vis = *(BYTE ***)(room + 0xc);
        for (j = 0; vis && j < nvis; j++)
            if (vis[j] && !IsBadReadPtr(vis[j], 0x44) &&
                point_in_box((float *)p, (float *)(vis[j] + 0x2c), (float *)(vis[j] + 0x38)))
                return 1;
    }
    return 0;
}

/* ---- rooms the other players enter ---- */

#define TRACK_ROOMS     1024
#define ENTER_DELAY_MS  3000.0      /* after a level starts the room list is rebuilt: leave it alone */

static double level_started;
static int prev_room[MAX_PLAYERS];      /* the room each puppet is in; -2 not known yet */
static unsigned char fired[TRACK_ROOMS];/* a puppet entered this room and ran its OnEnterRoom here */
static int local_room;

static int room_list_ok(void)
{
    return ROOM_COUNT > 0 && ROOM_COUNT <= TRACK_ROOMS && !IsBadReadPtr(ROOM_ARRAY, ROOM_COUNT * sizeof(BYTE *));
}

static int room_of(const float *pos)
{
    int i;

    for (i = 0; i < ROOM_COUNT; i++) {
        BYTE *room = ROOM_ARRAY[i];
        if (room && !IsBadReadPtr(room, 0x44) && point_in_box((float *)pos, (float *)(room + 0x2c), (float *)(room + 0x38)))
            return i;
    }
    return -1;
}

static int puppet_in(int room, int except)
{
    int s;

    for (s = 0; s < MAX_PLAYERS; s++)
        if (s != except && prev_room[s] == room)
            return 1;
    return 0;
}

void rooms_remote_frame(void)
{
    int s;

    if (!have_remote || now_ms() - level_started < ENTER_DELAY_MS || !room_list_ok())
        return;
    local_room = gob_alive(local_player) ? room_of((float *)(local_player + GOB_POSITION)) : -1;
    for (s = 0; s < MAX_PLAYERS; s++) {
        int r, old;

        if (s == my_slot || !gob_alive(puppets[s]) || player_life(s) != LIFE_ALIVE) {
            prev_room[s] = -2;          /* gone, or down or dead and parked somewhere: not entering anything */
            continue;
        }
        r = room_of((float *)(puppets[s] + GOB_POSITION));
        if (prev_room[s] == -2) {
            prev_room[s] = r;           /* the room they start in is not an entry */
            continue;
        }
        if (r < 0 || r == prev_room[s])
            continue;                   /* between boxes: still where they were */
        old = prev_room[s];
        prev_room[s] = r;
        if (old >= 0 && !puppet_in(old, -1))
            fired[old] = 0;
        if (r == local_room || puppet_in(r, s))
            continue;                   /* somebody is in it already: that was the entry */
        {
            const char *name = gob_name(*(BYTE **)ROOM_ARRAY[r]);
            if (name[0]) {
                logmsg("player %d entered room %s: running its OnEnterRoom here too", s + 1, name);
                remote_queue(name, "OnEnterRoom", s, 0, 0);
                fired[r] = 1;
            }
        }
    }
}

/* coop_room_center(name) -> x, y, z of the middle of that room's box (tests: somewhere inside it); nil if unknown */
void rooms_center(const char *name, float *out, int *found)
{
    int i;

    *found = 0;
    if (!room_list_ok())
        return;
    for (i = 0; i < ROOM_COUNT; i++) {
        BYTE *room = ROOM_ARRAY[i];
        if (room && !IsBadReadPtr(room, 0x44) && *(BYTE **)room && strcmp(gob_name(*(BYTE **)room), name) == 0) {
            float *lo = (float *)(room + 0x2c), *hi = (float *)(room + 0x38);
            out[0] = (lo[0] + hi[0]) / 2;
            out[1] = (lo[1] + hi[1]) / 2;
            out[2] = (lo[2] + hi[2]) / 2;
            *found = 1;
            return;
        }
    }
}

/* The engine is about to run `room`'s OnEnterRoom for our player: not if a puppet that entered it has run it here */
int rooms_suppress_enter(BYTE *room)
{
    int i;

    if (!have_remote || !room_list_ok())
        return 0;
    for (i = 0; i < ROOM_COUNT; i++)
        if (ROOM_ARRAY[i] && !IsBadReadPtr(ROOM_ARRAY[i], 4) && *(BYTE **)ROOM_ARRAY[i] == room)
            return fired[i] && puppet_in(i, -1);
    return 0;
}

static void __cdecl mark_rooms_all(void *arg, BYTE *camera, int set_current)
{
    int s;

    ((mark_rooms_t)MARK_ROOMS_ADDR)(arg, camera, set_current);
    if (!have_remote)
        return;
    for (s = 0; s < MAX_PLAYERS; s++)
        if (gob_alive(puppets[s]))
            activate_rooms_at((float *)(puppets[s] + GOB_POSITION));
    /* our own player's rooms too: the others wake rooms where our player stands, we by our
     * camera; a room holding the player but not the camera (12-6, a step left at the start)
     * only woke over there and made its items there only */
    if (gob_alive(local_player))
        activate_rooms_at((float *)(local_player + GOB_POSITION));
    rooms_remote_frame();
}

/* Lowest point of the level: min z over the room boxes (computed once per level) */
static float level_floor;
static int have_floor;

void rooms_level_reset(void)
{
    int s;

    for (s = 0; s < MAX_PLAYERS; s++)
        prev_room[s] = -2;
    memset(fired, 0, sizeof(fired));
    local_room = -1;
    have_floor = 0;
    level_started = now_ms();
}

static int compute_floor(void)
{
    int i, found = 0;

    for (i = 0; i < ROOM_COUNT; i++) {
        BYTE *room = ROOM_ARRAY[i];
        float *bmin, *bmax;
        if (!room || IsBadReadPtr(room, 0x44))
            continue;
        bmin = (float *)(room + 0x2c);
        bmax = (float *)(room + 0x38);
        if (bmax[2] <= bmin[2])
            continue;       /* no box */
        if (!found || bmin[2] < level_floor)
            level_floor = bmin[2];
        found = 1;
    }
    if (found)
        logmsg("rooms: lowest point of the level z %.1f", level_floor);
    return found;
}

/* With teammates, falling far below every room (the level has no kill plane there: Kurt
 * fell forever off level 1's dome area) is a death with no body to reach (revive.c) */
void rooms_check_fall(void)
{
    BYTE *puppet;
    float *pos;

    if (!local_player || !have_remote || !(puppet = puppet_nearest((float *)(local_player + GOB_POSITION))))
        return;
    /* the room list is rebuilt while a level starts: reading it in the first frames of
     * level 2 killed the client */
    if (!have_floor && (now_ms() - level_started < FLOOR_DELAY_MS || ROOM_COUNT <= 0 || IsBadReadPtr(ROOM_ARRAY, 4) ||
                        !(have_floor = compute_floor())))
        return;
    pos = (float *)(local_player + GOB_POSITION);
    if (pos[2] > level_floor - FALL_MARGIN)
        return;
    if (revive_life() != LIFE_ALIVE)
        return;
    logmsg("fell out of the world at z %.1f (floor %.1f)", pos[2], level_floor);
    revive_unreachable("fell out of the world");
}

void rooms_init(void)
{
    BYTE *call = MARK_ROOMS_CALL;
    DWORD old;

    if (call[0] != 0xe8 || (DWORD)(call + 5) + *(DWORD *)(call + 1) != MARK_ROOMS_ADDR) {
        logmsg("rooms: unexpected code at %p, not patched", (void *)call);
        return;
    }
    VirtualProtect(call, 5, PAGE_EXECUTE_READWRITE, &old);
    *(DWORD *)(call + 1) = (DWORD)mark_rooms_all - (DWORD)(call + 5);
    VirtualProtect(call, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), call, 5);
    logmsg("rooms: manager also keeps the puppets' rooms active");
}
