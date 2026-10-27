/* Production pickup routing against a fake world, clock and event sink. */
#include "../bridge.h"
#include <stdlib.h>
#include <string.h>
#ifndef PICKUPS_SOURCE
#define PICKUPS_SOURCE "../pickups.c"
#endif
#include PICKUPS_SOURCE
struct build_addrs ADDR;
int role_host=1, have_remote=1;
static DWORD table[GOB_SLOTS*2];
static BYTE item[256], info[256];
static double clock_ms=100;
static int sends, relays, rewards, removals, checks;
static struct event last_sent;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); return 1; } checks++; } while(0)
double now_ms(void) { return clock_ms; }
void logmsg(const char *fmt, ...) { }
void *hook_code(BYTE *fn,const BYTE *expect,int len,void *hook) { return NULL; }
int gob_type(BYTE *g) { return *(short *)*(BYTE **)(g+GOB_INFO); }
int gob_doomed(BYTE *g) { return 0; }
void gob_kill(BYTE *g) { removals++; }
int rooms_suppress_enter(BYTE *g) { return 0; }
void script_trace(BYTE *g,int event) { }
void puzzle_resend(int to) { }
void event_init(struct event *e,int kind) { memset(e,0,sizeof(*e)); e->kind=kind; }
void event_send(const struct event *e,int to) { sends++; last_sent=*e; }
void event_relay(const struct event *e,int from) { relays++; }
void remote_queue(const char *name,const char *event,int slot,int arg,int kill) { if(kill==2) rewards++; }
static int no_handler(BYTE *g,int event) { return 0; }
static void reset(void) { pickups_reset(); sends=relays=rewards=removals=0; }
int main(void)
{
    struct event e;
    ADDR.gob_table=(DWORD)table;
    table[0]=(DWORD)item;
    *(BYTE **)(item+GOB_INFO)=info;
    *(short *)info=325;
    strcpy((char *)item+GOB_NAME,"mortars");
    has_event_orig=no_handler;
    event_init(&e,EV_PICKUP); e.type=325; strcpy(e.name,"mortars");
    reset();
    pickups_event(&e,1);
    CHECK(rewards==1 && relays==1 && ntaken==1);
    /* The same pickup from a second collector must not give a third player two rewards. */
    e.pos[0]=0.5f; pickups_event(&e,2);
    CHECK(rewards==1 && relays==1 && ntaken==1);
    reset();
    has_event_hook(item,EVENT_ONPICKEDUP);
    CHECK(sends==1 && last_sent.type==325 && ntaken==1);
    pickups_event(&e,1);
    CHECK(rewards==0 && relays==0);
    /* World history only removes old copies; it never regrants rewards. */
    sends=0; pickups_resend(1);
    CHECK(sends==1 && last_sent.type==0);
    reset(); e.type=0; pickups_event(&e,0);
    CHECK(rewards==0);
    reset(); strcpy((char *)item+GOB_NAME,"coop_reward_1");
    has_event_hook(item,EVENT_ONPICKEDUP);
    CHECK(sends==0);
    /* Unnamed drops are shared immediately, without a name lookup. */
    reset(); item[GOB_NAME]=0; has_event_hook(item,EVENT_ONPICKEDUP);
    CHECK(sends==1 && !last_sent.name[0] && last_sent.type==325);
    reset(); e.name[0]=0; e.type=325; e.pos[0]=0;
    pickups_event(&e,1); pickups_event(&e,2);
    CHECK(rewards==1 && relays==1);
    e.pos[0]=10; pickups_event(&e,1);
    CHECK(rewards==2);
    /* A named item can respawn later and be collected again. */
    reset(); strcpy((char *)item+GOB_NAME,"mortars"); strcpy(e.name,"mortars"); e.pos[0]=0;
    pickups_event(&e,1); clock_ms+=3000; pickups_event(&e,1);
    CHECK(rewards==2 && ntaken==1);
    /* Missing local copies still grant the reward after the script-spawn grace period. */
    reset(); strcpy(e.name,"missing"); pickups_event(&e,1);
    CHECK(rewards==0); clock_ms+=9000;
    for(int i=0;i<6;i++) pickups_frame();
    CHECK(rewards==1);
    reset(); e.type=500; pickups_event(&e,1);
    CHECK(rewards==0 && relays==0);
    printf("PASS: %d pickup routing checks\n",checks);
    return 0;
}
