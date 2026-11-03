/* Exercise the actual engine adapter against a bounded fake gob table. */
#define BRIDGE_H
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include "snapshot.h"
typedef unsigned char BYTE;
typedef uint16_t WORD;
typedef uint32_t DWORD;
#define MAX_PLAYERS 3
#define GOB_SLOTS 16
#define EV_NAME 32
#define GOB_INFO 0x84
#define GOB_TARGET 0x88
#define GOB_ANIM 0x60
#define GOB_POSITION 0x18
#define LIFE_ALIVE 0
#define GOB_FLAGS 0xa6
#define GOB_BONE_SLOT 0x8c
#define GOB_ORIENTATION 0x24
#define ANIM_ID_MAX 160
#define MAX_PATH 260
static BYTE bodies[6][256], infos[6][256], states[6][256], controllers[6][256];
static DWORD animation[6][5][10], fake_gob_table[GOB_SLOTS * 2];
static DWORD fake_player_handle, fake_ai_target_handle;
#define GOB_TABLE fake_gob_table
#define GOB_TABLE_AT(i) ((i) < 6 ? bodies[i] : NULL)
#define PLAYER_HANDLE fake_player_handle
#define AI_TARGET_HANDLE fake_ai_target_handle
static int types[6] = {100,100,100,202,0,202};
static int alive[6] = {1,1,1,1,1,1}, in_level[3] = {1,1,1}, life[3];
static char coop_dir[MAX_PATH] = ".";
static const char *names[6] = {"p0","p1","p2","enemy","",""};
static int role_host = 1, have_remote = 1, my_slot, test_background = 1;
static int filtered_classes, repositioned;
static BYTE *local_player = bodies[0], *puppets[3] = {NULL,bodies[1],bodies[2]};
static struct { DWORD player_handle; } ADDR = {0x5494c8};
/* Like the engine table, NULL matches an empty entry (index 6) instead of being rejected. */
static int gob_slot(BYTE *g) { int i; if(!g) return 6; for(i=0;i<6;i++) if(g==bodies[i]) return i; return -1; }
static DWORD gob_handle(int i) { return 0x10000u + i; }
static BYTE *gob_by_handle(DWORD h) { int i; for(i=0;i<6;i++) if(h==gob_handle(i) && alive[i]) return bodies[i]; return NULL; }
static int gob_alive(BYTE *g) { int i=gob_slot(g); return g && i>=0 && alive[i]; } /* NULL-safe, as in game.c */
static int gob_type(BYTE *g) { return types[gob_slot(g)]; }
static int gob_is_actor(BYTE *g) { assert(g); return gob_type(g)>=200 && gob_type(g)<260; }
static int gob_class(BYTE *g) { return gob_is_actor(g) ? 3 : 1; }
static int gob_is_projectile(BYTE *g) { return g && gob_type(g)>=400 && gob_type(g)<500; }
static void class_filter_think(int cls) { if (cls > 0) filtered_classes++; }
static void gob_set_position(void *g, float x, float y, float z) {
    float *p = (float *)((BYTE *)g + GOB_POSITION);
    p[0] = x; p[1] = y; p[2] = z;
    repositioned++;
}
static int puppet_slot(BYTE *g) { int i; for(i=0;i<3;i++) if(g && puppets[i]==g) return i; return -1; }
static int player_in_level(int s) { return in_level[s]; }
static int revive_life(void) { return life[0]; }
static int player_life(int s) { return life[s]; }
/* like the real one, every call returns the same buffer */
static const char *gob_name(BYTE *g) { static char buf[32]; snprintf(buf, sizeof(buf), "%s", names[gob_slot(g)]); return buf; }
static void gob_peername(BYTE *g, char *out, int size) { snprintf(out,size,"@p%d",gob_slot(g)+1); }
static DWORD *anim_state(BYTE *g, int id) {
    int k = id==66 ? 0 : id==67 ? 1 : id==62 ? 2 : id==77 ? 3 : id==65 ? 4 : -1;
    return k < 0 ? NULL : animation[gob_slot(g)][k];
}
static char *luaL_check_lstr_fake(int n, long *len) { *len = 1; return "k"; }
#define luaL_check_lstr luaL_check_lstr_fake
static void gob_kill(BYTE *g) { alive[gob_slot(g)] = 0; }
static unsigned int fake_ticks = 10000;
static unsigned int GetTickCount(void) { return fake_ticks; }
/* the game's equip natives, as fakes: a free fake gob, a model controller, a bone, attach, placement, animation */
static int bones_found, attaches, placed, played_anim, models_built, fail_bone, last_created = 4;
static const char *built_model;
static BYTE *om_gob_create_fake(const char *name) {
    int i; for (i = 4; i < 6; i++) if (!alive[i]) {
        memset(bodies[i], 0, sizeof(bodies[i])); alive[i] = 1; types[i] = 0; names[i] = name; last_created = i; return bodies[i]; }
    return NULL;
}
static void *om_model_new_fake(const char *name) { built_model = name; models_built++; memset(controllers[last_created], 0, 256); return controllers[last_created]; }
static int om_bone_find_fake(void *g, const char *name) { bones_found++; return fail_bone ? -1 : 19; }
static void om_attach_fake(int a, void *parent, void *child, int bone) {
    *(DWORD *)((BYTE *)child + GOB_TARGET) = gob_handle(gob_slot(parent)); *(int *)((BYTE *)child + GOB_BONE_SLOT) = bone;
    *(WORD *)((BYTE *)child + GOB_FLAGS) |= 0x20; attaches++;
}
static void om_bone_place_fake(void *g, int bone, float *pos, float *quat) { placed++; }
static int om_anim_play_fake(void *g, int id, int flags) { played_anim = id; return 1; }
#define om_gob_create om_gob_create_fake
#define om_model_new om_model_new_fake
#define om_bone_find om_bone_find_fake
#define om_attach om_attach_fake
#define om_bone_place om_bone_place_fake
#define om_anim_play om_anim_play_fake
static BYTE *fallback;
static BYTE *original_target(void) { return fallback; }
static void *hook_code(BYTE *fn, const BYTE *expect, int size, void *hook) {
    assert(fn==(BYTE *)0x42ad50 && size==6 && expect[0]==0xa1 && expect[5]==0x56);
    return original_target;
}
static unsigned int lua_getparam(int n) { return n; }
static int lua_isnumber(unsigned int n) { return 1; }
static double lua_getnumber(unsigned int n) { return 0; }
static void *tolua_getusertype(int n, void *def) { return NULL; }
static void tolua_function(const char *module, const char *name, void (*fn)(void)) { }
static void push_gob(BYTE *g) { }
static void lua_pushnumber(double n) { }
static void lua_pushstring(const char *s) { }
static void net_cancel_join(void) { }
static void net_test_legacy(int on) { }
static void net_retry_join(void) { }
static void logmsg(const char *fmt, ...) { (void)fmt; }
static int net_version_mismatch(void) { return 0; }
static int net_join_cancelled(void) { return 0; }
static double now_ms(void) { return 1000; }
static double host_silent_ms(void) { return 0; }
static int host_level(void) { return 1; }
#ifndef ADAPTER_SOURCE
#define ADAPTER_SOURCE "../presence_game.c"
#endif
#include ADAPTER_SOURCE
static BYTE *expected;
static DWORD fake_think(BYTE *g, DWORD a, DWORD b, DWORD c)
{
    assert(a==11 && b==22 && c==33);
    assert(ai_target()==expected);
    if (role_host && gob_is_actor(g)) {
        assert(gob_by_handle(*(DWORD *)(g + GOB_TARGET)) == expected);
        assert(AI_TARGET_HANDLE == gob_handle(gob_slot(expected)));
        if (expected == local_player || puppet_slot(expected) >= 0)
            assert(PLAYER_HANDLE == gob_handle(gob_slot(expected)));
    }
    return 42;
}

