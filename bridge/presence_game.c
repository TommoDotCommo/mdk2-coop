/* Engine adapters for attached visuals, host AI queries and the client waiting UI. */
#include <assert.h>
#include <string.h>
#include "bridge.h"

#ifndef GOB_TARGET
#define GOB_TARGET          0x88
#endif
#ifndef PLAYER_HANDLE
#define PLAYER_HANDLE       (*(DWORD *)ADDR.player_handle)
#endif
#ifndef AI_TARGET_HANDLE
#define AI_TARGET_HANDLE    (*(DWORD *)(ADDR.player_handle - 0x20))
#endif
#ifndef GOB_TABLE_AT
#define GOB_TABLE_AT(i)     ((BYTE *)GOB_TABLE[(i) * 2])
#endif

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
    if (state == anim_state(gob, 66)) return 66;
    if (state == anim_state(gob, 67)) return 67;
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
        DWORD *state = anim_state(child, 66 + i);
        if (state) memcpy(s->anim[i], state, sizeof(s->anim[i]));
    }
}

static int no_canopy, no_targeting, no_direct_hurt, melee_only, store_idle;
void canopy_apply(BYTE *gob, const struct canopy_state *s)
{
    BYTE *child = canopy_gob(gob), *ctl;
    int i;
    if (no_canopy || !child || !s->present || !(ctl = *(BYTE **)(child + GOB_ANIM))) return;
    for (i = 0; i < 2; i++) {
        DWORD *state = anim_state(child, 66 + i);
        if (state) memcpy(state, s->anim[i], sizeof(s->anim[i]));
    }
    *(WORD *)(ctl + 0x3e) = (*(WORD *)(ctl + 0x3e) & ~0x22) | s->flags;
    *(DWORD *)(ctl + 0x28) = s->cur == NO_ANIM ? 0 : (DWORD)(uintptr_t)anim_state(child, s->cur);
    *(DWORD *)(ctl + 0x2c) = s->prev == NO_ANIM ? 0 : (DWORD)(uintptr_t)anim_state(child, s->prev);
}

static BYTE *ai_actor;
static BYTE *(*target_original)(void);
static struct { DWORD actor, victim; unsigned int queries, hits; } ai_evidence[GOB_SLOTS];
static struct { DWORD actor; unsigned int shots[MAX_PLAYERS]; } ranged_evidence[GOB_SLOTS];
static unsigned int local_direct_hits;
static char local_direct_source[EV_NAME];

#define AI_STACK_MAX 16
static struct {
    BYTE *actor;
    DWORD player_handle, target_handle, ret;
} ai_stack[AI_STACK_MAX];
static int ai_depth;

static BYTE *base_target(void)
{
    if (target_original) return target_original();
    return AI_TARGET_HANDLE ? gob_by_handle(AI_TARGET_HANDLE) : gob_by_handle(PLAYER_HANDLE);
}

static BYTE *ai_select(const float *pos, BYTE *actor)
{
    struct presence_player candidates[MAX_PLAYERS] = {0};
    BYTE *bodies[MAX_PLAYERS] = {0};
    BYTE *fallback = base_target();
    int s, best;
    /* Scripted targets (boss objects, movie gobs, decoys) retain their authority. */
    if (!role_host || no_targeting || !have_remote || (actor ? !gob_alive(actor) : !pos) ||
        (fallback && fallback != local_player && puppet_slot(fallback) < 0)) return fallback;
    for (s = 0; s < MAX_PLAYERS; s++) {
        bodies[s] = s == my_slot ? local_player : puppets[s];
        if (!gob_alive(bodies[s]) || (s != my_slot && !player_in_level(s)) ||
            (s == my_slot ? revive_life() : player_life(s)) != LIFE_ALIVE) continue;
        candidates[s].eligible = 1;
        memcpy(candidates[s].pos, bodies[s] + GOB_POSITION, sizeof(candidates[s].pos));
    }
    best = presence_nearest(pos, candidates, MAX_PLAYERS);
    return best >= 0 ? bodies[best] : NULL;
}

/* The sight check at 0x402a40 (three callers) reads gob+0x84 -> field +8 of the target this getter
 * returns and faults when a gob that is still in the table has no game object yet (a level being
 * built) or no longer (a doomed one): mdk2hd.exe+0x2a95 reading 8. The native getter returns such a
 * gob as it is. No target is the answer its callers already handle (the getter itself returns
 * none for a downed hero). */
