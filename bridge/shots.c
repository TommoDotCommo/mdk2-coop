/*
 * Shots (OBJ types 400-499). Every gob is made by one engine function,
 * create_object (0x42d360). Creating a shot also sends a reliable FIRE event (the host
 * announces every shot, a client the ones its own Kurt fires, which the host passes on
 * to the other clients); the others replay it through the same function so it flies with
 * the game's own physics. Class 5 shots are announced after launch, including grenades
 * first attached to a hand: creation alone does not give them a trajectory.
 * Replayed shots are "foreign" and only for show: damage stays
 * with the host (HURT, HIT, APPLY).
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
#ifndef MAIN_SCENE
#define MAIN_SCENE          (*(void **)0x549500)
#endif
#define LAUNCH_SHOT_ADDR    0x403b00
#define SHOT_CLASS          5
#define SHOT_STATE          0x40    /* info -> class 5 state */
#define SHOT_ACTIVE         0x04
#define SHOT_OWNER          0x18
#define SHOT_TARGET         0x1c
#define SHOT_PHYSICS        0x68    /* gob -> physics; velocity at +4 */
#define SHOT_BODY           0x80    /* bouncing shots: rigid body, velocity at +0xb4 */
#define SHOT_BOUNCES        0x08    /* shot descriptor flags (+0x54) */
#ifndef SHOT_IGNORE_OWNER
/* Same handle-list insertion used by launch_shot at 0x403b5a. */
#define SHOT_IGNORE_OWNER(list, owner) \
    ((void (*)(void *, DWORD, const char *, int))0x458360)(list, owner, "coop shots", 0)
#endif

static const BYTE create_prologue[7] = { 0x8b, 0x44, 0x24, 0x04, 0x83, 0xec, 0x0c };
typedef BYTE *(*create_t)(const char *, int, void *, BYTE *, int, float *, float *, const char *,
                          DWORD, DWORD, DWORD, DWORD);
static create_t create_orig;
/* launch_shot(shot, shooter, target, &direction), cdecl; direction may be NULL.
 * The result includes the engine's random aim spread and ballistic arc. */
static const BYTE launch_prologue[5] = { 0x83, 0xec, 0x20, 0x55, 0x56 };
typedef void (*launch_t)(BYTE *, BYTE *, BYTE *, float *);
static launch_t launch_orig;
static int replaying, creating;

/* shot owner handles that mean "player n's character" (OWNER_PLAYER | slot): the receiver
 * swaps them for its own player or that player's puppet. A real handle never looks like
 * this (its low half is a gob slot, below GOB_SLOTS). */
#define OWNER_PLAYER            0xffffff00u

/* ---- foreign shots ---- */

#define MAX_FOREIGN 128

static struct { BYTE *gob; unsigned int handle; } foreign[MAX_FOREIGN];
static int nforeign;

/* shots our own player fired, for friendly fire: same bookkeeping */
static struct { BYTE *gob; unsigned int handle; } own[MAX_FOREIGN];
static int nown;

static void own_add(BYTE *gob)
{
    int slot = gob_slot(gob);

    shots_frame();    /* an address reused this frame must not inherit an old shot's flags */
    if (slot < 0 || nown == MAX_FOREIGN)
        return;
    own[nown].gob = gob;
    own[nown].handle = gob_handle(slot);
    nown++;
    gob_set_own_shot(gob, 1);
}

static void foreign_add(BYTE *gob)
{
    int slot = gob_slot(gob);

    shots_frame();
    if (slot < 0 || nforeign == MAX_FOREIGN)
        return;
    foreign[nforeign].gob = gob;
    foreign[nforeign].handle = gob_handle(slot);
    nforeign++;
    gob_set_foreign(gob, 1);
}

/* Forget replayed and own shots the engine has removed */
void shots_frame(void)
{
    int i;

    for (i = 0; i < nown; ) {
        if (gob_by_handle(own[i].handle) != own[i].gob) {
            gob_set_own_shot(own[i].gob, 0);
            own[i] = own[--nown];
        } else {
            i++;
        }
    }

    for (i = 0; i < nforeign; ) {
        if (gob_by_handle(foreign[i].handle) != foreign[i].gob) {
            gob_set_foreign(foreign[i].gob, 0);
            foreign[i] = foreign[--nforeign];
        } else {
            i++;
        }
    }
}

