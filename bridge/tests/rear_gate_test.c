/* Production reliable puzzle routing: rear-gate authority and reconnect history. */
#include "../bridge.h"
#include <string.h>
#ifndef REMOTE_SOURCE
#define REMOTE_SOURCE "../remote.c"
#endif
#include REMOTE_SOURCE
#undef lua_getparam
#undef lua_isnumber
#undef lua_getnumber
struct build_addrs ADDR;
int role_host = 1, cur_level = 2, test_background;
BYTE *local_player, *puppets[MAX_PLAYERS];
int gob_alive(BYTE *g) { return 0; }
int gob_type(BYTE *g) { return 0; }
int door_locked(BYTE *g) { return 0; }
int revive_life(void) { return 0; }
int rooms_in_zone(const float *a, const float *b) { return 0; }
void rooms_center(const char *n, float *out, int *found) { *found = 0; }
const char *player_name(int slot) { return ""; }
void chat_notice(const char *text) { }
static int sends, index_arg = 4, checks;
static struct event sent;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; } checks++; } while (0)
static unsigned int getparam(int n) { return 1; }
static int isnumber(unsigned int n) { return 1; }
static double getnumber(unsigned int n) { return index_arg; }
void logmsg(const char *fmt, ...) { }
void pickups_finish(const char *name) { }
void event_init(struct event *e, int kind) { memset(e, 0, sizeof(*e)); e->kind = kind; }
void event_send(const struct event *e, int to) { sends++; sent = *e; }
int main(void)
{
    struct event e;
    ADDR.lua_getparam = (DWORD)getparam;
    ADDR.lua_isnumber = (DWORD)isnumber;
    ADDR.lua_getnumber = (DWORD)getnumber;
    remote_reset();
    l_coop_puzzle();
    CHECK(sends == 1 && sent.type == 4 && sent.kind == EV_PUZZLE);
    l_coop_puzzle();
    CHECK(sends == 1);
    puzzle_resend(1);
    CHECK(sends == 2 && sent.type == 4);
    role_host = 0;
    l_coop_puzzle();
    CHECK(sends == 2); /* a client cannot commit the rear lock */
    event_init(&e, EV_PUZZLE); e.type = 4;
    puzzle_event(&e, 1);
    CHECK(pend_head == pend_tail);
    puzzle_event(&e, 0);
    CHECK(pend_head - pend_tail == 1 && pend[0].kill == 4);
    CHECK(!strcmp(pend[0].name, "l2_r8entrdoor06"));
    remote_reset();
    role_host = 1;
    puzzle_event(&e, 1);
    CHECK(pend_head == pend_tail); /* host rejects a forged client result */
    puzzle_resend(1);
    CHECK(sends == 2); /* level reset clears reconnect history */
    cur_level = 3;
    l_coop_puzzle(); puzzle_event(&e, 0); puzzle_resend(1);
    CHECK(sends == 2 && pend_head == pend_tail);
    cur_level = 1; index_arg = 1;
    l_coop_puzzle();
    CHECK(sends == 3 && sent.type == 1);
    role_host = 0; e.type = 1;
    puzzle_event(&e, 0);
    CHECK(pend_head - pend_tail == 1 && pend[0].kill == 3);
    CHECK(!strcmp(pend[0].name, "platportal1"));
    printf("PASS: %d puzzle routing checks\n", checks);
    return 0;
}
