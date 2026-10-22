/* Real revive state machine against a fake clock, world, input and reliable event sink. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../bridge.h"

static void test_position(void *gob, float x, float y, float z);
#undef gob_set_position
#define gob_set_position test_position
#ifndef REVIVE_SOURCE
#define REVIVE_SOURCE "../revive.c"
#endif
#include REVIVE_SOURCE

int role_host, have_remote, my_slot, cur_level, local_max_hp;
int rule_down_secs = 30, rule_revive_secs = 3;
BYTE *local_player, *puppets[MAX_PLAYERS];
static BYTE objects[MAX_PLAYERS][0xc0];
static int connected[MAX_PLAYERS], lives[MAX_PLAYERS], revivers[MAX_PLAYERS];
static int health = 100, mirrored, revive_key, click, zone_enemies, sent, destination;
static int checks, pose_id;
static struct event emitted;
static double clock_ms = 1000;
static char hud_line[128];

#define CHECK(expr) do { checks++; if (!(expr)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); exit(1); } } while (0)

double now_ms(void) { return clock_ms; }
void logmsg(const char *fmt, ...) { }
int gob_alive(BYTE *gob) { return gob != NULL; }
int gob_hp(BYTE *gob) { return health; }
int gob_class(BYTE *gob) { return 1; }
void gob_set_hp(BYTE *gob, int hp) { health = hp; }
void gob_set_mirrored(BYTE *gob, int on) { mirrored = on; }
void class_filter_think(int cls) { }
void anim_apply(BYTE *gob, const struct snap *snap) { pose_id = snap->cur; }
int player_connected(int slot) { return connected[slot]; }
int player_life(int slot) { return lives[slot]; }
int player_reviving(int slot) { return revivers[slot]; }
const char *player_name(int slot) { return slot == 0 ? "Host" : "Teammate"; }
int players_connected(void)
{
    int n = 0, s;
    for (s = 0; s < MAX_PLAYERS; s++) if (s != my_slot && connected[s]) n++;
    return n;
}
int ents_alive_in_zone(const float *position) { return zone_enemies; }
int gamekey_down(int which) { return revive_key; }
int input_down(int code, int vk) { return click; }
void hud_status(const char *a, const char *b, float progress)
{ snprintf(hud_line, sizeof(hud_line), "%s", a ? a : ""); }
void event_init(struct event *event, int kind)
{ memset(event, 0, sizeof(*event)); event->kind = kind; }
void event_send(const struct event *event, int to)
{ sent++; emitted = *event; destination = to; }
static void test_position(void *gob, float x, float y, float z)
{
    float position[] = {x, y, z};
    memcpy((BYTE *)gob + GOB_POSITION, position, sizeof(position));
}
static int action(const char *expected)
{
    const char *a;
    int found = 0;
    while ((a = revive_tick())) if (!strcmp(a, expected)) found++;
    return found;
}
static void advance(double ms) { clock_ms += ms; revive_frame(); }
static void setup(int host, int people)
{
    int s;
    role_host = host; my_slot = host ? 0 : 1; cur_level = 1;
    have_remote = people > 1; health = local_max_hp = 100;
    revive_key = click = zone_enemies = sent = mirrored = pose_id = 0;
    rule_down_secs = 30; rule_revive_secs = 3;
    memset(connected, 0, sizeof(connected)); memset(lives, 0, sizeof(lives));
    memset(objects, 0, sizeof(objects)); memset(puppets, 0, sizeof(puppets));
    for (s = 0; s < MAX_PLAYERS; s++) revivers[s] = -1;
    local_player = objects[my_slot];
    for (s = 0; s < people; s++) {
        connected[s] = 1;
        *(float *)(objects[s] + GOB_ORIENTATION) = 1;
        test_position(objects[s], 3.0f * s, 0, 0);
        if (s != my_slot) puppets[s] = objects[s];
    }
    revive_level_reset();
    action("up");
    clock_ms += 1000;
    revive_frame();
    sent = 0;
}
static void receive(int kind, int by, int target)
{
    struct event e;
    event_init(&e, kind); e.type = by; e.damage = target;
    revive_event(&e, 0);
}

static void damage_and_bleed(void)
{
    int i;
    setup(0, 2);
    CHECK(!revive_hit(99, 2) && revive_life() == LIFE_ALIVE);
    CHECK(revive_hit(100, 2) && revive_life() == LIFE_DOWNED);
    CHECK(mirrored && action("down") == 1);
    CHECK(revive_hit(999, 64) && revive_life() == LIFE_DOWNED);
    /* A long pause/load contributes at most 250 ms to bleeding. */
    advance(60000);
    CHECK(revive_life() == LIFE_DOWNED && pose_id == 17);
    /* A living teammate holding revive pauses bleed-out, even past its deadline. */
    revivers[0] = my_slot;
    for (i = 0; i < 140; i++) advance(250);
    CHECK(revive_life() == LIFE_DOWNED);
    revivers[0] = -1;
    for (i = 0; i < 120; i++) advance(250);
    CHECK(revive_life() == LIFE_DEAD);
    CHECK(sent == 1 && emitted.kind == EV_DIED && emitted.type == my_slot && destination == 0);
}
static void unreachable_and_spectate(void)
{
    int types[] = {64, 128, 256, 2048}, i;
    for (i = 0; i < 4; i++) {
        setup(0, 3);
        CHECK(revive_hit(999, types[i]) && revive_life() == LIFE_DEAD);
        CHECK(action("down") == 1 && mirrored);
        advance(2499); CHECK(revive_life() == LIFE_DEAD);
        advance(2); CHECK(revive_life() == LIFE_SPECTATING && action("hide") == 1);
        CHECK(((float *)(local_player + GOB_POSITION))[0] == 0);
        click = 1; advance(1);
        CHECK(((float *)(local_player + GOB_POSITION))[0] == 6);
        advance(1); CHECK(((float *)(local_player + GOB_POSITION))[0] == 6); /* held click is not another edge */
        receive(EV_RESPAWN, 1, my_slot);
        CHECK(revive_life() == LIFE_ALIVE && health == 50 && !mirrored);
        CHECK(action("up") == 1);
    }
    setup(0, 2);
    revive_unreachable("test void");
    CHECK(revive_life() == LIFE_DEAD && emitted.kind == EV_DIED);
}
static void incoming_revive(void)
{
    setup(0, 2); revive_hit(999, 2); action("down");
    test_position(puppets[0], 20, 0, 0);
    receive(EV_REVIVE, 0, my_slot);
    CHECK(revive_life() == LIFE_DOWNED);
    test_position(puppets[0], 0, 0, 0);
    receive(EV_REVIVE, 0, my_slot);
    CHECK(revive_life() == LIFE_ALIVE && health == 50 && action("up") == 1);
    receive(EV_REVIVE, 0, my_slot);
    CHECK(action("up") == 0); /* reliable replay cannot revive twice */
    revive_hit(999, 2);
    receive(EV_RESPAWN, 0, my_slot); CHECK(revive_life() == LIFE_DOWNED);
    receive(EV_RESPAWN, 1, my_slot); CHECK(revive_life() == LIFE_ALIVE);
}
static void revive_hold(void)
{
    setup(1, 2); lives[1] = LIFE_DOWNED; revive_key = 1;
    advance(1); advance(2999); CHECK(sent == 0);
    advance(2); CHECK(sent == 1 && emitted.kind == EV_REVIVE && emitted.damage == 1 && destination == 1);
    advance(4000); advance(4000); CHECK(sent == 1); /* require key release before another revive */
    revive_key = 0; advance(1); revive_key = 1; advance(1); advance(3001);
    CHECK(sent == 2);
    setup(1, 2); lives[1] = LIFE_DOWNED;
    test_position(puppets[1], 30, 0, 0); revive_key = 1;
    advance(1); advance(4000); CHECK(sent == 0);
}
static void checkpoint_and_zone(void)
{
    int i;
    setup(1, 3); lives[1] = LIFE_DOWNED; lives[2] = LIFE_DEAD;
    revive_checkpoint(); CHECK(sent == 2 && emitted.kind == EV_RESPAWN && emitted.type == 1 && destination == 2);
    setup(0, 2); lives[0] = LIFE_DOWNED; revive_hit(999, 2);
    revive_checkpoint(); CHECK(sent == 0 && revive_life() == LIFE_DOWNED); /* a client cannot authorize either player */
    setup(1, 2); revive_hit(999, 64); revive_checkpoint();
    CHECK(revive_life() == LIFE_ALIVE && health == 50 && ((float *)(local_player + GOB_POSITION))[0] == 3);
    setup(1, 2); lives[1] = LIFE_DEAD;
    { struct event e; event_init(&e, EV_DIED); e.type = 1; revive_event(&e, 1); }
    zone_enemies = 1;
    for (i = 0; i < 16; i++) advance(250);
    CHECK(sent == 0);
    zone_enemies = 0; advance(1); advance(1999); CHECK(sent == 0);
    advance(2); CHECK(sent == 1 && emitted.kind == EV_RESPAWN && emitted.type == 0 && destination == 1);
    advance(3000); CHECK(sent == 1);
}
static void wipe_and_solo(void)
{
    setup(1, 2); lives[1] = LIFE_DOWNED; revive_hit(999, 2); action("down");
    advance(1); advance(2999); CHECK(action("wipe") == 0);
    advance(2); CHECK(action("wipe") == 1 && revive_take_forced() == 1 && revive_take_forced() == 0);
    advance(4000); CHECK(action("wipe") == 0);
    revive_level_reset(); CHECK(revive_life() == LIFE_ALIVE);
    setup(1, 1); CHECK(revive_hit(999, 64) == 0 && revive_life() == LIFE_ALIVE);
    revive_unreachable("solo void"); CHECK(revive_life() == LIFE_ALIVE);
    setup(0, 2); revive_hit(999, 2); action("down");
    have_remote = 0; advance(1);
    CHECK(revive_life() == LIFE_ALIVE && action("die") == 1);
}
int main(void)
{
    damage_and_bleed(); unreachable_and_spectate(); incoming_revive();
    revive_hold(); checkpoint_and_zone(); wipe_and_solo();
    printf("PASS: %d revive state-machine checks\n", checks);
    return 0;
}