void shots_reset(void)
{
    while (nforeign)
        gob_set_foreign(foreign[--nforeign].gob, 0);
    while (nown)
        gob_set_own_shot(own[--nown].gob, 0);
}

/* ---- host: announce ---- */

static BYTE *shot_state(BYTE *gob)
{
    BYTE *info = *(BYTE **)(gob + GOB_INFO);
    return info ? *(BYTE **)(info + SHOT_STATE) : NULL;
}

static float *shot_velocity(BYTE *gob, BYTE *state)
{
    BYTE *desc = *(BYTE **)state, *physics;

    /* 0x403e04 chooses the rigid body's velocity for bouncing shots. The regular
     * physics vector still exists on a grenade, but does not drive its flight. */
    if (*(DWORD *)(desc + 0x54) & SHOT_BOUNCES) {
        physics = *(BYTE **)(gob + SHOT_BODY);
        return physics ? (float *)(physics + 0xb4) : NULL;
    }
    physics = *(BYTE **)(gob + SHOT_PHYSICS);
    return physics ? (float *)(physics + 4) : NULL;
}

static DWORD owner_peer(BYTE *gob)
{
    int s, slot;

    if (!gob)
        return 0;
    if (gob == local_player)
        return OWNER_PLAYER | (my_slot >= 0 ? my_slot : 0);
    if ((s = puppet_slot(gob)) >= 0)
        return OWNER_PLAYER | s;
    slot = gob_slot(gob);
    return slot >= 0 ? gob_handle(slot) : 0;
}

static void announce_launch(BYTE *gob)
{
    struct event e;
    BYTE *state = shot_state(gob), *owner, *target;
    float *velocity;

    if (!state || !*(DWORD *)(state + SHOT_ACTIVE) || !(velocity = shot_velocity(gob, state)))
        return;     /* still attached to a hand, or not launched yet */
    owner = gob_by_handle(*(DWORD *)(state + SHOT_OWNER));
    target = gob_by_handle(*(DWORD *)(state + SHOT_TARGET));
    if (gob_is_foreign(gob))
        return;
    /* Copied attack animations can launch shots even though mirrored AI does not think.
     * Only the shooter's own game (or the host for enemies) may announce or damage. */
    if (gob_is_foreign(owner) || gob_is_mirrored(owner) ||
        (!role_host && owner != local_player) || (role_host && puppet_slot(owner) >= 0)) {
        foreign_add(gob);
        gob_kill(gob);
        return;
    }
    if (owner && owner == local_player)
        own_add(gob);
    event_init(&e, EV_FIRE);
    e.part = FIRE_LAUNCHED;
    e.objtype = (short)gob_type(gob);
    e.type = -1;     /* replay in the world, detached from the shooter's model slot */
    e.extra[0] = owner_peer(owner);
    memcpy(e.pos, gob + GOB_POSITION, sizeof(e.pos));
    memcpy(e.quat, gob + GOB_ORIENTATION, sizeof(e.quat));
    memcpy(e.shot.velocity, velocity, sizeof(e.shot.velocity));
    memcpy(e.shot.state, state + SHOT_ACTIVE, sizeof(e.shot.state));
    gob_peername(target, e.name, EV_NAME);
    logmsg("fire type %d at (%.1f, %.1f, %.1f) target '%s' launched velocity (%.3f, %.3f, %.3f) owner %lx",
           e.objtype, e.pos[0], e.pos[1], e.pos[2], e.name,
           velocity[0], velocity[1], velocity[2], (unsigned long)e.extra[0]);
    event_send(&e, TO_ALL);
}

static void launch_hook(BYTE *gob, BYTE *shooter, BYTE *target, float *direction)
{
    /* Replay restores the finished result. Suppress constructor launches as well:
     * they otherwise rerun shooter scripts and can dereference a despawned owner. */
    if (replaying)
        return;
    shots_frame();
    launch_orig(gob, shooter, target, direction);
    if (!creating && have_remote && gob_is_projectile(gob) && gob_class(gob) == SHOT_CLASS)
        announce_launch(gob);
}