static unsigned int ai_infoless_targets;
int ai_guard_on = 1;                    /* the test mutant (coop_ai_guard in lua.c) turns the guard off */
static BYTE *ai_target_ready(BYTE *target)
{
    if (ai_guard_on && (!target || !*(BYTE **)(target + GOB_INFO))) {
        if (target && ai_infoless_targets++ < 8)
            logmsg("ai: target gob %p '%s' flags %04x has no game object (level loading?): none given; AI target handle %08x",
                   (void *)target, gob_name(target), *(unsigned short *)(target + 0xa6), (unsigned)AI_TARGET_HANDLE);
        return NULL;
    }
    return target;
}

/* Test hook (background test mode, coop_test_infoless_target in lua.c): the named gob is made the AI target
 * and loses its game object for a while, the state in which the sight check crashed. */
static int planted_slot = -1;
static BYTE *planted_info;

int presence_test_plant(const char *name)
{
    int i;
    if (planted_slot >= 0) {                      /* restore the previous one */
        BYTE *old = (BYTE *)GOB_TABLE[planted_slot * 2];
        if (old) *(BYTE **)(old + GOB_INFO) = planted_info;
        planted_slot = -1;
    }
    if (!name || !*name) return 0;
    for (i = 0; i < GOB_SLOTS; i++) {
        BYTE *gob = (BYTE *)GOB_TABLE[i * 2];
        if (!gob || gob == local_player || puppet_slot(gob) >= 0 || !*(BYTE **)(gob + GOB_INFO) ||
            !strstr(gob_name(gob), name)) continue;
        planted_slot = i;
        planted_info = *(BYTE **)(gob + GOB_INFO);
        *(BYTE **)(gob + GOB_INFO) = NULL;
        AI_TARGET_HANDLE = gob_handle(i);
        logmsg("test: planted AI target %s in slot %d without a game object", gob_name(gob), i);
        return 1;
    }
    return 0;
}

static unsigned int ai_queries;
/* who asks for the target (return addresses of the getter, the first 16 seen) and how often, and how often the test's
 * planted gob was handed out: state evidence for tests/qa_ai_target_guard.sh */
static struct { void *ra; unsigned int n; } ai_callers[16];
static unsigned int ai_planted_served;

static BYTE *ai_target(void);

/* Test hook (coop_test_sight in lua.c): what the native callers at 0x434e16 do for an actor, on demand. Ask for the
 * AI target through the hooked getter and, when there is one, run the sight check 0x402a40 on it. With the test's
 * planted gob (no game object) and the guard off this is the fault at 0x2a95. Overhaul build only (checked bytes). */
int presence_test_sight(const char *name)
{
    static const BYTE entry[8] = { 0x83, 0xec, 0x48, 0x53, 0x8b, 0x5c, 0x24, 0x50 };
    int (*sight)(BYTE *, BYTE *, float, float) = (int (*)(BYTE *, BYTE *, float, float))0x402a40;
    BYTE *actor = NULL, *target;
    int i;
    if (memcmp((BYTE *)0x402a40, entry, sizeof(entry))) { logmsg("test: sight check not at 0x402a40 in this build"); return -1; }
    for (i = 0; i < GOB_SLOTS && name && *name; i++) {
        BYTE *gob = (BYTE *)GOB_TABLE[i * 2];
        if (gob && gob != (BYTE *)GOB_TABLE[planted_slot < 0 ? 0 : planted_slot * 2] && *(BYTE **)(gob + GOB_INFO) &&
            strstr(gob_name(gob), name)) { actor = gob; break; }
    }
    if (!actor) { logmsg("test: no actor named %s", name ? name : ""); return -1; }
    target = ((BYTE *(*)(void))0x42ad50)();
    if (!target) { logmsg("test: sight check skipped, no AI target (guard)"); return 0; }
    logmsg("test: sight check %s -> target %p (game object %p)", gob_name(actor), (void *)target, *(void **)(target + GOB_INFO));
    return sight(actor, target, *(float *)0x4c687c, *(float *)0x4c3048) != 0 ? 1 : 2;
}

