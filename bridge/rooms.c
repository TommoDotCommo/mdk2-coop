/*
 * Rooms. Each frame the room manager (0x4302a0) marks every room inactive, then
 * mark_rooms(arg, camera, 1) (0x430020) re-activates the room whose bounding box holds
 * the camera plus the rooms visible from it and makes it current; inactive rooms go
 * into stasis (gob flag 0x8) and nothing in them runs. Redirecting that one call so it
 * also activates the rooms around the other player's puppet (without entering them:
 * no OnEnterRoom, music or current room change) keeps the world running where either
 * player is. Each game also activates the rooms at its own player (the partner does,
 * for the puppet), so both wake the same rooms.
 */
#include <string.h>
#include "bridge.h"

#define MARK_ROOMS_ADDR     0x430020
#define MARK_ROOMS_CALL     ((BYTE *)0x430314)      /* call mark_rooms(arg, camera, 1) */

/* Room list: ROOM_COUNT entries at ROOM_ARRAY, each a struct with
 * [0] room gob, +0x4 number of visible rooms, +0xc array of them, +0x24 inactive,
 * +0x2c / +0x38 bounding box min / max. point_in_box(pos, min, max) = 0x453fc0. */
#define ROOM_COUNT      (*(int *)0x524378)
#define ROOM_ARRAY      (*(BYTE ***)0x524380)
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

static void __cdecl mark_rooms_both(void *arg, BYTE *camera, int set_current)
{
    ((mark_rooms_t)MARK_ROOMS_ADDR)(arg, camera, set_current);
    if (puppet && have_remote && gob_slot(puppet) >= 0) {
        activate_rooms_at((float *)(puppet + GOB_POSITION));
        /* our own player's rooms too: the partner wakes rooms where our player stands,
         * we by our camera; a room holding the player but not the camera (12-6, a step
         * left at the start) only woke over there and made its items there only */
        if (gob_alive(local_player))
            activate_rooms_at((float *)(local_player + GOB_POSITION));
    }
}

/* Lowest point of the level: min z over the room boxes (computed once per level) */
static float level_floor;
static int have_floor;
static double level_started;

void rooms_level_reset(void)
{
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

/* With a partner, falling far below every room (the level has no kill plane there:
 * Kurt fell forever off level 1's dome area) brings us back to the partner */
void rooms_check_fall(void)
{
    float *pos;

    if (!local_player || !have_remote || !gob_alive(puppet))
        return;
    /* the room list is rebuilt while a level starts: reading it in the first frames of
     * level 2 killed the client */
    if (!have_floor && (now_ms() - level_started < FLOOR_DELAY_MS || ROOM_COUNT <= 0 || IsBadReadPtr(ROOM_ARRAY, 4) ||
                        !(have_floor = compute_floor())))
        return;
    pos = (float *)(local_player + GOB_POSITION);
    if (pos[2] > level_floor - FALL_MARGIN)
        return;
    logmsg("fell out of the world at z %.1f (floor %.1f): back to the partner", pos[2], level_floor);
    gob_set_position(local_player, ((float *)(puppet + GOB_POSITION))[0], ((float *)(puppet + GOB_POSITION))[1],
                     ((float *)(puppet + GOB_POSITION))[2]);
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
    *(DWORD *)(call + 1) = (DWORD)mark_rooms_both - (DWORD)(call + 5);
    VirtualProtect(call, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), call, 5);
    logmsg("rooms: manager also keeps the puppet's rooms active");
}