static BYTE *create_hook(const char *name, int type, void *scene, BYTE *parent, int slot, float *pos,
                         float *quat, const char *model, DWORD a8, DWORD a9, DWORD a10, DWORD a11)
{
    BYTE *gob;
    struct event ev, *e = &ev;

    creating++;
    gob = create_orig(name, type, scene, parent, slot, pos, quat, model, a8, a9, a10, a11);
    creating--;
    if (!gob || replaying || !have_remote || type < PROJECTILE_TYPE_MIN || type > PROJECTILE_TYPE_MAX)
        return gob;
    if (launch_orig && gob_class(gob) == SHOT_CLASS) {
        announce_launch(gob);    /* some constructors launch before create_object returns */
        return gob;
    }
    /* our player's shot (sniper rounds are created with no owner) */
    if ((a8 && gob_by_handle(a8) == local_player) || (!role_host && !a8))
        own_add(gob);
    /* the client only announces its own Kurt's shots (mirrored enemies do not think, so
     * do not fire; sniper rounds are created with no owner); the host announces all */
    if (!role_host && a8 && gob_by_handle(a8) != local_player)
        return gob;
    if (scene != MAIN_SCENE || model || !pos || !quat) {
        logmsg("fire type %d not replicated: scene %p model %p pos %p quat %p", type, scene, (void *)model,
               (void *)pos, (void *)quat);
        return gob;
    }
    event_init(e, EV_FIRE);
    e->objtype = (short)type;
    e->type = slot;
    memcpy(e->pos, pos, sizeof(e->pos));
    memcpy(e->quat, quat, sizeof(e->quat));
    e->extra[0] = a8;
    if ((a8 && gob_by_handle(a8) && gob_by_handle(a8) == local_player) || (!role_host && !a8))
        e->extra[0] = OWNER_PLAYER | (my_slot >= 0 ? my_slot : 0);
    else if (a8 && gob_by_handle(a8) && puppet_slot(gob_by_handle(a8)) >= 0)
        e->extra[0] = OWNER_PLAYER | puppet_slot(gob_by_handle(a8));
    e->extra[1] = a9;
    e->extra[2] = a10;
    e->extra[3] = a11;
    /* the target as the others know it ("@p<n>" for a player's character) */
    gob_peername(parent, e->name, EV_NAME);
    logmsg("fire type %d at (%.1f, %.1f, %.1f) target '%s' slot %d extra %lx %lx %lx %lx", type, pos[0], pos[1], pos[2],
           e->name, slot, (unsigned long)a8, (unsigned long)a9, (unsigned long)a10, (unsigned long)a11);
    event_send(e, TO_ALL);
    return gob;
}

/* ---- replay ---- */

/* Sender's owner handle -> our handle for the same shooter, or 0 */
static DWORD owner_local(DWORD host)
{
    BYTE *gob = NULL;
    int slot;

    if ((host & 0xffffff00u) == OWNER_PLAYER) {
        int s = (int)(host & 0xff);
        gob = s == my_slot ? local_player : (s < MAX_PLAYERS ? puppets[s] : NULL);
    } else if (host && !role_host)
        gob = ent_gob(host);
    if (!gob || (slot = gob_slot(gob)) < 0)
        return 0;
    return gob_handle(slot);
}