static BYTE *ai_target(void)
{
    BYTE *target;
    ai_queries++;
    {
        void *ra = __builtin_return_address(0);
        int c;
        for (c = 0; c < 16 && ai_callers[c].ra && ai_callers[c].ra != ra; c++) ;
        if (c < 16) { ai_callers[c].ra = ra; ai_callers[c].n++; }
    }
    if (planted_slot >= 0) AI_TARGET_HANDLE = gob_handle(planted_slot);
    target = ai_target_ready(ai_select(ai_actor ? (const float *)(ai_actor + GOB_POSITION) : NULL, ai_actor));
    int index = ai_actor ? gob_slot(ai_actor) : -1;
    if (planted_slot >= 0 && target && target == (BYTE *)GOB_TABLE[planted_slot * 2]) ai_planted_served++;
    if (index >= 0 && role_host && have_remote) {
        int victim = target ? gob_slot(target) : -1;
        if (ai_evidence[index].actor != gob_handle(index)) memset(&ai_evidence[index], 0, sizeof(ai_evidence[index]));
        ai_evidence[index].actor = gob_handle(index);
        ai_evidence[index].victim = victim >= 0 ? gob_handle(victim) : 0;
        ai_evidence[index].queries++;
    }
    return target;
}

static void ai_store_target(BYTE *gob, BYTE *target)
{
    int slot = target ? gob_slot(target) : -1;
    /* Native ranged fire resolves gob+0x88 through the handle table. */
    if (gob && slot >= 0) *(DWORD *)(gob + GOB_TARGET) = gob_handle(slot);
}

/* A gob that fires at players and is not one: actors, and scenery guns that shoot from
 * their animation (the level 1 boss's ring turrets, turret01_xx / turret02_xx, are
 * OBJ_SCENERY on l1_r8outerguns and aim at the engine's player, which is the host). */
static int world_shooter(BYTE *gob)
{
    return gob_alive(gob) && gob != local_player && puppet_slot(gob) < 0 && !gob_is_projectile(gob);
}

/* Some ranged tasks pass the game's cached host target directly to launch.
 * Keep the native decision to fire and native ballistics, but use the same
 * eligible player as acquisition; explicit scripted non-player targets win.
 * A shooter that is not an actor is only retargeted when it aims at a player. */
BYTE *ai_projectile_target(BYTE *shooter, BYTE *target)
{
    BYTE *selected;
    if (!role_host || !have_remote || no_targeting || !world_shooter(shooter)) return target;
    if (target && target != local_player && puppet_slot(target) < 0) return target;
    if (!target && !gob_is_actor(shooter)) return target;
    selected = ai_select((const float *)(shooter + GOB_POSITION), shooter);
    return selected ? selected : target;
}

/* Called from captured native launch state, not from target candidate seeding. */
void presence_projectile(BYTE *shooter, BYTE *target)
{
    int actor = gob_slot(shooter), player = target == local_player ? my_slot : puppet_slot(target);
    if (!role_host || actor < 0 || !world_shooter(shooter) || player < 0 || player >= MAX_PLAYERS) return;
    if (ranged_evidence[actor].actor != gob_handle(actor)) memset(&ranged_evidence[actor], 0, sizeof(ranged_evidence[actor]));
    ranged_evidence[actor].actor = gob_handle(actor);
    ranged_evidence[actor].shots[player]++;
}

static void ai_scope_handles(BYTE *target)
{
    int tslot = target ? gob_slot(target) : -1;
    if (tslot >= 0) {
        DWORD h = gob_handle(tslot);
        if (target == local_player || puppet_slot(target) >= 0) PLAYER_HANDLE = h;
        AI_TARGET_HANDLE = h;
    }
}

static void ai_scope_enter(BYTE *gob, DWORD ret)
{
    BYTE *saved = ai_actor;
    DWORD saved_player = PLAYER_HANDLE, saved_target = AI_TARGET_HANDLE;

    if (role_host && gob_is_actor(gob)) ai_actor = gob;
    if (ai_depth < AI_STACK_MAX) {
        ai_stack[ai_depth].actor = saved;
        ai_stack[ai_depth].player_handle = saved_player;
        ai_stack[ai_depth].target_handle = saved_target;
        ai_stack[ai_depth].ret = ret;
        ai_depth++;
    }
    if (role_host && gob_is_actor(gob)) {
        BYTE *target = ai_select((const float *)(gob + GOB_POSITION), gob);
        ai_store_target(gob, target);
        ai_scope_handles(target);
    }
}

