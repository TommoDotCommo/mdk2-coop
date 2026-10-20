/*
 * Proximity doors (OBJ_PROXDOOR1, 700). Their think (0x4251a0) opens the door when the
 * player, the gob whose handle is at PLAYER_HANDLE, is within its radius, and closes it
 * when they leave; it never looks at anyone else. Wrapping the think so it sees
 * whichever of our Kurt and the other players' puppets is closest lets everyone open
 * doors, in every game.
 */
#include <string.h>
#include "bridge.h"

#define PLAYER_HANDLE   (*(DWORD *)0x5494c8)
#define OBJ_PROXDOOR1   700

static DWORD door_think_orig;

static float dist2(BYTE *a, BYTE *b)
{
    float *pa = (float *)(a + GOB_POSITION), *pb = (float *)(b + GOB_POSITION);
    float dx = pa[0] - pb[0], dy = pa[1] - pb[1], dz = pa[2] - pb[2];

    return dx * dx + dy * dy + dz * dz;
}

/* door state: game object +0x40 -> { open, hold, ?, radius, locked } (from the think) */
static int door_open(BYTE *door)
{
    BYTE *info = *(BYTE **)(door + GOB_INFO);
    DWORD *state = info ? *(DWORD **)(info + 0x40) : NULL;

    return state ? (int)state[0] : 0;
}

/* log open/close transitions across one think */
/* log open/close transitions across one think; nearer: slot of the puppet it saw, -1 ours */
static void door_log(BYTE *door, int was_open, int nearer)
{
    int now_open = door_open(door);

    if (was_open == now_open)
        return;
    if (nearer >= 0)
        logmsg("door %s %s (nearer player: puppet %d)", gob_name(door), now_open ? "opening" : "closing", nearer + 1);
    else
        logmsg("door %s %s (nearer player: ours)", gob_name(door), now_open ? "opening" : "closing");
}

static void __cdecl door_think(BYTE *door)
{
    DWORD saved = PLAYER_HANDLE;
    BYTE *player = gob_by_handle(saved);
    float best = player ? dist2(door, player) : 0.0f;
    int s, nearer = -1, was_open;

    for (s = 0; player && s < MAX_PLAYERS; s++) {
        int gs;
        float d;
        if (!puppets[s] || (gs = gob_slot(puppets[s])) < 0)
            continue;
        if ((d = dist2(door, puppets[s])) < best) {
            best = d;
            nearer = s;
            PLAYER_HANDLE = gob_handle(gs);
        }
    }
    was_open = door_open(door);
    ((void (*)(BYTE *))door_think_orig)(door);
    door_log(door, was_open, nearer);
    PLAYER_HANDLE = saved;
}

void doors_reset(void)
{
}

/* Per frame: install the wrapper once a door exists */
void doors_frame(void)
{
    int slot;

    if (door_think_orig)
        return;
    for (slot = 0; slot < GOB_SLOTS; slot++) {
        BYTE *gob = (BYTE *)GOB_TABLE[slot * 2];
        DWORD *desc, old;
        int cls;

        if (!gob || !*(BYTE **)(gob + GOB_INFO) || gob_type(gob) != OBJ_PROXDOOR1)
            continue;
        cls = gob_class(gob);
        desc = (DWORD *)CLASS_TABLE[cls];
        if (!desc[CLASS_THINK])
            return;
        door_think_orig = desc[CLASS_THINK];
        VirtualProtect(desc, CLASS_SLOTS * 4, PAGE_READWRITE, &old);
        desc[CLASS_THINK] = (DWORD)door_think;
        VirtualProtect(desc, CLASS_SLOTS * 4, old, &old);
        logmsg("doors: class %d think %lx wrapped (opens for the nearer player)", cls,
               (unsigned long)door_think_orig);
        return;
    }
}
