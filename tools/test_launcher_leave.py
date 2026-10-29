#!/usr/bin/env python3
"""Exercise the production render dispatch: leaving must not click Join underneath."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'bridge/launcher.c').read_text()
start = source.index('    if (page == PAGE_PLAY) {\n        if (lob.on)')
end = source.index('\nstatic void pass(void)', start)
# Keep the production renderer's page dispatch and footer, stopping at its closing brace.
body = source[start:end].rstrip()
PRELUDE = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#define PAGE_PLAY 0
#define PAGE_PROFILES 1
#define PAGE_SAVES 2
#define PAGE_SETTINGS 3
#define PAGE_HELP 4
#define MODE_SOLO 0
#define MODE_JOIN 1
#define LS_CHAT_HISTORY 64
#define ID 1
#define PX 760
#define PW 480
#define LH 720
#define C_BLUE 1
#define C_ORANGE 2
#define DT_LEFT 0
typedef unsigned DWORD;
static int page, guide_open, f_small, sel=-1, done, opens, leaves, click;
static struct {int mode;} cfg;
static struct {int on, host, n, connected, start_at, ready;
    struct {int nmembers; unsigned history_total; int members[16];} comm;} lob;
static DWORD GetTickCount(void) {return 0;}
static int ls_local_talking(void *c,DWORD now) {return 0;}
static int ls_member_talking(void *c,DWORD now) {return 0;}
static void gdi_text(int f,int x,int y,int color,const char *s,int flags) {}
static int button(int id,int x,int y,int w,int h,const char *s,int primary) {
    return click && x <= 1000 && 1000 < x+w && y <= 660 && 660 < y+h;
}
static void lob_leave(void) {lob.on=0;leaves++;}
static void lobby_page(void) {if(click) lob_leave();}
static void page_play(void) {}
static void page_profiles(void) {}
static void page_saves(void) {}
static void page_settings(void) {}
static void page_help(void) {}
static void page_about(void) {}
static void draw_full_guide(void) {}
static void lob_primary(void) {}
static void start_summary(char *s,int n) {s[0]=0;}
static int lob_wants(void) {return 1;}
static void lob_open(void) {opens++;lob.on=1;}
static void render(void) {char line[160];
'''
TEST = r'''
int main(void) {
    for(int host=0;host<2;host++) {
        page=PAGE_PLAY; lob.on=1; lob.host=host; cfg.mode=host?2:MODE_JOIN;
        click=1; opens=leaves=done=0;
        render();
        assert(leaves==1 && "Leave must close the lobby");
        assert(!lob.on && opens==0 && !done && "Leave click must not activate Host/Join underneath");
        click=0; render(); assert(!lob.on && opens==0);
        click=1; render(); assert(lob.on && opens==1 && "A later explicit click may rejoin");
    }
    puts("PASS: host/client Leave consumes the click; idle stays out; explicit rejoin works");
}
'''
with tempfile.TemporaryDirectory() as temp:
    p=Path(temp); c=p/'leave.c'; exe=p/'leave'
    def run(text):
        c.write_text(PRELUDE+text+TEST)
        subprocess.run(['cc','-std=c99','-O0',str(c),'-o',str(exe)],check=True)
        return subprocess.run([str(exe)],capture_output=True, cwd=temp)
    result=run(body)
    assert result.returncode==0,result.stderr.decode()
    print(result.stdout.decode().strip())
    guard='if (!lob.on) return; /* Leave must not activate the Play footer in this pass. */'
    assert guard in body
    mutant=run(body.replace(guard,''))
    assert mutant.returncode!=0,'SURVIVED: leave-click-through'
    print('KILLED: leave-click-through')