DWORD ai_leave(void)
{
    BYTE *saved = NULL, *gob = ai_actor;
    DWORD ret = 0;
    if (ai_depth > 0) {
        ai_depth--;
        saved = ai_stack[ai_depth].actor;
        PLAYER_HANDLE = ai_stack[ai_depth].player_handle;
        AI_TARGET_HANDLE = ai_stack[ai_depth].target_handle;
        ret = ai_stack[ai_depth].ret;
    }
    ai_actor = saved;
    (void)gob;
    return ret;
}

#if defined(__i386__) && defined(_WIN32)
void ai_return_thunk(void);
__asm__(
    ".text\n"
    ".globl _ai_return_thunk\n"
    "_ai_return_thunk:\n"
    "\tpushl %eax\n"
    "\tpushl %edx\n"
    "\tcall _ai_leave\n"
    "\tmovl %eax, %ecx\n"
    "\tpopl %edx\n"
    "\tpopl %eax\n"
    "\tjmpl *%ecx\n"
);
#endif

void ai_hook_return(BYTE *gob, DWORD *ret_slot)
{
    ai_scope_enter(gob, ret_slot ? *ret_slot : 0);
#if defined(__i386__) && defined(_WIN32)
    if (ret_slot) *ret_slot = (DWORD)ai_return_thunk;
#endif
}

/* The game's ScriptUpdate executes native attack tasks after class think has
 * returned. Scope that path too; the Lua wrapper uses protected call so errors
 * cannot leave the engine's player handles pointing at a puppet. */
static int ai_script_begin(BYTE *gob)
{
    if (!role_host || !have_remote || no_targeting || !gob_alive(gob) || !gob_is_actor(gob) || ai_depth >= AI_STACK_MAX)
        return 0;
    ai_scope_enter(gob, 0);
    return 1;
}

static void l_ai_begin(void)
{
    lua_pushnumber(ai_script_begin(tolua_getusertype(1, NULL)));
}

static void l_ai_end(void) { ai_leave(); }

/* Call a slot callback and verify/restore the stack pointer afterwards so both
 * caller-pop (__cdecl) and callee-pop (__stdcall / ret N) callbacks stay balanced. */
static DWORD __attribute__((noinline)) ai_call(uintptr_t original, BYTE *gob, DWORD a, DWORD b, DWORD c)
{
    DWORD result;
#if defined(__i386__)
    uintptr_t saved_sp, after_sp;
    DWORD args[4] = { (DWORD)gob, a, b, c };
    __asm__ volatile(
        "movl %%esp, %1\n\t"
        "pushl 12(%4)\n\t"
        "pushl 8(%4)\n\t"
        "pushl 4(%4)\n\t"
        "pushl 0(%4)\n\t"
        "call *%3\n\t"
        "movl %1, %%esp\n\t" /* restore caller stack for both caller-pop and callee-pop */
        "movl %%esp, %2\n\t"
        "movl %1, %%esp\n\t"
        : "=a"(result), "=&r"(saved_sp), "=&r"(after_sp)
        : "r"(original), "r"(args)
        : "ecx", "edx", "cc", "memory"
    );
    assert(after_sp == saved_sp);
#elif defined(__x86_64__)
    uintptr_t saved_sp, after_sp;
    uintptr_t rdi_gob = (uintptr_t)gob, rsi_a = a, rdx_b = b, rcx_c = c;
    __asm__ volatile(
        "movq %%rsp, %1\n\t"
        "pushq %6\n\t"
        "pushq %5\n\t"
        "pushq %4\n\t"
        "pushq %3\n\t"
        "callq *%7\n\t"
        "movq %1, %%rsp\n\t" /* restore caller stack for both caller-pop and callee-pop */
        "movq %%rsp, %2\n\t"
        "movq %1, %%rsp\n\t"
        : "=a"(result), "=&r"(saved_sp), "=&r"(after_sp),
          "+D"(rdi_gob), "+S"(rsi_a), "+d"(rdx_b), "+c"(rcx_c)
        : "r"(original)
        : "r8", "r9", "r10", "r11", "cc", "memory"
    );
    assert(after_sp == saved_sp);
#else
    result = ((DWORD (*)(BYTE *, DWORD, DWORD, DWORD))original)(gob, a, b, c);
#endif
    return result;
}

/* Both think and animation-event callbacks ask for the target. Keep the context
 * scoped, including nested callbacks, and preserve stack balance across conventions. */
