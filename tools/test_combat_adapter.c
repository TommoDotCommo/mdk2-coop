/* Engine-free execution of the real bridge adapter with a tiny fake gob table. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "combat_core.h"
#define BRIDGE_H
#define MAX_PLAYERS 16
#define EV_NAME 32
#define MAX_PATH 260
#define GOB_POSITION 0
#define TO_ALL -1
#define VK_CONTROL 17
#define VK_PRIOR 33
#define VK_NEXT 34
#define CB_CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); exit(1); } } while(0)
typedef unsigned char BYTE;
struct event { unsigned id; unsigned char kind; signed char part; short damage; int type;
    unsigned netid; short objtype; unsigned short epoch; float pos[3], quat[4];
    uint32_t extra[4]; char name[32], from[32]; };
enum {EV_HIT=1,EV_COMBAT=23};
int role_host=1, cur_epoch=7, test_background, my_slot=1;
static unsigned lua_getparam(int n) {return 1;}
static double lua_getnumber(unsigned n) {return 0;}
char coop_dir[260]=".";
static BYTE target[32], puppet[32], *puppets[16];
static int fake_hp=100, fake_kind=200, presses[256], connected=1;
static double stamp=100;
static struct event sent;
static int sends;
static double now_ms(void) {return stamp;}
static int gob_slot(BYTE *g) {return g==target?0:-1;}
static unsigned gob_handle(int slot) {return slot==0?42:0;}
static unsigned ent_netid(BYTE *g) {return g==target?42:0;}
static int gob_is_projectile(BYTE *g) {return 0;}
static int gob_type(BYTE *g) {return fake_kind;}
static int gob_hp(BYTE *g) {return fake_hp;}
static int gob_alive(BYTE *g) {return g==target || g==puppet;}
static int gob_is_actor(BYTE *g) {return fake_kind>=200 && fake_kind<=259;}
static int player_connected(int slot) {return connected;}
static void event_init(struct event *e,int kind) {memset(e,0,sizeof(*e)); e->kind=kind;e->epoch=cur_epoch;}
static void event_send(const struct event *e,int to) {sent=*e;sends++;}
static int chat_blocks_keys(void) {return 0;}
static int input_down(int key,int vk) {return presses[key];}
static void tolua_function(void *a,const char *n,void (*f)(void)) {}
#ifndef COMBAT_SOURCE
#define COMBAT_SOURCE "../bridge/combat.c"
#endif
#include COMBAT_SOURCE
int main(void)
{
    struct event report, confirmation;
    struct cb_event payload;
    const struct cb_state *s;
    float pos[3]={10,0,0};
    FILE *f;
    puppets[1]=puppet; memcpy(target,pos,sizeof(pos));
    combat_report(puppet,target,"enemy",20,1,-1); report=sent;
    CB_CHECK(report.kind==EV_HIT && report.netid==42 && report.extra[1]==200);
    CB_CHECK(combat_allow_hit(&report,1,target));
    CB_CHECK(!combat_allow_hit(&report,1,target));
    combat_report(puppet,target,"enemy",20000,1,-1); report=sent;
    CB_CHECK(combat_allow_hit(&report,1,target) && report.damage==10000);
    combat_report(puppet,target,"enemy",20,1,-1); report=sent;
    role_host=0; CB_CHECK(!combat_allow_hit(&report,1,target)); role_host=1;
    puppets[1]=NULL; CB_CHECK(!combat_allow_hit(&report,1,target)); puppets[1]=puppet;
    CB_CHECK(!combat_allow_hit(&report,0,target));
    report.netid=43; CB_CHECK(!combat_allow_hit(&report,1,target)); report.netid=42;
    report.epoch=6; CB_CHECK(!combat_allow_hit(&report,1,target)); report.epoch=7;
    CB_CHECK(combat_allow_hit(&report,1,target));
    combat_applied(1,target,42,200,100,80,2049,pos); confirmation=sent;
    CB_CHECK(confirmation.kind==EV_COMBAT); memcpy(&payload,confirmation.name,sizeof(payload));
    CB_CHECK(payload.amount==20 && payload.attacker==1 && payload.target==42);
    s=combat_state(); CB_CHECK(s->total[1]==20 && s->size==1);
    { int before=sends; combat_applied(1,target,42,200,100,100,2049,pos); CB_CHECK(sends==before); }
    cb_reset(&combat); role_host=0;
    combat_event(&confirmation,1); CB_CHECK(combat.size==0);
    combat_event(&confirmation,0); CB_CHECK(combat.size==1 && combat.total[1]==20);
    reset_meter(); CB_CHECK(combat.total[1]==0 && combat.size==1);
    combat_event(&confirmation,0); CB_CHECK(combat.total[1]==0); /* reset retains dedupe */
    cur_epoch=8; combat_event(&confirmation,0); CB_CHECK(combat.size==0);
    /* A same-epoch stale payload inside a current envelope still fails. */
    confirmation.epoch=8; combat_event(&confirmation,0); CB_CHECK(combat.size==0);
    confirmation.epoch=6; payload.epoch=8; payload.id=99;
    memcpy(confirmation.name,&payload,sizeof(payload));
    combat_event(&confirmation,0); CB_CHECK(combat.size==0);
    role_host=1; combat_applied(0,target,42,200,100,0,2049,pos);
    CB_CHECK(combat.lines[0].e.flags==CB_KILL && combat.total[0]==100);
    presses[29]=presses[50]=1; combat_frame(1); CB_CHECK(!meter);
    combat_frame(1); CB_CHECK(!meter); /* edge triggered */
    presses[50]=0; combat_frame(1); presses[50]=1; combat_frame(1); CB_CHECK(meter);
    presses[50]=0; presses[19]=1; combat_frame(1); CB_CHECK(combat.total[0]==0 && combat.size==1);
    presses[19]=0;
    f=tmpfile(); CB_CHECK(f!=NULL); combat_dump(f); CB_CHECK(ftell(f)>0); fclose(f);
    {
        int i, before=sends;
        stamp=2000;
        for(i=0;i<65;i++) combat_report(puppet,target,"enemy",20,1,-1);
        CB_CHECK(sends-before==64);
    }
    puts("PASS real adapter: report validation, HP loss publication, host-only confirmation, epochs, reset, keys and dumps");
    return 0;
}
