/* Portable combat telemetry core. No engine pointers or platform dependencies. */
#ifndef COMBAT_CORE_H
#define COMBAT_CORE_H
#include <stdint.h>
#define CB_PLAYERS 16
#define CB_KEEP 512
#define CB_RATE 64
#define CB_MAX_DAMAGE 10000
#define CB_WINDOW 5000
#define CB_IDLE 8000
#define CB_KILL 1
#define CB_CRIT 2
#define CB_CONFIRM 0
#define CB_REPORT 1
#define CB_PUBLISH 2
/* 32 bytes; transported in the existing reliable event envelope. */
struct cb_event {
    uint32_t id, target;
    uint16_t epoch, amount, weapon, kind;
    uint8_t attacker, flags;
    uint16_t reserved;
    float pos[3];
};
typedef char cb_event_size_must_be_32[(sizeof(struct cb_event) == 32) ? 1 : -1];
struct cb_guard {
    uint32_t newest[CB_PLAYERS];
    uint64_t seen[CB_PLAYERS], second[CB_PLAYERS];
    unsigned count[CB_PLAYERS];
};
struct cb_line { struct cb_event e; uint64_t at; };
struct cb_state {
    struct cb_guard guard;
    struct cb_line lines[CB_KEEP];
    unsigned size, next;
    uint64_t total[CB_PLAYERS], fight[CB_PLAYERS];
    uint64_t buckets[CB_PLAYERS][51], ticks[51];
    uint64_t start, last;
    int started;
};
struct cb_layout { int x, y, width, row, feed_rows, meter_rows; };
void cb_reset(struct cb_state *s);
int cb_validate(struct cb_guard *g, struct cb_event *e, int sender, int host,
                uint16_t epoch, uint64_t now);
int cb_validate_hit(struct cb_guard *g, struct cb_event *e, int sender, uint16_t epoch,
                    uint32_t target, int kind, int hp, const float *pos, uint64_t now);
int cb_attack_valid(int type, int part);
int cb_accept(struct cb_state *s, struct cb_event *e, int sender, int host,
              uint16_t epoch, uint64_t now);
double cb_rolling(const struct cb_state *s, int slot, uint64_t now);
double cb_fight_dps(const struct cb_state *s, int slot, uint64_t now);
struct cb_layout cb_layout(int w, int h, int font);
#endif
