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

#define ENT_MAGIC       0x464b444d      /* "MDKF": compact snapshots (net.c snap_encode) */
#define MAX_ENTS        256
#define ORPHAN_GRACE_MS 3000.0
#define ENT_PAYLOAD     1200            /* bytes per datagram, under any internet MTU */
#define ENT_ENC_MAX     384             /* one encoded snapshot at most (8 full anims) */
#define ENT_SETTLE      2               /* unchanged snapshots still sent after a change */
#define ENT_KEEPALIVE_MS 1000.0         /* an unchanged enemy is still sent this often */

/* Datagram: this header, then count records of { u32 id, snap_encode bytes } */
struct ent_head {
    unsigned int magic;
    unsigned short epoch;       /* host's level epoch: dropped by a client in another one */
    unsigned char count;
    unsigned char pad;
    unsigned int time;          /* host clock when the snapshots were taken, ms */
};

struct ent {
    unsigned int id;            /* host gob handle */
    BYTE *gob;                  /* local gob */
    unsigned int local;         /* local gob handle, to notice when the gob goes away */
    struct track track;         /* client */
    /* host: what was sent last, to send an enemy only when it changes */
    BYTE last[ENT_ENC_MAX];
    short last_len;
    unsigned char quiet;        /* snapshots in a row that matched the last one */
    unsigned char died;         /* host: the game ran this actor's OnDie (it was killed, not just removed) */
    double last_sent;
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

/* Host, revive.c: enemies still standing in a dead player's zone */
int ents_alive_in_zone(const float *zone_pos)
{
    int i, n = 0;

    for (i = 0; i < nents; i++) {
        BYTE *gob = ents[i].gob;
        if (gob_alive(gob) && gob_hp(gob) > 0 && rooms_in_zone(zone_pos, (float *)(gob + GOB_POSITION)))
            n++;
    }
    return n;
}

/* netstats.c: is this datagram one of ours */
int ents_is_packet(const void *buf, int len)
{
    return len >= (int)sizeof(unsigned int) && *(const unsigned int *)buf == ENT_MAGIC;
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

/* 1 if the gob is one of the synced enemies */
int ent_is_synced(BYTE *gob)
{
    int i;

    for (i = 0; i < nents; i++)
        if (ents[i].gob == gob && gob_by_handle(ents[i].local) == gob)
            return 1;
    return 0;
}

/* state.c: one "ent" line per synced enemy, "gone" where the local gob no longer exists */
void ents_dump(FILE *f)
{
    int i;

    for (i = 0; i < nents; i++) {
        BYTE *gob = gob_by_handle(ents[i].local) == ents[i].gob ? ents[i].gob : NULL;
        float *p;

        if (!gob) {
            fprintf(f, "ent %x gone\n", ents[i].id);
            continue;
        }
        p = (float *)(gob + GOB_POSITION);
        fprintf(f, "ent %x %d %d %.2f %.2f %.2f %s\n", ents[i].id, gob_type(gob), gob_hp(gob),
                p[0], p[1], p[2], gob_name(gob));
    }
}

/* New level: every gob is gone */
void ents_reset(void)
{
    int i;

    for (i = 0; i < nents; i++)
        if (ents[i].gob)
            gob_set_mirrored(ents[i].gob, 0);
    nents = 0;
}

/* ---- death scripts ----
 * An actor's OnDie (the level script's: unlock a door, count the dead, open a way on) runs in the game that kills it,
 * the host's. A client only had the mirror deleted, so doors that open when the first enemy dies stayed shut for
 * every client. The host notes which synced actors the game ran OnDie for and says so in their DESPAWN; the client
 * then runs the same OnDie on its mirror before deleting it. fire_ondie(gob) is the engine's own call
 * (has_lua_event(gob, OnDie), then the script's function with the gob). */
#define FIRE_ONDIE_ADDR 0x40d100
static const BYTE fire_ondie_prologue[7] = { 0x56, 0x8b, 0x74, 0x24, 0x08, 0x6a, 0x0a };
typedef void (*fire_ondie_t)(BYTE *);
static fire_ondie_t fire_ondie_orig;

static void fire_ondie_hook(BYTE *gob)
{
    int i;

    for (i = 0; i < nents; i++)
        if (ents[i].gob == gob)
            ents[i].died = 1;
    fire_ondie_orig(gob);
}

void ents_init(void)
{
    fire_ondie_orig = hook_code((BYTE *)FIRE_ONDIE_ADDR, fire_ondie_prologue, sizeof(fire_ondie_prologue),
                                fire_ondie_hook);
    logmsg("death script hook %s", fire_ondie_orig ? "installed" : "NOT installed");
}

/* ---- host ---- */

void ents_host_tick(void)
{
    static double next_tick;
    double now = now_ms();
    int slot, i;

    if (!have_remote || now < next_tick)
        return;
    next_tick = now + 1000.0 / ent_hz;

    /* despawns go first: a script that deletes an actor and makes a new one of the same
     * name (ch2 in 9-7) would otherwise have the client bind the new id to its old gob,
     * then delete that gob for the old id */
    for (i = 0; i < nents; ) {
        BYTE *gob = gob_by_handle(ents[i].id);
        if (gob != ents[i].gob || !gob_is_actor(gob)) {
            event_queue_spawn(TO_ALL, EV_DESPAWN, ents[i].id, ents[i].died, 0, NULL, "");     /* objtype 1: it died */
            logmsg("ent despawn %x%s", ents[i].id, ents[i].died ? " (died)" : "");
            ent_remove(&ents[i]);
        } else {
            i++;
        }
    }
    for (slot = 0; slot < GOB_SLOTS; slot++) {
        BYTE *gob = (BYTE *)GOB_TABLE[slot * 2];
        unsigned int id;
        struct ent *e;
        char name[EV_NAME];

        if (!gob || !gob_is_actor(gob))
            continue;
        id = gob_handle(slot);
        if (ent_find(id) || nents == MAX_ENTS)
            continue;
        e = &ents[nents++];
        memset(e, 0, sizeof(*e));
        e->id = id;
        e->gob = gob;
        e->local = id;
        /* tougher or softer enemies ([game] enemy_hp), and more with more players (enemy_scale % per extra player) */
        if (gob_hp(gob) > 0 && (cfg_enemy_hp != 100 || (cfg_enemy_scale > 0 && players_connected() > 0))) {
            int max_hp = gob_max_hp(gob);
            if (max_hp <= 0) max_hp = gob_hp(gob);
            max_hp = max_hp * cfg_enemy_hp / 100;
            int hp = gob_hp(gob) * cfg_enemy_hp / 100;
            hp += hp * cfg_enemy_scale * players_connected() / 100;
            max_hp += max_hp * cfg_enemy_scale * players_connected() / 100;
            gob_set_max_hp(gob, max_hp < 1 ? 1 : max_hp);
            gob_set_hp(gob, hp < 1 ? 1 : hp);
        }
        gob_netname(gob, name, sizeof(name));
        event_queue_spawn(TO_ALL, EV_SPAWN, id, gob_type(gob), gob_hp(gob), (float *)(gob + GOB_POSITION), name);
        logmsg("ent spawn %x %s type %d", id, name, gob_type(gob));
    }

    /* snapshots: an enemy goes out when it changed, for ENT_SETTLE more ticks after that
     * (so the client's track ends on two equal states and stops instead of extrapolating
     * the last step), and every ENT_KEEPALIVE_MS while it rests (most are in rooms in
     * stasis). Records are packed into datagrams of up to ENT_PAYLOAD bytes. */
    {
        BYTE buf[sizeof(struct ent_head) + ENT_PAYLOAD], *o = buf + sizeof(struct ent_head);
        struct ent_head *h = (struct ent_head *)buf;

        memset(h, 0, sizeof(*h));
        h->magic = ENT_MAGIC;
        h->epoch = cur_epoch;
        h->time = (unsigned int)now;
        for (i = 0; i < nents; i++) {
            struct ent *e = &ents[i];
            struct snap sn;
            BYTE enc[ENT_ENC_MAX];
            int len, same;

            snap_capture(e->gob, &sn);
            if (!(len = snap_encode(&sn, enc, sizeof(enc))))
                continue;
            same = len == e->last_len && memcmp(enc, e->last, len) == 0;
            e->quiet = same ? (e->quiet < 255 ? e->quiet + 1 : 255) : 0;
            if (same && e->quiet > ENT_SETTLE && now - e->last_sent < ENT_KEEPALIVE_MS)
                continue;
            if (o + 4 + len > buf + sizeof(buf)) {
                net_send(buf, (int)(o - buf));
                o = buf + sizeof(struct ent_head);
                h->count = 0;
            }
            memcpy(o, &e->id, 4);
            memcpy(o + 4, enc, len);
            o += 4 + len;
            h->count++;
            memcpy(e->last, enc, len);
            e->last_len = (short)len;
            e->last_sent = now;
        }
        if (h->count)
            net_send(buf, (int)(o - buf));
    }
}

/* A player who has just joined: announce every enemy we already have */
void ents_resend(int to)
{
    int i;

    for (i = 0; i < nents; i++) {
        char name[EV_NAME];
        if (!gob_alive(ents[i].gob))
            continue;
        gob_netname(ents[i].gob, name, sizeof(name));
        event_queue_spawn(to, EV_SPAWN, ents[i].id, gob_type(ents[i].gob), gob_hp(ents[i].gob), (float *)(ents[i].gob + GOB_POSITION), name);
    }
    logmsg("ents: %d announced to player %d", nents, to + 1);
}

/* ---- client ---- */

void ent_bind(unsigned int netid, BYTE *gob)
{
    struct ent *e = ent_find(netid);
    int slot = gob_slot(gob);
    int i;

    if (slot < 0)
        return;
    /* one gob, one host actor: an older binding to it would delete it on its despawn */
    for (i = 0; i < nents; i++)
        if (ents[i].id != netid && ents[i].gob == gob) {
            logmsg("ent %x lets go of %s", ents[i].id, gob_name(gob));
            ents[i].gob = NULL;
            ents[i].local = 0;
        }
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

int test_no_ondie;              /* coop_test_noondie(1) (background test mode): the old bug, for tools/qa_sign.sh's MUTANT=noondie */

void ent_despawn(unsigned int netid, int died)
{
    struct ent *e = ent_find(netid);
    BYTE *gob;

    if (!e)
        return;
    gob = ent_gob(netid);
    logmsg("ent despawn %x (%s)%s", netid, gob ? gob_name(gob) : "already gone", died ? " died: running its OnDie" : "");
    if (e->gob)
        gob_set_mirrored(e->gob, 0);
    if (gob && died && fire_ondie_orig && !test_no_ondie)
        fire_ondie_orig(gob);
    if (gob)
        gob_kill(gob);
    ent_remove(e);
}

void ents_receive(const void *buf, int len)
{
    const struct ent_head *h = buf;
    const BYTE *p = (const BYTE *)buf + sizeof(*h), *end = (const BYTE *)buf + len;
    int i;

    if (role_host || len < (int)sizeof(*h) || h->magic != ENT_MAGIC || h->epoch != cur_epoch)
        return;
    for (i = 0; i < h->count; i++) {
        unsigned int id;
        struct snap sn;
        struct ent *e;
        int used;

        if (end - p < 4)
            return;
        memcpy(&id, p, 4);
        if (!(used = snap_decode(p + 4, (int)(end - p - 4), &sn)))
            return;
        p += 4 + used;
        sn.time = h->time;
        if ((e = ent_find(id)) && e->gob)
            track_add(&e->track, &sn);
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
        track_update(&e->track, host_clock_offset());
        gob_set_position(gob, e->track.view.pos[0], e->track.view.pos[1], e->track.view.pos[2]);
        memcpy(gob + GOB_ORIENTATION, e->track.view.quat, sizeof(e->track.view.quat));
        if (sync_anim)
            anim_apply(gob, &e->track.view);
    }
}

/* OBJ_DOGANBOYGENERATOR (511), OBJ_SPAWNER (540) */
static int is_spawner(BYTE *gob)
{
    int t;

    if (!*(BYTE **)(gob + GOB_INFO))
        return 0;
    t = gob_type(gob);
    return t == 511 || t == 540;
}

/* Spawners that make items (coop_keep_spawner): they stay on in the client, whose items
 * only exist if it makes them itself. A ring of gob handles: never cleared, since a level's
 * scripts register them before coop_level runs, and an old level's handles match no gob. */
#define MAX_KEPT 128
static unsigned int kept[MAX_KEPT];
static int nkept;           /* registrations so far; kept[] holds the last MAX_KEPT */

void ents_keep_spawner(BYTE *gob)
{
    int slot = gob_slot(gob);

    if (slot < 0)
        return;
    kept[nkept++ % MAX_KEPT] = gob_handle(slot);
    if (!role_host && gob_is_mirrored(gob)) {
        gob_set_mirrored(gob, 0);
        logmsg("spawner %s makes items: switched back on", gob_name(gob));
    }
}

static int spawner_kept(int slot)
{
    unsigned int h = gob_handle(slot);
    int i;

    for (i = 0; i < nkept && i < MAX_KEPT; i++)
        if (kept[i] == h)
            return 1;
    return 0;
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

        if (gob && is_spawner(gob) && !gob_is_mirrored(gob) && !spawner_kept(slot)) {
            /* the host's spawners make the real enemies; ours would only make orphans */
            gob_set_mirrored(gob, 1);
            class_filter_think(gob_class(gob));
            logmsg("spawner %s (type %d) switched off here", gob_name(gob), gob_type(gob));
        }
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
            gob_kill(gob);
        }
    }
}