DWORD callee_pop_verify(uintptr_t g, uintptr_t a, uintptr_t b, uintptr_t c)
{
    return fake_think((BYTE *)g, (DWORD)a, (DWORD)b, (DWORD)c);
}

#if defined(__x86_64__)
void callee_pop_think(void);
__asm__(
    ".text\n"
    ".globl callee_pop_think\n"
    "callee_pop_think:\n"
    "\tmovq 8(%rsp), %rdi\n"
    "\tmovq 16(%rsp), %rsi\n"
    "\tmovq 24(%rsp), %rdx\n"
    "\tmovq 32(%rsp), %rcx\n"
    "\tsubq $8, %rsp\n"
    "\tcall callee_pop_verify\n"
    "\taddq $8, %rsp\n"
    "\tret $32\n"
);
#elif defined(__i386__)
void callee_pop_think(void);
__asm__(
    ".text\n"
    ".globl _callee_pop_think\n"
    ".globl callee_pop_think\n"
    "_callee_pop_think:\n"
    "callee_pop_think:\n"
    "\tpushl 16(%esp)\n"
    "\tpushl 16(%esp)\n"
    "\tpushl 16(%esp)\n"
    "\tpushl 16(%esp)\n"
    "\tcall _callee_pop_verify\n"
    "\taddl $16, %esp\n"
    "\tret $16\n"
);
#endif

