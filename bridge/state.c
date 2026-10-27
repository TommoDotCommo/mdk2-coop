/*
 * State dumps for automated checks: coop_state(tag) on the host writes what this game
 * holds to <game dir>\coop\state_<tag>.txt and asks the client (EV_STATE) to do the same,
 * so tools/statecheck.py can compare the two worlds line by line.
 *
 *   role host|client
 *   level <level>
 *   me <n>                                  our player number (host 1)
 *   player <type> <hp> <x> <y> <z>          our Kurt (or Max, Doc)
 *   puppet <n> <x> <y> <z>                  player n as drawn here
 *   ent <netid> <type> <hp> <x> <y> <z> <name>   synced enemies (client: gone if deleted)
 *   actor <type> <hp> <x> <y> <z> <name>    actors outside the sync (client: not announced)
 *   item <type> <x> <y> <z> <name>
 *   door <open> <name>
 */
#include <stdio.h>
#include "bridge.h"

#define ITEM_TYPE_MIN   300
#define ITEM_TYPE_MAX   399
#define OBJ_PROXDOOR1   700

static void put_pos(FILE *f, BYTE *gob)
{
    float *p = (float *)(gob + GOB_POSITION);
    fprintf(f, " %.2f %.2f %.2f", p[0], p[1], p[2]);
}

/* door state: game object +0x40 -> { open, ... } (see doors.c) */
static int door_open(BYTE *door)
{
    BYTE *info = *(BYTE **)(door + GOB_INFO);
    DWORD *state = info ? *(DWORD **)(info + 0x40) : NULL;
    return state ? (int)state[0] : 0;
}

void state_dump(const char *tag)
{
    char path[MAX_PATH];
    FILE *f;
    int slot, s;

    snprintf(path, MAX_PATH, "%s\\state_%s.tmp", coop_dir, tag);
    if (!(f = fopen(path, "w")))
        return;
    fprintf(f, "role %s\nlevel %d\nme %d\nlife %d\n", role_host ? "host" : "client", cur_level, my_slot + 1,
            revive_life());
    if (gob_alive(local_player)) {
        fprintf(f, "player %d %d", gob_type(local_player), gob_hp(local_player));
        put_pos(f, local_player);
        fputc('\n', f);
    }
    for (s = 0; s < MAX_PLAYERS; s++) {
        if (!gob_alive(puppets[s]))
            continue;
        fprintf(f, "puppet %d", s + 1);
        put_pos(f, puppets[s]);
        fputc('\n', f);
    }
    ents_dump(f);
    combat_dump(f);
    for (slot = 0; slot < GOB_SLOTS; slot++) {
        BYTE *gob = (BYTE *)GOB_TABLE[slot * 2];
        int t;

        if (!gob || gob == local_player || puppet_slot(gob) >= 0 || !*(BYTE **)(gob + GOB_INFO))
            continue;
        t = gob_type(gob);
        if (gob_is_actor(gob) && !ent_is_synced(gob)) {
            fprintf(f, "actor %d %d", t, gob_hp(gob));
            put_pos(f, gob);
            fprintf(f, " %s\n", gob_name(gob));
        } else if (t >= ITEM_TYPE_MIN && t <= ITEM_TYPE_MAX) {
            fprintf(f, "item %d", t);
            put_pos(f, gob);
            fprintf(f, " %s\n", gob_name(gob));
        } else if (t == OBJ_PROXDOOR1) {
            fprintf(f, "door %d %s\n", door_open(gob), gob_name(gob));
        }
    }
    fclose(f);
    {
        char final[MAX_PATH];
        snprintf(final, MAX_PATH, "%s\\state_%s.txt", coop_dir, tag);
        DeleteFileA(final);
        MoveFileA(path, final);     /* scripts never see half a file */
    }
    logmsg("state %s dumped", tag);
}