DWORD ai_run(BYTE *gob, DWORD original, DWORD a, DWORD b, DWORD c)
{
    DWORD result;
    ai_scope_enter(gob, 0);
    result = ai_call((uintptr_t)original, gob, a, b, c);
    ai_leave();
    return result;
}

void ai_create_enter(const float *pos)
{
    if (ai_depth < AI_STACK_MAX) {
        ai_stack[ai_depth].actor = ai_actor;
        ai_stack[ai_depth].player_handle = PLAYER_HANDLE;
        ai_stack[ai_depth].target_handle = AI_TARGET_HANDLE;
        ai_stack[ai_depth].ret = 0;
        ai_depth++;
    }
    if (role_host && !no_targeting && have_remote && pos)
        ai_scope_handles(ai_select(pos, NULL));
}

void ai_create_leave(BYTE *gob, const float *pos)
{
    ai_leave();
    if (!role_host || !gob || !gob_is_actor(gob)) return;
    class_filter_think(gob_class(gob));
    if (no_targeting || !have_remote) return;
    if (pos) gob_set_position(gob, pos[0], pos[1], pos[2]);
    ai_store_target(gob, ai_select(pos ? pos : (const float *)(gob + GOB_POSITION), gob));
}

void ai_before_scene(void)
{
    int i;
    if (!role_host) return;
    for (i = 0; i < GOB_SLOTS; i++) {
        BYTE *g = GOB_TABLE_AT(i);
        if (!g || !gob_alive(g) || !gob_is_actor(g)) continue;
        class_filter_think(gob_class(g));
        /* Do not write a target into every actor each frame: held or idle actors (the level 1
         * dogans wait on their platform) left their hold and fell after a resumed co-op save.
         * The think/animation scope and the creation hook already give an actor its target.
         * store_idle restores the old per-frame store; only the adapter unit test sets it. */
        if (store_idle && !no_targeting && have_remote)
            ai_store_target(g, ai_select((const float *)(g + GOB_POSITION), g));
    }
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
    int index, victim;
    /* Environmental damage has no source gob; gob_slot(NULL) matches an empty table entry. */
    if (!from || !target) return 0;
    index = gob_slot(from); victim = gob_slot(target);
    if (index < 0 || victim < 0 || !world_shooter(from)) return 0;
    if (role_host && !applied) {
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
    tolua_function(NULL, "coop_ai_begin", l_ai_begin);
    tolua_function(NULL, "coop_ai_end", l_ai_end);
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
    char engine_player[EV_NAME];
    gob_peername(gob_by_handle(PLAYER_HANDLE), engine_player, sizeof(engine_player));
    fprintf(f, "ai_scope_depth %d\nengine_player %s\n", ai_depth, engine_player);
    fprintf(f, "direct_source %s\n", local_direct_source[0] ? local_direct_source : "none");
    fprintf(f, "ai_hook %d\njoin_screen %d\n", target_original != NULL, join_screen);
    fprintf(f, "ai_queries %u\nai_infoless_answers %u\nai_guard %d\n", ai_queries, ai_infoless_targets, ai_guard_on);
    fprintf(f, "ai_planted_served %u\n", ai_planted_served);
    {
        int c;
        for (c = 0; c < 16 && ai_callers[c].ra; c++) fprintf(f, "ai_caller %p %u\n", ai_callers[c].ra, ai_callers[c].n);
    }
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
        BYTE *actor = GOB_TABLE_AT(s);
        char name[EV_NAME];
        if (!actor || !gob_alive(actor) || !gob_is_actor(actor)) continue;
        gob_peername(gob_by_handle(*(DWORD *)(actor + GOB_TARGET)), name, sizeof(name));
        fprintf(f, "ai_target %x %d %s\n", gob_handle(s), gob_type(actor), name[0] ? name : "none");
    }
    for (s = 0; s < GOB_SLOTS; s++) {
        BYTE *shooter = GOB_TABLE_AT(s);
        int p;
        if (!shooter || !gob_alive(shooter) || ranged_evidence[s].actor != gob_handle(s)) continue;
        for (p = 0; p < MAX_PLAYERS; p++) if (ranged_evidence[s].shots[p])
            fprintf(f, "ai_fire %x %d %d %u %s\n", gob_handle(s), gob_type(shooter), p + 1,
                    ranged_evidence[s].shots[p], gob_name(shooter));
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
