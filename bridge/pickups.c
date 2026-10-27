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
#define EVENT_ONENTERROOM   11

static const BYTE has_event_prologue[6] = { 0x53, 0x56, 0x8b, 0x74, 0x24, 0x0c };
typedef int (*has_event_t)(BYTE *, int);
static has_event_t has_event_orig;
static int removing;

/* Host: items picked up in this level, by anyone, for players who join later */
#define MAX_TAKEN 256
static char taken[MAX_TAKEN][EV_NAME];
static int ntaken;

static void taken_add(const char *name)
{
    if (role_host && ntaken < MAX_TAKEN)
        snprintf(taken[ntaken++], EV_NAME, "%s", name);
}

#define MAX_WAITING 16
static void waiting_clear(void);

void pickups_reset(void)
{
    ntaken = 0;
    waiting_clear();
}

void pickups_resend(int to)
{
    struct event e;
    int i;

    for (i = 0; i < ntaken; i++) {
        event_init(&e, EV_PICKUP);
        snprintf(e.name, EV_NAME, "%s", taken[i]);
        event_send(&e, to);
    }
    if (ntaken)
        logmsg("pickups: %d taken items sent to player %d", ntaken, to + 1);
}

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
        if (name[0] && pickups_per_player(name)) {
            logmsg("pickup %s (type %d): the others take their own", name, gob_type(gob));
        } else if (name[0]) {
            event_queue(EV_PICKUP, name, 0, 0, 0);
            taken_add(name);
            logmsg("pickup %s (type %d)", name, gob_type(gob));
        } else {
            logmsg("pickup of unnamed item type %d not synced", gob_type(gob));
        }
    }
    if (event == EVENT_ONENTERROOM && rooms_suppress_enter(gob))
        return 0;           /* a teammate entering this room already ran it here (rooms.c) */
    {
        int has = has_event_orig(gob, event);
        if (has)
            script_trace(gob, event);       /* scripttrace.c */
        return has;
    }
}

/* Items that give the picker something to use and whose script then changes the picker's inventory: level 1's
 * tutorial grenades (their OnPickedUp takes four away, mdkKurtRemoveItem on "the player"; tools/script_census.py
 * --inventory-pickups lists them). Everyone needs one to do the tutorial, so a peer's pickup does not remove ours:
 * each player takes their own, and each game runs the item's script for its own player when they do. */
int pickups_per_player(const char *name)
{
    return !strcmp(name, "grenades") || !strcmp(name, "snipergrenade");
}

static BYTE *item_named(const char *name)
{
    int i;

    for (i = 0; i < GOB_SLOTS; i++) {
        BYTE *gob = (BYTE *)GOB_TABLE[i * 2];
        if (gob && strncmp((const char *)gob + GOB_NAME, name, EV_NAME) == 0 && is_item(gob) && !gob_doomed(gob))
            return gob;
    }
    return NULL;
}

/* The peer (player `from`) picked this up. Our copy goes, but not before the item's OnPickedUp has run here too
 * (remote.c, with their puppet as the player, and the item's type as its argument as the engine passes it): the
 * script state it changes is local to each game, and only the picker's ran it. The item may not be here yet: a
 * room's OnEnterRoom that makes it runs here a moment after the peer walked in, and a pickup can follow within
 * that moment; it waits a few seconds for the item (pickups_frame) */
#define WAIT_MS     8000.0
static struct { char name[EV_NAME]; int from; double until; } waiting[MAX_WAITING];

static int remove_now(const char *name, int from)
{
    BYTE *gob = item_named(name);

    if (!gob)
        return 0;
    logmsg("peer picked up %s: removing it here after its script", name);
    remote_queue(name, "OnPickedUp", from, gob_type(gob), 1);
    return 1;
}

void pickups_remove(const char *name, int from)
{
    int i;

    if (pickups_per_player(name))
        return;                 /* not sent by current games; an older peer's: keep ours */
    taken_add(name);
    if (remove_now(name, from))
        return;
    for (i = 0; i < MAX_WAITING; i++)
        if (!waiting[i].name[0]) {
            snprintf(waiting[i].name, EV_NAME, "%s", name);
            waiting[i].from = from;
            waiting[i].until = now_ms() + WAIT_MS;
            logmsg("peer picked up %s: not here yet, waiting for it", name);
            return;
        }
    logmsg("peer picked up %s: not here", name);
}

/* per frame, from diag_tick's neighbour in frame.c */
void pickups_frame(void)
{
    static int frames;
    double now;
    int i;

    if (++frames % 6)
        return;
    now = now_ms();
    for (i = 0; i < MAX_WAITING; i++) {
        if (!waiting[i].name[0])
            continue;
        if (remove_now(waiting[i].name, waiting[i].from) || now > waiting[i].until) {
            if (now > waiting[i].until)
                logmsg("peer picked up %s: it never appeared here", waiting[i].name);
            waiting[i].name[0] = 0;
        }
    }
}

static void waiting_clear(void)
{
    int i;

    for (i = 0; i < MAX_WAITING; i++)
        waiting[i].name[0] = 0;
}

/* remote.c: the handler has run (or there is none) */
void pickups_finish(const char *name)
{
    BYTE *gob = item_named(name);

    if (gob) {
        logmsg("peer picked up %s: removed here", name);
        removing = 1;
        gob_kill(gob);
        removing = 0;
    }
}

void pickups_init(void)
{
    has_event_orig = hook_code((BYTE *)HAS_LUA_EVENT_ADDR, has_event_prologue, sizeof(has_event_prologue),
                               has_event_hook);
    logmsg("pickup hook %s", has_event_orig ? "installed" : "NOT installed");
}
