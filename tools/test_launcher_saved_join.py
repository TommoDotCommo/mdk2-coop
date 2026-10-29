#!/usr/bin/env python3
"""Run production saved-address actions with a fake persistence/lobby boundary."""
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'bridge/launcher_profiles_win.h').read_text()
start = source.index('static int profile_save_endpoint(')
source = source[start:source.index('static void page_profiles(void)', start)]
prelude = r'''
#include <assert.h>
#include <arpa/inet.h>
#include <strings.h>
#include <time.h>
#include "launcher_profiles.h"
#define _stricmp strcasecmp
#define MODE_HOST 1
#define MODE_JOIN 2
#define PAGE_PLAY 0
static struct lp_store profiles;
static struct lp_probe profile_probes[LP_MAX];
static int profile_selected,profile_bad,profile_edit,profile_lock,page,changed,active,opened,writes,fail_save;
static char profile_message[180];
static struct {int steam,mode;char port[8],listen[8],host[128];} cfg;
static void profile_load(void) {}
static int profile_save(struct lp_store *next) {
 char b[LP_BYTES];
 if(fail_save || !lp_encode(next,b,sizeof(b))) return 0;
 profiles=*next;writes++;return 1;
}
static void EnterCriticalSection(int *p) {}
static void LeaveCriticalSection(int *p) {}
static int profile_session_active(void) {return active;}
static void lob_open(void) {active=1;opened++;}
'''
tests = r'''
int main(void) {
 assert(profile_save_endpoint("127.0.0.1",27900));
 assert(profiles.n==1 && profiles.p[0].port==27900 && !strcmp(profiles.p[0].host,"127.0.0.1"));
 strcpy(profiles.p[0].name,"Friends");strcpy(profiles.p[0].notes,"Keep my notes");
 assert(profile_save_endpoint("127.0.0.1",27900));
 assert(profiles.n==1 && writes==1 && !strcmp(profiles.p[0].notes,"Keep my notes"));
 assert(profile_save_endpoint("127.0.0.1",27901) && profiles.n==2);
 assert(!profile_save_endpoint("127.0.0.1",0) && profiles.n==2);
 profile_bad=1;assert(!profile_save_endpoint("127.0.0.2",27900) && profiles.n==2);profile_bad=0;
 fail_save=1;assert(!profile_save_endpoint("127.0.0.2",27900) && profiles.n==2);fail_save=0;
 profile_selected=0;page=5;profile_edit=1;profile_join(0);
 assert(active && opened==1 && page==PAGE_PLAY && !profile_edit);
 assert(cfg.mode==MODE_JOIN && !strcmp(cfg.host,"127.0.0.1") && !strcmp(cfg.port,"27900"));
 profile_join(0);assert(opened==1); /* cannot replace an active session */
 active=0;profile_selected=1;page=5;profile_join(1);
 assert(active && opened==2 && page==PAGE_PLAY && cfg.mode==MODE_HOST && !strcmp(cfg.listen,"27901"));
 puts("PASS: save actual host and port; preserve existing metadata; join/host open lobby on Play page");
}
'''
with tempfile.TemporaryDirectory() as temp:
    d=Path(temp)
    def run(s):
        (d/'test.c').write_text(prelude+s+tests)
        subprocess.run(['cc','-std=c99','-I',str(ROOT/'bridge'),str(d/'test.c'),'-o',str(d/'test')],check=True)
        return subprocess.run([str(d/'test')],capture_output=True,cwd=temp)
    result=run(source);assert result.returncode==0,result.stderr.decode();print(result.stdout.decode().strip())
    for name,before,after in [('no-lobby-open','    lob_open();',''),
                              ('stay-in-address-list','page = PAGE_PLAY;','page = 5;'),
                              ('duplicate-address','!_stricmp(next.p[i].host, host) && next.p[i].port == port','0')]:
        assert before in source
        result=run(source.replace(before,after,1))
        assert result.returncode!=0,'SURVIVED: '+name
        print('KILLED:',name)
