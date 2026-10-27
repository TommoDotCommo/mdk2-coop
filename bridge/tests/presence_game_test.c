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
#define GOB_ANIM 0x60
#define GOB_POSITION 0x18
#define LIFE_ALIVE 0
static BYTE bodies[6][256], infos[6][256], states[6][256], controllers[6][256];
static DWORD animation[6][2][10];
static int types[6] = {100,100,100,202,0,202};
static int alive[6] = {1,1,1,1,1,1}, in_level[3] = {1,1,1}, life[3];
static int role_host = 1, have_remote = 1, my_slot, test_background = 1;
static BYTE *local_player = bodies[0], *puppets[3] = {NULL,bodies[1],bodies[2]};
static struct { DWORD player_handle; } ADDR = {0x5494c8};
static int gob_slot(BYTE *g) { int i; for(i=0;i<6;i++) if(g==bodies[i]) return i; return -1; }
static DWORD gob_handle(int i) { return 0x10000u + i; }
static BYTE *gob_by_handle(DWORD h) { int i; for(i=0;i<6;i++) if(h==gob_handle(i) && alive[i]) return bodies[i]; return NULL; }
static int gob_alive(BYTE *g) { int i=gob_slot(g); return i>=0 && alive[i]; }
static int gob_type(BYTE *g) { return types[gob_slot(g)]; }
static int gob_is_actor(BYTE *g) { return g && gob_type(g)>=200 && gob_type(g)<260; }
static int puppet_slot(BYTE *g) { int i; for(i=0;i<3;i++) if(g && puppets[i]==g) return i; return -1; }
static int player_in_level(int s) { return in_level[s]; }
static int revive_life(void) { return life[0]; }
static int player_life(int s) { return life[s]; }
static const char *gob_name(BYTE *g) { return "enemy"; }
static void gob_peername(BYTE *g, char *out, int size) { snprintf(out,size,"@p%d",gob_slot(g)+1); }
static DWORD *om_anim_state(BYTE *g, int id) { return id==66 || id==67 ? animation[gob_slot(g)][id-66] : NULL; }
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
    return 42;
}
int main(void)
{
    struct canopy_state a = {0}, b;
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
    *(float *)(bodies[0]+GOB_POSITION)=50;
    *(float *)(bodies[1]+GOB_POSITION)=2;
    *(float *)(bodies[2]+GOB_POSITION)=5;
    expected=bodies[1];
    assert(ai_run(bodies[3],(DWORD)(uintptr_t)fake_think,11,22,33)==42);
    assert(ai_target()==local_player); /* callback context was restored */
    assert(ai_evidence[3].queries==1 && ai_evidence[3].victim==gob_handle(1));
    assert(presence_hit(bodies[3],bodies[1],0)==0 && ai_evidence[3].hits==1);
    no_direct_hurt=1; assert(presence_hit(bodies[3],bodies[1],0)==1); no_direct_hurt=0;
    life[1]=1; expected=bodies[2]; ai_run(bodies[3],(DWORD)(uintptr_t)fake_think,11,22,33); life[1]=0;
    in_level[1]=0; expected=bodies[2]; ai_run(bodies[3],(DWORD)(uintptr_t)fake_think,11,22,33); in_level[1]=1;
    fallback=bodies[5]; expected=fallback; ai_run(bodies[3],(DWORD)(uintptr_t)fake_think,11,22,33); /* scripted target */
    fallback=local_player; no_targeting=1; expected=local_player; ai_run(bodies[3],(DWORD)(uintptr_t)fake_think,11,22,33);
    no_targeting=0; role_host=0; expected=local_player; ai_run(bodies[3],(DWORD)(uintptr_t)fake_think,11,22,33);
    presence_hit(bodies[3],local_player,1); assert(local_direct_hits==1);
    melee_only=1;
    assert(presence_damage_allowed((void *)0x434e56));
    assert(!presence_damage_allowed((void *)0x1234));
    test_background=0; assert(presence_damage_allowed((void *)0x1234));
    puts("PASS engine adapter: attached canopy, scoped targets, life/epoch, direct damage evidence");
    return 0;
}
