/*
 * Enemy sync. The host runs every actor (OBJ types 200-259); the client mirrors them.
 * (Shots are replayed instead, see shots.c.)
 *
 * Host: scans the engine's gob table, gives each actor a network id (its gob handle),
 * announces it with a reliable SPAWN (DESPAWN when it disappears) and sends position,
 * rotation and animation ent_hz times a second.
 * Client: binds each SPAWN to a gob of the same name (level-placed actors) or one Lua
 * creates as "n<id>", skips their think (AI, shot flight), draws them from the host's
 * snapshots, and deletes actors its own spawners make that the host never announced.
 */
#include <stdio.h>
#include <string.h>
#include "bridge.h"

#define ENT_MAGIC       0x454b444d      /* "MDKE" */
#define ENT_PER_PACKET  3
#define MAX_ENTS        256
#define ORPHAN_GRACE_MS 3000.0

struct ent_state {
    unsigned int id;
    struct snap state;
};

struct ent_packet {
    unsigned int magic;
    unsigned char count;
    struct ent_state ents[ENT_PER_PACKET];
};

struct ent {
    unsigned int id;            /* host gob handle */
    BYTE *gob;                  /* local gob */
    unsigned int local;         /* local gob handle, to notice when the gob goes away */
    int seen;                   /* host: present in the latest scan */
    struct track track;         /* client */
};

static struct ent ents[MAX_ENTS];
static int nents;

static struct ent *ent_find(unsigned int id)
{
    int i;

    for (i = 0; i < nents; i++)
        if (ents[i].id == id)
            return &ents[i];
    return NULL;
}

static void ent_remove(struct ent *e)
{
    *e = ents[--nents];
}

int ent_count(void)
{
    return nents;
}

BYTE *ent_gob(unsigned int netid)
{
    struct ent *e = ent_find(netid);

    if (!e)
        return NULL;
    return gob_by_handle(e->local) == e->gob ? e->gob : NULL;
}

/* ---- host ---- */

void ents_host_tick(void)
{
    static double next_tick;
    struct ent_packet p;
    double now = now_ms();
    int slot, i;

    if (!have_remote || now < next_tick)
        return;
    next_tick = now + 1000.0 / ent_hz;

    for (i = 0; i < nents; i++)
        ents[i].seen = 0;
    for (slot = 0; slot < GOB_SLOTS; slot++) {
        BYTE *gob = (BYTE *)GOB_TABLE[slot * 2];
        unsigned int id;
        struct ent *e;

        if (!gob || !gob_is_actor(gob))
            continue;
        id = gob_handle(slot);
        if (!(e = ent_find(id))) {
            char name[EV_NAME];
            if (nents == MAX_ENTS)
                continue;
            e = &ents[nents++];
            memset(e, 0, sizeof(*e));
            e->id = id;
            e->gob = gob;
            e->local = id;
            gob_netname(gob, name, sizeof(name));
            event_queue_spawn(EV_SPAWN, id, gob_type(gob), (float *)(gob + GOB_POSITION), name);
            logmsg("ent spawn %x %s type %d", id, name, gob_type(gob));
        }
        e->seen = 1;
    }
    for (i = 0; i < nents; ) {
        if (!ents[i].seen) {
            event_queue_spawn(EV_DESPAWN, ents[i].id, 0, NULL, "");
            logmsg("ent despawn %x", ents[i].id);
            ent_remove(&ents[i]);
        } else {
            i++;
        }
    }

    memset(&p, 0, sizeof(p));
    p.magic = ENT_MAGIC;
    for (i = 0; i < nents; i++) {
        p.ents[p.count].id = ents[i].id;
        snap_capture(ents[i].gob, &p.ents[p.count].state);
        if (++p.count == ENT_PER_PACKET || i == nents - 1) {
            net_send(&p, sizeof(p));
            p.count = 0;
        }
    }
}

/* ---- client ---- */

void ent_bind(unsigned int netid, BYTE *gob)
{
    struct ent *e = ent_find(netid);
    int slot = gob_slot(gob);

    if (slot < 0)
        return;
    if (!e) {
        if (nents == MAX_ENTS)
            return;
        e = &ents[nents++];
        memset(e, 0, sizeof(*e));
        e->id = netid;
    }
    if (e->gob && e->gob != gob)
        gob_set_mirrored(e->gob, 0);
    e->gob = gob;
    e->local = gob_handle(slot);
    gob_set_mirrored(gob, 1);
    class_filter_think(gob_class(gob));
    logmsg("ent bind %x -> %s (local %x, type %d)", netid, gob_name(gob), e->local, gob_type(gob));
}

void ent_despawn(unsigned int netid)
{
    struct ent *e = ent_find(netid);
    BYTE *gob;

    if (!e)
        return;
    gob = ent_gob(netid);
    logmsg("ent despawn %x (%s)", netid, gob ? gob_name(gob) : "already gone");
    if (e->gob)
        gob_set_mirrored(e->gob, 0);
    if (gob)
        gob_delete(gob);
    ent_remove(e);
}

void ents_receive(const void *buf, int len)
{
    const struct ent_packet *p = buf;
    int i;

    if (role_host || len != sizeof(*p) || p->magic != ENT_MAGIC || p->count > ENT_PER_PACKET)
        return;
    for (i = 0; i < p->count; i++) {
        struct ent *e = ent_find(p->ents[i].id);
        if (e && e->gob && p->ents[i].state.nanims <= SNAP_ANIMS)
            track_add(&e->track, &p->ents[i].state);
    }
}

void ents_client_frame(void)
{
    int i;

    for (i = 0; i < nents; i++) {
        struct ent *e = &ents[i];
        BYTE *gob = ent_gob(e->id);

        if (!gob && e->gob) {
            /* the engine removed it (corpse, shot hit something): forget the pointer */
            gob_set_mirrored(e->gob, 0);
            e->gob = NULL;
        }
        if (!gob || !e->track.n)
            continue;
        track_update(&e->track);
        gob_set_position(gob, e->track.view.pos[0], e->track.view.pos[1], e->track.view.pos[2]);
        memcpy(gob + GOB_ORIENTATION, e->track.view.quat, sizeof(e->track.view.quat));
        if (sync_anim)
            anim_apply(gob, &e->track.view);
    }
}

/* Delete actors the host never announced (the client's own spawners and scripts) */
void ents_client_cleanup(void)
{
    static double next_check;
    static double first_seen[GOB_SLOTS];
    static unsigned int seen_handle[GOB_SLOTS];
    double now = now_ms();
    int slot, i;

    if (role_host || !have_remote || now < next_check)
        return;
    next_check = now + 500.0;
    for (slot = 0; slot < GOB_SLOTS; slot++) {
        BYTE *gob = (BYTE *)GOB_TABLE[slot * 2];
        unsigned int h = gob_handle(slot);

        if (!gob || !gob_is_actor(gob)) {
            seen_handle[slot] = 0;
            continue;
        }
        for (i = 0; i < nents && !(ents[i].local == h && ents[i].gob == gob); i++)
            ;
        if (i < nents) {
            seen_handle[slot] = 0;
            continue;
        }
        if (seen_handle[slot] != h) {
            seen_handle[slot] = h;
            first_seen[slot] = now;
        } else if (now - first_seen[slot] > ORPHAN_GRACE_MS) {
            logmsg("ent orphan %s (type %d) deleted: host has no such actor", gob_name(gob), gob_type(gob));
            seen_handle[slot] = 0;
            gob_delete(gob);
        }
    }
}
