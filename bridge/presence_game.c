/* Engine adapters for attached visuals, host AI queries and the client waiting UI. */
#include <string.h>
#include "bridge.h"

static double num_param(int n)
{
    unsigned int value = lua_getparam(n);
    return value && lua_isnumber(value) ? lua_getnumber(value) : 0;
}

/* Kurt's constructor (0x418240) creates and attaches a separate "chute" gob,
 * stores its generation-checked handle at character state +0x44, and starts
 * animation 67. Deploy/cut-away (0x416150/0x4161a0) play 66/67 on that child.
 * Copying only Kurt's animation controller cannot reproduce this. */
BYTE *canopy_gob(BYTE *gob)
{
    BYTE *info, *state;
    if (!gob_alive(gob) || gob_type(gob) != 100) return NULL;
    info = *(BYTE **)(gob + GOB_INFO);
    state = info ? *(BYTE **)(info + 0x40) : NULL;
    return state ? gob_by_handle(*(DWORD *)(state + 0x44)) : NULL;
}

static int canopy_anim(BYTE *gob, DWORD *state)
{
    if (!state) return NO_ANIM;
    if (state == om_anim_state(gob, 66)) return 66;
    if (state == om_anim_state(gob, 67)) return 67;
    return NO_ANIM;
}

void canopy_capture(BYTE *gob, struct canopy_state *s)
{
    BYTE *child = canopy_gob(gob), *ctl;
    int i;
    memset(s, 0, sizeof(*s));
    if (!child || !(ctl = *(BYTE **)(child + GOB_ANIM))) return;
    s->present = 1;
    s->flags = *(WORD *)(ctl + 0x3e) & 0x22;
    s->cur = canopy_anim(child, (DWORD *)(uintptr_t)*(DWORD *)(ctl + 0x28));
    s->prev = canopy_anim(child, (DWORD *)(uintptr_t)*(DWORD *)(ctl + 0x2c));
    for (i = 0; i < 2; i++) {
        DWORD *state = om_anim_state(child, 66 + i);
        if (state) memcpy(s->anim[i], state, sizeof(s->anim[i]));
    }
}

static int no_canopy, no_targeting, no_direct_hurt, melee_only;
void canopy_apply(BYTE *gob, const struct canopy_state *s)
{
    BYTE *child = canopy_gob(gob), *ctl;
    int i;
    if (no_canopy || !child || !s->present || !(ctl = *(BYTE **)(child + GOB_ANIM))) return;
    for (i = 0; i < 2; i++) {
        DWORD *state = om_anim_state(child, 66 + i);
        if (state) memcpy(state, s->anim[i], sizeof(s->anim[i]));
    }
    *(WORD *)(ctl + 0x3e) = (*(WORD *)(ctl + 0x3e) & ~0x22) | s->flags;
    *(DWORD *)(ctl + 0x28) = s->cur == NO_ANIM ? 0 : (DWORD)(uintptr_t)om_anim_state(child, s->cur);
    *(DWORD *)(ctl + 0x2c) = s->prev == NO_ANIM ? 0 : (DWORD)(uintptr_t)om_anim_state(child, s->prev);
}

static BYTE *ai_actor;
static BYTE *(*target_original)(void);
static struct { DWORD actor, victim; unsigned int queries, hits; } ai_evidence[GOB_SLOTS];
static unsigned int local_direct_hits;
static char local_direct_source[EV_NAME];
static BYTE *ai_target(void)
{
    struct presence_player candidates[MAX_PLAYERS] = {0};
    BYTE *bodies[MAX_PLAYERS] = {0};
    BYTE *fallback = target_original();
    int s, best, index;
    /* Scripted targets (boss objects, movie gobs, decoys) retain their authority. */
    if (!role_host || no_targeting || !have_remote || !gob_alive(ai_actor) ||
        (fallback && fallback != local_player && puppet_slot(fallback) < 0)) return fallback;
    for (s = 0; s < MAX_PLAYERS; s++) {
        bodies[s] = s == my_slot ? local_player : puppets[s];
        if (!gob_alive(bodies[s]) || (s != my_slot && !player_in_level(s)) ||
            (s == my_slot ? revive_life() : player_life(s)) != LIFE_ALIVE) continue;
        candidates[s].eligible = 1;
        memcpy(candidates[s].pos, bodies[s] + GOB_POSITION, sizeof(candidates[s].pos));
    }
    best = presence_nearest((float *)(ai_actor + GOB_POSITION), candidates, MAX_PLAYERS);
    index = gob_slot(ai_actor);
    if (index >= 0 && best >= 0) {
        if (ai_evidence[index].actor != gob_handle(index)) memset(&ai_evidence[index], 0, sizeof(ai_evidence[index]));
        ai_evidence[index].actor = gob_handle(index);
        ai_evidence[index].victim = gob_handle(gob_slot(bodies[best]));
        ai_evidence[index].queries++;
    }
    return best >= 0 ? bodies[best] : NULL;
}

/* Both think and animation-event callbacks ask for the target. Keep the context
 * scoped, including nested callbacks, and forward all four cdecl argument slots. */
