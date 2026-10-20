/*
 * Proximity doors (OBJ_PROXDOOR1, 700). Their think (0x4251a0) opens the door when the
 * player, the gob whose handle is at PLAYER_HANDLE, is within its radius, and closes it
 * when they leave; it never looks at anyone else. Wrapping the think so it sees
 * whichever of our Kurt and the other player's puppet is closer lets both open doors,
 * in both games.
 */
#include <string.h>
#include "bridge.h"

#define PLAYER_HANDLE   (*(DWORD *)0x5494c8)
#define OBJ_PROXDOOR1   700

static DWORD door_think_orig;
static DWORD puppet_handle;
static BYTE *puppet_handle_for;

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
static void door_log(BYTE *door, int was_open, int for_puppet)
{
    int now_open = door_open(door);

    if (was_open != now_open)
        logmsg("door %s %s (nearer player: %s)", gob_name(door), now_open ? "opening" : "closing",
               for_puppet ? "puppet" : "ours");
}

static void __cdecl door_think(BYTE *door)
{
    DWORD saved = PLAYER_HANDLE;
    BYTE *player = gob_by_handle(saved);
    int was_open;

    if (puppet && puppet_handle && gob_by_handle(puppet_handle) == puppet && player &&
        dist2(door, puppet) < dist2(door, player))
        PLAYER_HANDLE = puppet_handle;
    was_open = door_open(door);
    ((void (*)(BYTE *))door_think_orig)(door);
    door_log(door, was_open, PLAYER_HANDLE != saved);
    PLAYER_HANDLE = saved;
}

void doors_reset(void)
{
    puppet_handle = 0;
    puppet_handle_for = NULL;
}

/* Per frame: install the wrapper once a door exists, track the puppet's handle */
void doors_frame(void)
{
    int slot;

    if (puppet && puppet != puppet_handle_for) {
        slot = gob_slot(puppet);
        puppet_handle = slot >= 0 ? gob_handle(slot) : 0;
        puppet_handle_for = puppet;
    }
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
