/*
 * Script event trace: which of the level scripts' handlers (OnDie, OnTimer, OnEnterRoom...) ran in this game.
 * The engine asks has_lua_event(gob, event) (0x40cf60, hooked in pickups.c) before it runs a gob's handler for an
 * event, so each "yes" is noted here: the first time per level for every (gob, event), as
 *     script event L<level>: <event> <gob name> [(mirror)]
 * tools/script_diff.py compares these lines between the host's log and a client's: a handler that ran on one and
 * never on the other is the bug class "the door after the first enemy stayed shut for clients" belonged to.
 * "Asked and present" is not "run" in every case; that is what the diff and tools/script_events.txt are for.
 * Counts are kept too, and `coop_script_events()` logs them all.
 */
#include <stdio.h>
#include <string.h>
#include "bridge.h"

/* the engine's event ids (table at 0x4f6370, 16 bytes each; NOTES.md session 1j) */
static const char *const event_name[] = {
    "OnUpdate", "OnCollision", "OnDestroy", "OnDamage", "OnHear", "OnAnimLoop", "OnCustomKey", "OnTimer", "OnCreate",
    "OnSpawn", "OnDie", "OnEnterRoom", "OnShotLanded", "OnShotExploded", "OnOutOfAmmo", "OnPickedUp", "OnItemUsed",
    "OnModeSwitch", "OnItemEquiped"
};
#define NEVENTS ((int)(sizeof(event_name) / sizeof(event_name[0])))
#define TABLE   8192            /* (gob, event) pairs per level; open addressing, a power of two */

static struct entry {
    BYTE *gob;
    unsigned char event;
    unsigned int count;
    char name[24];
} table[TABLE];
static int used;
static int full_logged;

void script_trace_reset(void)
{
    memset(table, 0, sizeof(table));
    used = 0;
    full_logged = 0;
}

void script_trace(BYTE *gob, int event)
{
    unsigned int h;
    struct entry *e;
    int probes;

    if (event < 0 || event >= NEVENTS || !gob)
        return;
    h = (((unsigned int)(uintptr_t)gob >> 4) * 2654435761u + (unsigned int)event * 40503u) & (TABLE - 1);
    for (probes = 0; probes < TABLE; probes++, h = (h + 1) & (TABLE - 1)) {
        e = &table[h];
        if (e->gob == gob && e->event == event) {
            e->count++;
            return;
        }
        if (!e->gob)
            break;
    }
    if (used >= TABLE - TABLE / 8) {
        if (!full_logged++)
            logmsg("script event trace full: not counting more this level");
        return;
    }
    e = &table[h];
    e->gob = gob;
    e->event = (unsigned char)event;
    e->count = 1;
    snprintf(e->name, sizeof(e->name), "%s", gob_name(gob));
    used++;
    logmsg("script event L%d: %s %s%s", cur_level, event_name[event], e->name[0] ? e->name : "(unnamed)",
           gob_is_mirrored(gob) ? " (mirror)" : "");
}

/* coop_script_events(): every (gob, event) seen this level with how often */
void script_trace_dump(void)
{
    int i;

    for (i = 0; i < TABLE; i++)
        if (table[i].gob)
            logmsg("script event count L%d: %s %s x%u", cur_level, event_name[table[i].event],
                   table[i].name[0] ? table[i].name : "(unnamed)", table[i].count);
    logmsg("script events: %d (gob, event) pairs this level", used);
}