DWORD ai_run(BYTE *gob, DWORD original, DWORD a, DWORD b, DWORD c)
{
    BYTE *saved = ai_actor;
    DWORD result;
    if (role_host && gob_is_actor(gob)) ai_actor = gob;
    result = ((DWORD (*)(BYTE *, DWORD, DWORD, DWORD))original)(gob, a, b, c);
    ai_actor = saved;
    return result;
}

void ai_init(void)
{
    BYTE expect[6] = {0xa1, 0, 0, 0, 0, 0x56};
    /* The getter is shared by both recognised builds. The expected operand is
     * checked before installing the hook, just as the existing native hooks are. */
    *(DWORD *)(expect + 1) = ADDR.player_handle - 0x20;
    target_original = hook_code((BYTE *)0x42ad50, expect, sizeof(expect), ai_target);
}

/* This evidence counts native actor -> selected player calls, never explosion or
 * projectile sources. Client receipts are counted after applying that HURT. */
int presence_hit(BYTE *from, BYTE *target, int applied)
{
    int index = gob_slot(from), victim = gob_slot(target);
    if (index < 0 || victim < 0 || !gob_is_actor(from)) return 0;
    if (role_host) {
        if (no_direct_hurt) return 1;
        if (ai_evidence[index].actor == gob_handle(index) &&
            ai_evidence[index].victim == gob_handle(victim)) ai_evidence[index].hits++;
    } else if (applied && target == local_player) {
        local_direct_hits++;
        gob_peername(from, local_direct_source, sizeof(local_direct_source));
    }
    return 0;
}

int presence_damage_allowed(void *caller)
{
    return !test_background || !melee_only || caller == (void *)0x434e56 || caller == (void *)0x434dfb;
}

static struct join_flow joining;
static int join_screen;
static void l_join_screen(void) { join_screen = (int)num_param(1); }
static void l_join_cancelled(void) { lua_pushnumber(net_join_cancelled()); }
void net_cancel_join(void);
static void l_join_tick(void)
{
    int level = (int)num_param(1), pulling = (int)num_param(2), cancel = (int)num_param(3);
    int phase;
    if (role_host) return;
    if (cancel) net_cancel_join();
    phase = join_step(&joining, now_ms() / 1000, host_silent_ms() < 2000,
                      host_level(), level, pulling, net_version_mismatch(), net_join_cancelled());
    lua_pushnumber(phase);
    lua_pushstring(join_text(phase));
}

static void l_presence_test(void)
{
    if (!test_background) return;
    no_canopy = (int)num_param(1);
    no_targeting = (int)num_param(2);
    no_direct_hurt = (int)num_param(3);
    melee_only = (int)num_param(4);
    net_test_legacy((int)num_param(5));
}

static void l_join_retry(void)
{
    net_retry_join();
    memset(&joining, 0, sizeof(joining));
    joining.since = now_ms() / 1000;
}

static void l_canopy(void)
{
    push_gob(canopy_gob(tolua_getusertype(1, NULL)));
}

void presence_lua_init(void)
{
    joining.since = now_ms() / 1000;
    tolua_function(NULL, "coop_canopy", l_canopy);
    tolua_function(NULL, "coop_join_cancelled", l_join_cancelled);
    tolua_function(NULL, "coop_join_screen", l_join_screen);
    tolua_function(NULL, "coop_join_tick", l_join_tick);
    tolua_function(NULL, "coop_join_retry", l_join_retry);
    tolua_function(NULL, "coop_presence_test", l_presence_test);
}

void presence_dump(FILE *f)
{
    int s;
    fprintf(f, "direct_source %s\n", local_direct_source[0] ? local_direct_source : "none");
    fprintf(f, "ai_hook %d\njoin_screen %d\n", target_original != NULL, join_screen);
    fprintf(f, "join %d\ndirect_receipts %u\nmelee_only %d\n", joining.phase, local_direct_hits, melee_only);
    for (s = 0; s < MAX_PLAYERS; s++) {
        BYTE *body = s == my_slot ? local_player : puppets[s];
        struct canopy_state canopy;
        canopy_capture(body, &canopy);
        if (canopy.present) {
            int j;
            fprintf(f, "canopy %d %u %u %u", s + 1, canopy.flags, canopy.cur, canopy.prev);
            for (j = 0; j < 2; j++) {
                float phase;
                memcpy(&phase, &canopy.anim[j][ANIM_TIME0], sizeof(phase));
                fprintf(f, " %lu %.5f", (unsigned long)canopy.anim[j][0], phase);
            }
            fputc('\n', f);
        }
    }
    for (s = 0; s < GOB_SLOTS; s++) {
        BYTE *actor = gob_by_handle(ai_evidence[s].actor), *victim;
        char name[EV_NAME];
        if (!actor || !ai_evidence[s].queries || !gob_is_actor(actor)) continue;
        victim = gob_by_handle(ai_evidence[s].victim);
        gob_peername(victim, name, sizeof(name));
        fprintf(f, "ai %s %s %u %u\n", gob_name(actor), name,
                ai_evidence[s].queries, ai_evidence[s].hits);
    }
}