int main(void)
{
    struct canopy_state a = {0}, b;
    float spawn_pos[3] = {2.5f, 0.0f, 0.0f};
    DWORD ret_slot = 0xdeadbeefu;
    int i;
    for(i=0;i<6;i++) {
        *(BYTE **)(bodies[i]+GOB_INFO)=infos[i];
        *(BYTE **)(infos[i]+0x40)=states[i];
        *(BYTE **)(bodies[i]+GOB_ANIM)=controllers[i];
    }
    *(DWORD *)(states[0]+0x44)=gob_handle(4);
    *(DWORD *)(states[1]+0x44)=gob_handle(4);
    *(DWORD *)(controllers[4]+0x28)=(DWORD)(uintptr_t)animation[4][0];
    *(DWORD *)(controllers[4]+0x2c)=(DWORD)(uintptr_t)animation[4][1];
    *(WORD *)(controllers[4]+0x3e)=0x22;
    animation[4][0][0]=1;
    canopy_capture(local_player,&a);
    assert(a.present && a.cur==66 && a.prev==67 && a.flags==0x22 && a.anim[0][0]==1);
    a.cur=67; a.prev=66; a.flags=2; a.anim[0][0]=0; a.anim[1][0]=1;
    canopy_apply(bodies[1],&a);
    canopy_capture(bodies[1],&b);
    assert(memcmp(&a,&b,sizeof(a))==0);
    types[1]=101; canopy_capture(bodies[1],&b); assert(!b.present); types[1]=100;
    alive[4]=0; assert(!canopy_gob(local_player)); alive[4]=1;
    ai_init(); fallback=local_player;
    PLAYER_HANDLE=gob_handle(0); AI_TARGET_HANDLE=gob_handle(0);
    *(float *)(bodies[0]+GOB_POSITION)=50;
    *(float *)(bodies[1]+GOB_POSITION)=2;
    *(float *)(bodies[2]+GOB_POSITION)=5;
    expected=bodies[1];
    assert(ai_run(bodies[3],(DWORD)(uintptr_t)fake_think,11,22,33)==42);
    assert(ai_target()==local_player); /* callback context was restored */
    assert(PLAYER_HANDLE==gob_handle(0) && AI_TARGET_HANDLE==gob_handle(0));
    assert(ai_evidence[3].queries==1 && ai_evidence[3].victim==gob_handle(1));
    assert(ai_run(bodies[3],(DWORD)(uintptr_t)callee_pop_think,11,22,33)==42);
    assert(PLAYER_HANDLE==gob_handle(0) && AI_TARGET_HANDLE==gob_handle(0));
    ai_hook_return(bodies[3], &ret_slot);
    assert(ai_target()==bodies[1] && PLAYER_HANDLE==gob_handle(1) && AI_TARGET_HANDLE==gob_handle(1));
    assert(ai_leave()==0xdeadbeefu && ai_target()==local_player && PLAYER_HANDLE==gob_handle(0));
    /* ScriptUpdate runs the native attack task outside the class think callback. */
    assert(ai_script_begin(bodies[3]) == 1);
    assert(ai_target() == bodies[1] && PLAYER_HANDLE == gob_handle(1));
    assert(ai_script_begin(bodies[5]) == 1); /* nested script */
    ai_leave();
    assert(ai_target() == bodies[1]);
    ai_leave();
    assert(ai_target() == local_player && PLAYER_HANDLE == gob_handle(0));
    assert(ai_script_begin(local_player) == 0 && ai_depth == 0);
    no_targeting = 1;
    assert(ai_script_begin(bodies[3]) == 0 && ai_depth == 0);
    no_targeting = 0;
    ai_create_enter(spawn_pos);
    assert(PLAYER_HANDLE==gob_handle(1) && AI_TARGET_HANDLE==gob_handle(1));
    *(DWORD *)(bodies[3]+GOB_TARGET)=0;
    ai_create_leave(bodies[3], spawn_pos);
    assert(PLAYER_HANDLE==gob_handle(0) && AI_TARGET_HANDLE==gob_handle(0));
    assert(gob_by_handle(*(DWORD *)(bodies[3]+GOB_TARGET))==bodies[1] && filtered_classes>0 && repositioned==1);
    *(DWORD *)(bodies[3]+GOB_TARGET)=gob_handle(0);
    /* A held or idle actor must not be handed a target every frame: that released the level 1
     * dogans from their hold after a resumed co-op save. */
    ai_before_scene();
    assert(*(DWORD *)(bodies[3]+GOB_TARGET)==gob_handle(0));
    store_idle=1; ai_before_scene(); store_idle=0; /* the old behaviour is the QA mutant */
    assert(gob_by_handle(*(DWORD *)(bodies[3]+GOB_TARGET))==bodies[1]);
    assert(ai_projectile_target(bodies[3], local_player)==bodies[1]);
    assert(ai_projectile_target(bodies[3], bodies[5])==bodies[5]); /* script target */
    assert(ai_projectile_target(local_player, local_player)==local_player); /* player aim */
    no_targeting=1; assert(ai_projectile_target(bodies[3], local_player)==local_player); no_targeting=0;
    /* Scenery guns (the level 1 ring turrets) aim at the engine's player from an animation:
     * they get the nearest player too, but a shot fired at nothing stays aimless. */
    assert(ai_projectile_target(bodies[4], local_player)==bodies[1]);
    assert(ai_projectile_target(bodies[4], NULL)==NULL);
    assert(ai_projectile_target(bodies[4], bodies[5])==bodies[5]);
    presence_projectile(bodies[4], bodies[1]); assert(ranged_evidence[4].shots[1]==1);
    no_direct_hurt=1; assert(presence_hit(bodies[4],bodies[1],0)==1); no_direct_hurt=0;
    {
        DWORD victim = ai_evidence[3].victim;
        unsigned int queries = ai_evidence[3].queries;
        *(float *)(bodies[1]+GOB_POSITION)=100;
        ai_before_scene();
        /* Candidate seeding is not evidence of a native target lookup. */
        assert(ai_evidence[3].victim==victim && ai_evidence[3].queries==queries);
        *(float *)(bodies[1]+GOB_POSITION)=2;
        ai_before_scene();
    }
    assert(ai_script_begin(NULL) == 0 && ai_depth == 0);
    assert(presence_hit(bodies[3],bodies[1],0)==0 && ai_evidence[3].hits==1);
    no_direct_hurt=1; assert(presence_hit(bodies[3],bodies[1],0)==1); no_direct_hurt=0;
    /* Environmental damage (nil attacker) crashed the host on the empty-entry match above. */
    assert(presence_hit(NULL,bodies[1],0)==0 && presence_hit(NULL,local_player,1)==0 && presence_hit(bodies[3],NULL,0)==0);
    life[1]=1; expected=bodies[2]; ai_run(bodies[3],(DWORD)(uintptr_t)fake_think,11,22,33); life[1]=0;
    in_level[1]=0; expected=bodies[2]; ai_run(bodies[3],(DWORD)(uintptr_t)fake_think,11,22,33); in_level[1]=1;
    fallback=bodies[5]; expected=fallback; ai_run(bodies[3],(DWORD)(uintptr_t)fake_think,11,22,33); /* scripted target */
    /* A target without a game object (a gob still being built or already doomed) crashed the native
     * sight check at 0x402a95: the getter must answer none, for every route to the target. */
    {
        BYTE *info = *(BYTE **)(bodies[5] + GOB_INFO);
        fallback=bodies[5]; assert(ai_target()==bodies[5]);
        *(BYTE **)(bodies[5] + GOB_INFO)=NULL;
        assert(ai_target()==NULL);
        role_host=0; assert(ai_target()==NULL); role_host=1;
        no_targeting=1; assert(ai_target()==NULL); no_targeting=0;
        *(BYTE **)(bodies[5] + GOB_INFO)=info;
        assert(ai_target()==bodies[5]);
    }
    fallback=local_player; no_targeting=1; expected=local_player; ai_run(bodies[3],(DWORD)(uintptr_t)fake_think,11,22,33);
    no_targeting=0; role_host=0; expected=local_player; ai_run(bodies[3],(DWORD)(uintptr_t)fake_think,11,22,33);
    presence_hit(bodies[3],local_player,1); assert(local_direct_hits==1);
    melee_only=1;
    assert(presence_damage_allowed((void *)0x434e56));
    assert(!presence_damage_allowed((void *)0x1234));
    test_background=0; assert(presence_damage_allowed((void *)0x1234));
    /* Equipment visuals (Max's jetpack, Doc's leaf blower): read from the owner's gob, built and driven on a puppet */
    {
        struct gear_state g = {0}, h;
        BYTE *child, *built;
        alive[4] = alive[5] = 0; types[0] = types[1] = 101; types[2] = 100;
        child = om_gob_create_fake("maxjetpack");              /* what the game's pickup code did for the owner */
        assert(child == bodies[4]);
        *(DWORD *)(child + GOB_TARGET) = gob_handle(0); *(int *)(child + GOB_BONE_SLOT) = 19;
        *(WORD *)(child + GOB_FLAGS) = 0x22;
        *(BYTE **)(child + GOB_ANIM) = controllers[4];
        memset(animation[4], 0, sizeof(animation[4])); memset(animation[5], 0, sizeof(animation[5]));
        animation[4][3][0] = 1;
        { float phase = 0.25f; memcpy(&animation[4][3][3], &phase, 4); memcpy(&animation[4][3][4], &phase, 4); }
        *(DWORD *)(controllers[4] + 0x28) = (DWORD)(uintptr_t)animation[4][3];     /* thrusting: anim 77, was 62 */
        *(DWORD *)(controllers[4] + 0x2c) = (DWORD)(uintptr_t)animation[4][2];
        *(DWORD *)(states[0] + 0x38) = 2;                                            /* the atomic jetpack */
        gear_capture(local_player, &g);
        assert(g.v[0].present && g.v[0].cur == 77 && g.v[0].prev == 62 && g.v[0].nanims == 1 && g.v[0].id[0] == 77);
        assert(g.v[0].model == 1 && !g.v[0].hidden && !g.v[1].present);
        *(WORD *)(child + GOB_FLAGS) = 0x2a; gear_capture(local_player, &h); assert(h.v[0].hidden);
        *(WORD *)(child + GOB_FLAGS) = 0x22;
        /* the puppet has none: it is built with the game's own calls, on the bone and parent, and driven */
        types[0] = 100; gear_capture(local_player, &h); assert(!h.v[0].present); types[0] = 101;    /* not Max */
        gear_apply(1, bodies[1], &g);
        built = gob_by_handle(gear_owned[1][0]);
        assert(built && gob_alive(built) && !strcmp(names[gob_slot(built)], "maxjetpack"));
        assert(*(DWORD *)(built + GOB_TARGET) == gob_handle(1) && *(int *)(built + GOB_BONE_SLOT) == 19);
        assert(!strcmp(built_model, "jetpackatm") && attaches == 1 && placed == 1 && played_anim == 62);
        gear_capture(bodies[1], &h);
        assert(h.v[0].present && h.v[0].cur == 77 && h.v[0].prev == 62 && h.v[0].nanims == 1);
        assert(memcmp(h.v[0].anim[0], g.v[0].anim[0], sizeof(h.v[0].anim[0])) == 0);
        /* the owner stops thrusting and hides it: the same gob follows, none is built twice */
        g.v[0].cur = 62; g.v[0].prev = 77; g.v[0].id[0] = 62; g.v[0].hidden = 1; g.v[0].anim[0][0] = 1;
        gear_apply(1, bodies[1], &g);
        assert(attaches == 1 && gob_by_handle(gear_owned[1][0]) == built);
        gear_capture(bodies[1], &h); assert(h.v[0].cur == 62 && h.v[0].prev == 77 && h.v[0].hidden);
        /* it ends on the owner's side: the puppet's goes too; a gob this side did not build is left alone */
        memset(&g, 0, sizeof(g)); gear_apply(1, bodies[1], &g);
        assert(!alive[gob_slot(built)] && !gear_owned[1][0]);
        /* a visual for a character that does not wear it is never built; a missing bone is not retried every frame */
        g.v[0].present = 1; g.v[0].cur = g.v[0].prev = NO_ANIM;
        attaches = 0; gear_apply(2, bodies[2], &g); assert(!attaches && !gear_owned[2][0]);
        fail_bone = 1; bones_found = 0;
        gear_apply(1, bodies[1], &g); gear_apply(1, bodies[1], &g); assert(bones_found == 1 && !attaches);
        fake_ticks += 2001; gear_apply(1, bodies[1], &g); assert(bones_found == 2);
        fail_bone = 0; fake_ticks += 2001; gear_apply(1, bodies[1], &g); assert(attaches == 1 && gear_owned[1][0]);
        /* the puppet going away takes what this side built with it; the switch for tests turns the whole thing off */
        gear_release(1); assert(!gear_owned[1][0] && !alive[5]);
        no_gear = 1; fake_ticks += 2001; gear_apply(1, bodies[1], &g); assert(!gear_owned[1][0]); no_gear = 0;
        fake_ticks += 2001; gear_apply(1, bodies[1], &g); assert(gear_owned[1][0]);                       /* built again ... */
        no_gear = 1; gear_apply(1, bodies[1], &g); assert(!gear_owned[1][0] && !alive[5]); no_gear = 0;   /* ... and the switch takes it away */
        /* Kurt's canopy exists from his constructor; only whether it is drawn crosses over, and it is forced on the
         * puppet each time (the game's room stasis bit had hidden it while the owner's was open) */
        alive[4] = 1; types[0] = types[1] = 100; *(WORD *)(bodies[4] + GOB_FLAGS) = 0x22;
        gear_capture(local_player, &g); assert(g.v[2].present && !g.v[2].hidden && !g.v[0].present && !g.v[1].present);
        *(WORD *)(bodies[4] + GOB_FLAGS) = 0x2a; gear_apply(1, bodies[1], &g);
        assert((*(WORD *)(bodies[4] + GOB_FLAGS) & 0x08) == 0 && gear_left[1][2] == 0x22);
        g.v[2].hidden = 1; gear_apply(1, bodies[1], &g);
        assert((*(WORD *)(bodies[4] + GOB_FLAGS) & 0x08) && gear_left[1][2] == 0x2a);
        g.v[2].present = 0; *(WORD *)(bodies[4] + GOB_FLAGS) = 0x22; gear_apply(1, bodies[1], &g);
        assert(*(WORD *)(bodies[4] + GOB_FLAGS) == 0x22);                  /* nothing to copy: left alone */
        types[0] = types[1] = 101; alive[4] = 0;
        /* Doc's leaf blower is the same mechanism on another character and bone */
        alive[4] = 0;                                   /* the owner's own jetpack is not needed any more */
        types[1] = 102; memset(&g, 0, sizeof(g)); g.v[1].present = 1; g.v[1].cur = 77; g.v[1].prev = 65; g.v[1].nanims = 1;
        g.v[1].id[0] = 77; g.v[1].anim[0][0] = 1;
        attaches = 0; fake_ticks += 2001; gear_apply(1, bodies[1], &g);
        built = gob_by_handle(gear_owned[1][1]);
        assert(built && !strcmp(names[gob_slot(built)], "docleafblower") && !strcmp(built_model, "leafer") && played_anim == 65);
        gear_capture(bodies[1], &h); assert(h.v[1].present && h.v[1].cur == 77 && h.v[1].prev == 65 && !h.v[0].present);
        /* a model's sub-gobs (the blower's air puffs) go with the item */
        {
            BYTE *puff = om_gob_create_fake("docleafblower__1");
            assert(puff && puff != built && gob_alive(puff));
            *(DWORD *)(puff + GOB_TARGET) = gob_handle(gob_slot(built));
            /* the owner's game shows and hides them itself (Max's thrust code shows the jetpack's flame), the puppet
             * never runs that code: the owner's bits are copied onto the puppet's sub-gobs, read back by a capture */
            *(WORD *)(puff + GOB_FLAGS) = 0x2a; g.v[1].subs = 0; gear_apply(1, bodies[1], &g);
            assert(*(WORD *)(puff + GOB_FLAGS) == 0x22 && gear_subs_left[1][1] == 0);
            gear_capture(bodies[1], &h); assert(h.v[1].subs == 0);
            g.v[1].subs = 1; gear_apply(1, bodies[1], &g);
            assert(*(WORD *)(puff + GOB_FLAGS) == 0x2a && gear_subs_left[1][1] == 1);
            gear_capture(bodies[1], &h); assert(h.v[1].subs == 1);
            no_subs = 1; g.v[1].subs = 0; gear_apply(1, bodies[1], &g); no_subs = 0;   /* the switch for tests */
            assert(*(WORD *)(puff + GOB_FLAGS) == 0x2a);
            gear_release(1);
            assert(!alive[gob_slot(built)] && !alive[gob_slot(puff)]);
        }
    }
    puts("PASS engine adapter: attached canopy, scoped targets, ABI stack balance, life/epoch, direct damage evidence, equipment visuals");
    return 0;
}
