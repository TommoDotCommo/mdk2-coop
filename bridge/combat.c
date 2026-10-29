/* Damage telemetry uses the same authority as HIT/APPLY, never predicted HP. */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "bridge.h"
#include "combat_core.h"
#include "combat_hud.h"
static struct cb_state combat;
static struct cbh_state hud;
static struct cb_guard reports, outgoing;
static unsigned short epoch;
static unsigned int serial;
/* meter: 0 collapsed to one thin line (default), 1 expanded. The dump keeps the old header fields. */
static int meter, keys[3];
static unsigned int source_serial;
static int dirty, test_deaf;
static uint64_t last_dump;
static uint64_t clock_ms(void) { return (uint64_t)now_ms(); }
static void sync_epoch(void)
{
    if (epoch != cur_epoch) {
        cb_reset(&combat); memset(&reports, 0, sizeof(reports));
        memset(&outgoing, 0, sizeof(outgoing));
        cbh_reset(&hud); epoch = cur_epoch;
    }
}
static unsigned int target_id(BYTE *g)
{
    int slot = gob_slot(g);
    return role_host ? (slot >= 0 ? gob_handle(slot) : 0) : ent_netid(g);
}
void combat_report(BYTE *from, BYTE *target, const char *name, int damage, int type, int part)
{
    struct event e;
    sync_epoch();
    /* Zero is a real hit: the blower pushes with 0 damage (type 0x21) and the host applies the push. */
    if (damage < 0 || damage > 32767 || !cb_attack_valid(type, part)) return;
    event_init(&e, EV_HIT);
    e.damage = (short)damage; e.type = type; e.part = (signed char)part;
    e.objtype = gob_is_projectile(from) ? gob_type(from) : (type | 2048);
    e.netid = target_id(target); e.extra[1] = gob_type(target);
    if (!++source_serial) ++source_serial;
    e.extra[3] = source_serial;
    memcpy(e.pos, target + GOB_POSITION, sizeof(e.pos));
    if (gob_is_actor(target)) {
        struct cb_event c;
        memset(&c, 0, sizeof(c));
        c.id = source_serial; c.target = e.netid; c.kind = e.extra[1];
        c.epoch = cur_epoch; c.attacker = (unsigned char)my_slot;
        c.amount = damage; c.weapon = e.objtype;
        memcpy(c.pos, e.pos, sizeof(c.pos));
        if (!cb_validate(&outgoing, &c, my_slot, CB_REPORT, cur_epoch, clock_ms())) return;
        e.damage = (short)c.amount;
    }
    snprintf(e.name, sizeof(e.name), "%s", name);
    event_send(&e, TO_ALL);
}
/* Called after Lua resolved the gob; identities/positions are compared to the live host world. */
int combat_allow_hit(struct event *e, int sender, BYTE *target)
{
    struct cb_event c;
    float *p = (float *)(target + GOB_POSITION);
    sync_epoch();
    if (!role_host || sender <= 0 || sender >= MAX_PLAYERS || !gob_alive(puppets[sender])) return 0;
    if (!cb_attack_valid(e->type, e->part)) return 0;
    /* Preserve existing non-enemy interactions; only enemies enter combat telemetry. */
    if (!gob_is_actor(target)) return e->damage >= 0 && e->damage <= CB_MAX_DAMAGE;
    memset(&c, 0, sizeof(c));
    c.id = e->extra[3]; c.target = e->netid; c.epoch = e->epoch;
    c.attacker = (unsigned char)sender; c.amount = e->damage;
    c.kind = e->extra[1]; c.weapon = e->objtype;
    memcpy(c.pos, e->pos, sizeof(c.pos));
    if (!cb_validate_hit(&reports, &c, sender, cur_epoch, target_id(target), gob_type(target),
                         gob_hp(target), p, clock_ms())) return 0;
    e->damage = (short)c.amount;
    return 1;
}
void combat_applied(int attacker, BYTE *target, unsigned int id, int kind, int before,
                    int after, int weapon, const float *pos)
{
    struct cb_event c;
    struct event e;
    sync_epoch();
    if (!role_host || attacker < 0 || attacker >= MAX_PLAYERS || before <= 0 || after >= before) return;
    memset(&c, 0, sizeof(c));
    if (!++serial) ++serial;
    c.id = serial; c.target = id; c.kind = kind; c.epoch = cur_epoch;
    c.attacker = (unsigned char)attacker;
    c.amount = (unsigned short)(before - (after > 0 ? after : 0));
    c.weapon = weapon; c.flags = after <= 0 ? CB_KILL : 0;
    memcpy(c.pos, pos, sizeof(c.pos));
    if (!cb_accept(&combat, &c, attacker, CB_PUBLISH, cur_epoch, clock_ms())) return;
    cbh_note(&hud, &c, attacker == my_slot, clock_ms());
    dirty = 1;
    event_init(&e, EV_COMBAT);
    /* cb_event is exactly one EV_NAME field; no wire layout change. */
    memcpy(e.name, &c, sizeof(c));
    event_send(&e, TO_ALL);
}
void combat_event(const struct event *e, int sender)
{
    struct cb_event c;
    sync_epoch();
    if ((test_background && test_deaf) || role_host || e->epoch != cur_epoch) return;
    memcpy(&c, e->name, sizeof(c));
    if (cb_accept(&combat, &c, sender, CB_CONFIRM, cur_epoch, clock_ms())) {
        cbh_note(&hud, &c, c.attacker == my_slot, clock_ms());
        dirty = 1;
    }
}
const struct cb_state *combat_state(void) { sync_epoch(); return &combat; }
const struct cbh_state *combat_hud(void) { sync_epoch(); return &hud; }
void combat_dump(FILE *f)
{
    unsigned i;
    int p;
    sync_epoch();
    fprintf(f, "combat %u %u %d %d %d\n", cur_epoch, combat.size, meter, 0, 0);
    for (p = 0; p < MAX_PLAYERS; p++)
        fprintf(f, "combat_player %d %llu %llu %.3f %.3f\n", p + 1,
                (unsigned long long)combat.total[p], (unsigned long long)combat.fight[p],
                cb_rolling(&combat, p, clock_ms()), cb_fight_dps(&combat, p, clock_ms()));
    for (i = 0; i < combat.size; i++) {
        const struct cb_event *c = &combat.lines[(combat.next + CB_KEEP - combat.size + i) % CB_KEEP].e;
        fprintf(f, "combat_event %u %u %d %u %u %u %u %u %.3f %.3f %.3f\n", c->epoch,
                c->id, c->attacker + 1, c->target, c->kind, c->amount, c->weapon, c->flags,
                c->pos[0], c->pos[1], c->pos[2]);
    }
}
static void dump_file(void)
{
    char path[MAX_PATH + 16]; FILE *f;
    snprintf(path, sizeof(path), "%s\\combat.txt", coop_dir);
    if ((f = fopen(path, "w"))) { combat_dump(f); fclose(f); }
}
static void reset_meter(void)
{
    /* Keep replay protection and history through a local presentation reset. */
    memset(combat.total, 0, sizeof(combat.total)); memset(combat.fight, 0, sizeof(combat.fight));
    memset(combat.buckets, 0, sizeof(combat.buckets)); memset(combat.ticks, 0, sizeof(combat.ticks));
    combat.started = 0; dirty = 1;
}
void combat_frame(int allowed)
{
    static const int codes[3] = {50, 19, 32};
    static const int vks[3] = {'M', 'R', 'D'};
    int i, ctrl;
    sync_epoch();
    ctrl = allowed && !chat_blocks_keys() && (input_down(29, VK_CONTROL) || input_down(157, VK_CONTROL));
    for (i = 0; i < 3; i++) {
        int down = ctrl && input_down(codes[i], vks[i]);
        if (down && !keys[i]) {
            if (i == 0) { meter = !meter; cbh_peek(&hud, clock_ms()); }
            if (i == 1) reset_meter();
            if (i == 2) dump_file();
        }
        keys[i] = down;
    }
    if (dirty && clock_ms() - last_dump >= 1000) {
        dump_file(); dirty = 0; last_dump = clock_ms();
    }
    for (i = 1; i < MAX_PLAYERS; i++) if (!player_connected(i)) {
        reports.seen[i] = reports.newest[i] = reports.count[i] = 0;
    }
}
int combat_meter(void) { return meter; }
static void l_dump(void) { dump_file(); }
static void l_reset(void) { reset_meter(); }
static void l_test(void)
{
    if (test_background) test_deaf = (int)lua_getnumber(lua_getparam(1));
}
void combat_lua_init(void)
{
    tolua_function(NULL, "coop_combat_test", l_test);
    tolua_function(NULL, "coop_combat_dump", l_dump);
    tolua_function(NULL, "coop_combat_reset", l_reset);
}
