/*
 * Pickups. Whoever picks an item up keeps it; the peer deletes its copy.
 *
 * Every pickup path (Kurt, Max and Doc adding to their inventory at 0x414e50, items
 * used on the spot near 0x4157c4) asks has_lua_event(item, OnPickedUp) and nothing
 * else asks for that event, so hooking has_lua_event spots pickups. Items are matched
 * by name, so only named (level-placed) items are synced so far.
 */
#include <stdio.h>
#include <string.h>
#include "bridge.h"

/* has_lua_event(gob, event): starts push ebx; push esi; mov esi,[esp+0xc] */
#define HAS_LUA_EVENT_ADDR  0x40cf60
#define EVENT_ONPICKEDUP    15

static const BYTE has_event_prologue[6] = { 0x53, 0x56, 0x8b, 0x74, 0x24, 0x0c };
typedef int (*has_event_t)(BYTE *, int);
static has_event_t has_event_orig;
static int removing;

/* Items: OBJ_SHOTGUN .. 399 (apples, hams, grenades, decoys, weapons); some pickup paths
 * also ask about the picker, so everything else is ignored */
#define ITEM_TYPE_MIN   300
#define ITEM_TYPE_MAX   399

static int is_item(BYTE *gob)
{
    int t;

    if (!gob || IsBadReadPtr(gob + GOB_INFO, 4) || !*(BYTE **)(gob + GOB_INFO))
        return 0;
    t = gob_type(gob);
    return t >= ITEM_TYPE_MIN && t <= ITEM_TYPE_MAX;
}

static int has_event_hook(BYTE *gob, int event)
{
    if (!gob)
        return 0;       /* the original reads gob+0x84 */
    if (event == EVENT_ONPICKEDUP && !removing && have_remote && is_item(gob)) {
        char name[EV_NAME];
        gob_netname(gob, name, sizeof(name));
        if (name[0]) {
            event_queue(EV_PICKUP, name, 0, 0, 0);
            logmsg("pickup %s (type %d)", name, gob_type(gob));
        } else {
            logmsg("pickup of unnamed item type %d not synced", gob_type(gob));
        }
    }
    return has_event_orig(gob, event);
}

/* The peer picked this up: remove our copy */
void pickups_remove(const char *name)
{
    int i;

    for (i = 0; i < GOB_SLOTS; i++) {
        BYTE *gob = (BYTE *)GOB_TABLE[i * 2];
        if (gob && strncmp((const char *)gob + GOB_NAME, name, EV_NAME) == 0 && is_item(gob) &&
            !gob_doomed(gob)) {
            logmsg("peer picked up %s: removed here", name);
            removing = 1;
            gob_kill(gob);
            removing = 0;
            return;
        }
    }
    logmsg("peer picked up %s: not here", name);
}

void pickups_init(void)
{
    has_event_orig = hook_code((BYTE *)HAS_LUA_EVENT_ADDR, has_event_prologue, sizeof(has_event_prologue),
                               has_event_hook);
    logmsg("pickup hook %s", has_event_orig ? "installed" : "NOT installed");
}
