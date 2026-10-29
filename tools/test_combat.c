/* Executes the shipped portable core, including a loss/reorder/retry simulator. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "combat_core.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); exit(1); } } while (0)
static struct cb_event hit(unsigned id, int slot)
{
    struct cb_event e = {0};
    e.id=id; e.attacker=slot; e.target=42; e.epoch=7;
    e.amount=20; e.kind=200; e.weapon=2049; e.pos[0]=10;
    return e;
}
static struct cb_guard guard;
static struct cb_state host, peers[3];
static void protections(void)
{
    struct cb_event e, bad;
    float p[3]={10,0,0};
    int i;
    CHECK(sizeof(e)==32);
#define REJECT(change) do { memset(&guard,0,sizeof(guard)); bad=hit(1,1); change; CHECK(!cb_validate(&guard,&bad,1,CB_REPORT,7,1000)); } while(0)
    e=hit(1,1); memset(&guard,0,sizeof(guard));
    CHECK(!cb_validate(&guard,&e,2,CB_REPORT,7,1000));
    CHECK(!cb_validate(&guard,&e,1,CB_CONFIRM,7,1000));
    e=hit(1,15); CHECK(cb_validate(&guard,&e,15,CB_REPORT,7,1000));
    REJECT(bad.attacker=16); REJECT(bad.epoch=6); REJECT(bad.id=0); REJECT(bad.target=0);
    REJECT(bad.kind=199); REJECT(bad.kind=260); REJECT(bad.amount=0); REJECT(bad.amount=32768);
    REJECT(bad.weapon=4096); REJECT(bad.flags=4); REJECT(bad.reserved=1);
    REJECT(bad.pos[0]=NAN); REJECT(bad.pos[1]=INFINITY); REJECT(bad.pos[2]=-1000001);
    memset(&guard,0,sizeof(guard)); e=hit(1,1); e.amount=20000;
    CHECK(cb_validate(&guard,&e,1,CB_REPORT,7,1000)); CHECK(e.amount==10000);
    memset(&guard,0,sizeof(guard)); e=hit(1,1); e.amount=20000;
    CHECK(cb_validate(&guard,&e,0,CB_CONFIRM,7,1000)); CHECK(e.amount==20000);
    memset(&guard,0,sizeof(guard)); e=hit(80,1);
    CHECK(cb_validate(&guard,&e,1,CB_REPORT,7,1000));
    CHECK(!cb_validate(&guard,&e,1,CB_REPORT,7,1000));
    e.id=79; CHECK(cb_validate(&guard,&e,1,CB_REPORT,7,1000));
    CHECK(!cb_validate(&guard,&e,1,CB_REPORT,7,1000));
    e.id=16; CHECK(!cb_validate(&guard,&e,1,CB_REPORT,7,1000));
    memset(&guard,0,sizeof(guard));
    for(i=1;i<=64;i++) { e=hit(i,1); CHECK(cb_validate(&guard,&e,1,CB_REPORT,7,1000)); }
    e.id=65; CHECK(!cb_validate(&guard,&e,1,CB_REPORT,7,1999));
    CHECK(cb_validate(&guard,&e,1,CB_REPORT,7,2000));
    memset(&guard,0,sizeof(guard));
    for(i=1;i<=64;i++) { e=hit(i,1); CHECK(cb_validate(&guard,&e,1,CB_PUBLISH,7,1000)); }
    e.id=65; CHECK(!cb_validate(&guard,&e,1,CB_PUBLISH,7,1000));
    memset(&guard,0,sizeof(guard));
    for(i=1;i<=128;i++) { e=hit(i,1); CHECK(cb_validate(&guard,&e,0,CB_CONFIRM,7,1000)); }
    memset(&guard,0,sizeof(guard)); e=hit(UINT32_MAX,1);
    CHECK(cb_validate(&guard,&e,1,CB_REPORT,7,1000));
    e.id=1; CHECK(cb_validate(&guard,&e,1,CB_REPORT,7,1000));
    e=hit(1,1); memset(&guard,0,sizeof(guard));
    CHECK(!cb_validate_hit(&guard,&e,1,7,43,200,100,p,1000));
    CHECK(!cb_validate_hit(&guard,&e,1,7,42,201,100,p,1000));
    CHECK(!cb_validate_hit(&guard,&e,1,7,42,200,0,p,1000));
    p[0]=75; CHECK(!cb_validate_hit(&guard,&e,1,7,42,200,100,p,1000));
    p[0]=NAN; CHECK(!cb_validate_hit(&guard,&e,1,7,42,200,100,p,1000));
    p[0]=10; CHECK(cb_validate_hit(&guard,&e,1,7,42,200,100,p,1000));
    CHECK(cb_attack_valid(1,-1)); CHECK(cb_attack_valid(65535,64));
    CHECK(!cb_attack_valid(0,0)); CHECK(!cb_attack_valid(65536,0));
    CHECK(!cb_attack_valid(1,-2)); CHECK(!cb_attack_valid(1,65));
    puts("PASS validation, clamp, replay, wrap, rate and live-target gates");
}
static void meters(void)
{
    struct cb_event e;
    int i;
    cb_reset(&host);
    e=hit(1,0); CHECK(cb_accept(&host,&e,0,CB_PUBLISH,7,0));
    e=hit(2,1); CHECK(cb_accept(&host,&e,1,CB_PUBLISH,7,1000));
    CHECK(host.total[0]==20 && host.total[1]==20);
    CHECK(cb_rolling(&host,0,4999)==4); CHECK(cb_rolling(&host,0,5000)==0);
    CHECK(cb_fight_dps(&host,0,2000)==10);
    CHECK(fabs(cb_fight_dps(&host,0,10000)-20.0)<0.001); /* frozen at last hit */
    e=hit(3,1); CHECK(cb_accept(&host,&e,1,CB_PUBLISH,7,9000));
    CHECK(host.fight[0]==0 && host.fight[1]==20 && host.total[1]==40);
    CHECK(!cb_accept(&host,&e,1,CB_PUBLISH,6,9000));
    for(i=4;i<804;i++) { e=hit(i,0); CHECK(cb_accept(&host,&e,0,CB_PUBLISH,7,(uint64_t)i*1000)); }
    CHECK(host.size==CB_KEEP && host.next<CB_KEEP);
    CHECK(host.lines[(host.next+CB_KEEP-1)%CB_KEEP].e.id==803);
    puts("PASS rolling/fight DPS, idle boundary, totals and bounded history");
}
static unsigned rng=1234567;
static unsigned random_u(void) { rng^=rng<<13; rng^=rng>>17; rng^=rng<<5; return rng; }
static void simulation(void)
{
    struct cb_event events[32];
    unsigned delivered[3], n, p, batch, attempt;
    uint64_t expected[16]={0};
    cb_reset(&host); for(p=0;p<3;p++) cb_reset(&peers[p]);
    for(batch=0;batch<500;batch++) {
        uint64_t now=(uint64_t)batch*1000;
        memset(delivered,0,sizeof(delivered));
        for(n=0;n<32;n++) {
            events[n]=hit(batch*32+n+1,n%4);
            events[n].amount=1+random_u()%100;
            expected[n%4]+=events[n].amount;
            CHECK(cb_accept(&host,&events[n],n%4,CB_PUBLISH,7,now));
        }
        /* Each tick loses/reorders and duplicates packets, then retries every lost one. */
        for(attempt=0;attempt<256;attempt++) {
            n=random_u()%32; p=random_u()%3;
            if(random_u()%4==0) continue;
            { int accepted=cb_accept(&peers[p],&events[n],0,CB_CONFIRM,7,now+100);
              CHECK(accepted==!(delivered[p]&(1u<<n))); delivered[p]|=1u<<n; }
        }
        for(p=0;p<3;p++) for(n=0;n<32;n++) {
            int accepted=cb_accept(&peers[p],&events[n],0,CB_CONFIRM,7,now+900);
            CHECK(accepted==!(delivered[p]&(1u<<n)));
        }
        for(p=0;p<3;p++) CHECK(!memcmp(host.total,peers[p].total,sizeof(expected)));
    }
    CHECK(!memcmp(host.total,expected,sizeof(expected)));
    puts("PASS 500 lossy simulation rounds, reordered retries and duplicate deliveries to three peers");
}
static void fuzz(void)
{
    struct cb_event e, copy;
    unsigned i,j;
    cb_reset(&host);
    for(i=0;i<200000;i++) {
        unsigned char *raw=(unsigned char *)&e;
        for(j=0;j<sizeof(e);j++) raw[j]=random_u();
        if(i%2) { e=hit(i+1,random_u()%16); e.amount=random_u()%65536; e.pos[0]=(float)(int)random_u(); }
        copy=e;
        if(cb_accept(&host,&copy,0,CB_CONFIRM,7,i)) {
            CHECK(e.attacker<16 && e.epoch==7 && e.id && e.target);
            CHECK(e.amount>0 && e.amount<=32767 && e.kind>=200 && e.kind<=259);
            CHECK(e.weapon<=4095 && !(e.flags&~3) && !e.reserved);
            for(j=0;j<3;j++) CHECK(isfinite(e.pos[j]) && fabsf(e.pos[j])<=1000000);
        }
        CHECK(host.size<=CB_KEEP && host.next<CB_KEEP);
    }
    puts("PASS 200000 deterministic malformed/valid payload fuzz cases");
}
int main(void) { protections(); meters(); simulation(); fuzz(); return 0; }
