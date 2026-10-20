/*
 * Shots (OBJ types 400-499). Every gob is made by one engine function,
 * create_object (0x42d360). Creating a shot also sends a reliable FIRE event (the host
 * announces every shot, the client the ones its own Kurt fires); the peer replays it
 * through the same function so it flies with the game's own physics. Replayed shots
 * are "foreign" and only for show: damage stays with the host (HURT, HIT, APPLY).
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "bridge.h"

/* create_object(name, type, scene, parent, slot, &pos, &quat, model, a8..a11), cdecl.
 * For shots, parent is the shooter's target (0x42e030 passes the gob at shooter+0x88)
 * and a8 is the shooter's gob handle (damage is credited to it).
 * Starts with mov eax,[esp+4]; sub esp,0xc: 7 bytes to move. */
#define CREATE_OBJECT_ADDR  0x42d360
#define MAIN_SCENE          (*(void **)0x549500)

static const BYTE create_prologue[7] = { 0x8b, 0x44, 0x24, 0x04, 0x83, 0xec, 0x0c };
typedef BYTE *(*create_t)(const char *, int, void *, BYTE *, int, float *, float *, const char *,
                          DWORD, DWORD, DWORD, DWORD);
static create_t create_orig;
static int replaying;

/* shot owner handles that mean "a player": the receiver swaps them for its own gobs */
#define OWNER_SENDER_PLAYER     0xfffffff1u     /* sender's Kurt = receiver's puppet */
#define OWNER_RECEIVER_PLAYER   0xfffffff2u     /* sender's puppet = receiver's Kurt */

/* ---- foreign shots ---- */

#define MAX_FOREIGN 128

static struct { BYTE *gob; unsigned int handle; } foreign[MAX_FOREIGN];
static int nforeign;

static void foreign_add(BYTE *gob)
{
    int slot = gob_slot(gob);

    if (slot < 0 || nforeign == MAX_FOREIGN)
        return;
    foreign[nforeign].gob = gob;
    foreign[nforeign].handle = gob_handle(slot);
    nforeign++;
    gob_set_foreign(gob, 1);
}

/* Forget replayed shots the engine has removed */
void shots_frame(void)
{
    int i;

    for (i = 0; i < nforeign; ) {
        if (gob_by_handle(foreign[i].handle) != foreign[i].gob) {
            gob_set_foreign(foreign[i].gob, 0);
            foreign[i] = foreign[--nforeign];
        } else {
            i++;
        }
    }
}

/* ---- host: announce ---- */

static BYTE *create_hook(const char *name, int type, void *scene, BYTE *parent, int slot, float *pos,
                         float *quat, const char *model, DWORD a8, DWORD a9, DWORD a10, DWORD a11)
{
    BYTE *gob = create_orig(name, type, scene, parent, slot, pos, quat, model, a8, a9, a10, a11);
    struct event *e;

    if (replaying || !have_remote || type < PROJECTILE_TYPE_MIN || type > PROJECTILE_TYPE_MAX)
        return gob;
    /* the client only announces its own Kurt's shots (mirrored enemies do not think, so
     * do not fire; sniper rounds are created with no owner); the host announces all */
    if (!role_host && a8 && gob_by_handle(a8) != local_player)
        return gob;
    if (scene != MAIN_SCENE || model || !pos || !quat) {
        logmsg("fire type %d not replicated: scene %p model %p pos %p quat %p", type, scene, (void *)model,
               (void *)pos, (void *)quat);
        return gob;
    }
    if (!(e = event_new(EV_FIRE)))
        return gob;
    e->objtype = (short)type;
    e->type = slot;
    memcpy(e->pos, pos, sizeof(e->pos));
    memcpy(e->quat, quat, sizeof(e->quat));
    e->extra[0] = a8;
    if ((a8 && gob_by_handle(a8) && gob_by_handle(a8) == local_player) || (!role_host && !a8))
        e->extra[0] = OWNER_SENDER_PLAYER;
    else if (a8 && gob_by_handle(a8) && gob_by_handle(a8) == puppet)
        e->extra[0] = OWNER_RECEIVER_PLAYER;
    e->extra[1] = a9;
    e->extra[2] = a10;
    e->extra[3] = a11;
    /* the target as the client knows it: our Kurt is their puppet, our puppet is their Kurt */
    if (!parent)
        e->name[0] = 0;
    else if (parent == local_player)
        snprintf(e->name, EV_NAME, "@puppet");
    else if (parent == puppet)
        snprintf(e->name, EV_NAME, "@player");
    else
        gob_netname(parent, e->name, EV_NAME);
    logmsg("fire type %d at (%.1f, %.1f, %.1f) target '%s' slot %d extra %lx %lx %lx %lx", type, pos[0], pos[1], pos[2],
           e->name, slot, (unsigned long)a8, (unsigned long)a9, (unsigned long)a10, (unsigned long)a11);
    return gob;
}

/* ---- client: replay ---- */

static BYTE *find_named(const char *name)
{
    int i;

    for (i = 0; i < GOB_SLOTS; i++) {
        BYTE *gob = (BYTE *)GOB_TABLE[i * 2];
        if (gob && strncmp((const char *)gob + GOB_NAME, name, EV_NAME) == 0)
            return gob;
    }
    return NULL;
}

/* Sender's owner handle -> our handle for the same shooter, or 0 */
static DWORD owner_local(DWORD host)
{
    BYTE *gob = NULL;
    int slot;

    if (host == OWNER_SENDER_PLAYER)
        gob = puppet;
    else if (host == OWNER_RECEIVER_PLAYER)
        gob = local_player;
    else if (host && !role_host)
        gob = ent_gob(host);
    if (!gob || (slot = gob_slot(gob)) < 0)
        return 0;
    return gob_handle(slot);
}

void shots_replay(const struct event *e)
{
    BYTE *target = NULL, *gob;
    float pos[3], quat[4];
    DWORD owner;

    if (!create_orig)
        return;
    if (!strcmp(e->name, "@player"))
        target = local_player;
    else if (!strcmp(e->name, "@puppet"))
        target = puppet;
    else if (e->name[0] == 'n' && strspn(e->name + 1, "0123456789abcdef") == strlen(e->name + 1) && e->name[1])
        target = ent_gob(strtoul(e->name + 1, NULL, 16));
    else if (e->name[0])
        target = find_named(e->name);
    memcpy(pos, e->pos, sizeof(pos));
    memcpy(quat, e->quat, sizeof(quat));
    owner = owner_local(e->extra[0]);
    replaying = 1;
    gob = create_orig("", e->objtype, MAIN_SCENE, target, e->type, pos, quat, NULL,
                      owner, e->extra[1], e->extra[2], e->extra[3]);
    replaying = 0;
    if (gob)
        foreign_add(gob);
    logmsg("replay fire type %d target '%s' (%s) -> %s", e->objtype, e->name, target ? "found" : "none",
           gob ? "created" : "FAILED");
}

void shots_init(void)
{
    create_orig = hook_code((BYTE *)CREATE_OBJECT_ADDR, create_prologue, sizeof(create_prologue), create_hook);
    logmsg("create_object hook %s", create_orig ? "installed" : "NOT installed");
}