void shots_replay(const struct event *e)
{
    BYTE *target = NULL, *gob, *state;
    float pos[3], quat[4], *velocity;
    DWORD owner;

    if (!create_orig || e->epoch != cur_epoch || e->objtype < PROJECTILE_TYPE_MIN ||
        e->objtype > PROJECTILE_TYPE_MAX || (e->part != 0 && e->part != FIRE_LAUNCHED))
        return;
    if (e->part == FIRE_LAUNCHED && !launch_orig)
        return;
    target = gob_find_peername(e->name);
    memcpy(pos, e->pos, sizeof(pos));
    memcpy(quat, e->quat, sizeof(quat));
    owner = owner_local(e->extra[0]);
    replaying++;
    gob = create_orig("", e->objtype, MAIN_SCENE, e->part == FIRE_LAUNCHED ? NULL : target, e->type, pos, quat, NULL,
                      owner, e->part == FIRE_LAUNCHED ? 0 : e->extra[1],
                      e->part == FIRE_LAUNCHED ? 0 : e->extra[2],
                      e->part == FIRE_LAUNCHED ? 0 : e->extra[3]);
    if (gob)
        foreign_add(gob);
    if (gob && e->part == FIRE_LAUNCHED) {
        state = gob_class(gob) == SHOT_CLASS ? shot_state(gob) : NULL;
        velocity = state ? shot_velocity(gob, state) : NULL;
        if (velocity) {
            BYTE *physics = *(BYTE **)(gob + SHOT_PHYSICS);
            int target_slot = target ? gob_slot(target) : -1;

            gob_set_position(gob, pos[0], pos[1], pos[2]);
            memcpy(gob + GOB_ORIENTATION, e->quat, sizeof(e->quat));
            memcpy(velocity, e->shot.velocity, sizeof(e->shot.velocity));
            memcpy(state + SHOT_ACTIVE, e->shot.state, sizeof(e->shot.state));
            *(DWORD *)(state + SHOT_OWNER) = owner ? owner : 0xffffffffu;
            *(DWORD *)(state + SHOT_TARGET) = target_slot >= 0 ? gob_handle(target_slot) : 0xffffffffu;
            if (owner && physics)
                SHOT_IGNORE_OWNER(physics + 0x104, owner);
            logmsg("replay launch type %d at (%.1f, %.1f, %.1f) velocity (%.3f, %.3f, %.3f) owner %lx",
                   e->objtype, pos[0], pos[1], pos[2], velocity[0], velocity[1], velocity[2], (unsigned long)owner);
        } else {
            gob_kill(gob);
            gob = NULL;
        }
    }
    replaying--;
    logmsg("replay fire type %d target '%s' (%s) -> %s", e->objtype, e->name, target ? "found" : "none",
           gob ? "created" : "FAILED");
}

void shots_init(void)
{
    launch_orig = hook_code((BYTE *)LAUNCH_SHOT_ADDR, launch_prologue, sizeof(launch_prologue), launch_hook);
    logmsg("launch_shot hook %s", launch_orig ? "installed" : "NOT installed");
    create_orig = hook_code((BYTE *)CREATE_OBJECT_ADDR, create_prologue, sizeof(create_prologue), create_hook);
    logmsg("create_object hook %s", create_orig ? "installed" : "NOT installed");
}

void shots_dump(void)
{
    int slot, count = 0;

    for (slot = 0; slot < GOB_SLOTS; slot++) {
        BYTE *gob = (BYTE *)GOB_TABLE[slot * 2], *state;
        float *p, *v;
        if (!gob || !gob_is_projectile(gob) || gob_class(gob) != SHOT_CLASS || gob_doomed(gob))
            continue;
        state = shot_state(gob);
        v = state ? shot_velocity(gob, state) : NULL;
        if (!v)
            continue;
        p = (float *)(gob + GOB_POSITION);
        logmsg("shot type %d active %lu foreign %d own %d at (%.3f, %.3f, %.3f) velocity (%.3f, %.3f, %.3f)",
               gob_type(gob), (unsigned long)*(DWORD *)(state + SHOT_ACTIVE), gob_is_foreign(gob),
               gob_is_own_shot(gob), p[0], p[1], p[2], v[0], v[1], v[2]);
        count++;
    }
    logmsg("shots: %d class-5 projectiles", count);
}

/* Test-only access to the newest live foreign shot, for flight and damage assertions. */
BYTE *shots_test_latest(int type)
{
    int i;

    for (i = nforeign - 1; test_background && i >= 0; i--)
        if (gob_by_handle(foreign[i].handle) == foreign[i].gob &&
            !gob_doomed(foreign[i].gob) && gob_type(foreign[i].gob) == type)
            return foreign[i].gob;
    return NULL;
}

/* The same create / launch pair as Kurt's sniper (0x4091ec), independent of input focus. */
void shots_test_sniper(void)
{
    BYTE *gob;
    float pos[3], quat[4];

    if (!test_background || !create_orig || !launch_orig || !gob_alive(local_player) || gob_type(local_player) != 100)
        return;
    memcpy(pos, local_player + GOB_POSITION, sizeof(pos));
    memcpy(quat, local_player + GOB_ORIENTATION, sizeof(quat));
    pos[2] += 1.0f;
    gob = create_hook("", 406, MAIN_SCENE, NULL, -1, pos, quat, NULL, 0, 0, 0, 0);
    if (gob)
        launch_hook(gob, local_player, NULL, quat);
}
