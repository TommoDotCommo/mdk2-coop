/* Exercise production class-slot installation and dispatch with a fake engine. */
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef unsigned char BYTE;
typedef uint32_t DWORD;
#define __cdecl
#define CLASS_SLOTS 14
#define CLASS_THINK 1
#define CLASS_ANIMLOOP 11
/* Slot indices are copied from the production header by the test runner. */
#include "ai_slot_constants.h"
static DWORD descriptors[64][CLASS_SLOTS];
static BYTE *classes[64];
#define CLASS_TABLE classes
static BYTE gob[16], thunks[4][32];
static int allocation, mirrored, role_host=1, scoped;
static int gob_class(BYTE *g) { assert(g==gob); return 4; }
static int gob_is_mirrored(BYTE *g) { return mirrored; }
static int gob_is_actor(BYTE *g) { return g==gob; }
static void ai_hook_return(BYTE *g,DWORD *ret) { assert(g==gob && *ret==123); scoped++; }
#define MEM_COMMIT 1
#define MEM_RESERVE 2
#define PAGE_EXECUTE_READWRITE 4
#define PAGE_READWRITE 8
static void *VirtualAlloc(void *p,int bytes,int flags,int prot) { assert(allocation<4); return thunks[allocation++]; }
static void VirtualProtect(void *p,int bytes,int prot,DWORD *old) { *old=0; }
#define logmsg(...) ((void)0)
#include "ai_slots_source.h"
int main(void) {
    DWORD frame[2]={123,(DWORD)(uintptr_t)gob};
    int f, attack=-1;
    classes[4]=(BYTE *)descriptors[4];
    descriptors[4][3]=0x434d00; /* native Grunt custom animation key/strike */
    descriptors[4][7]=0x434ed0; /* collision callback: preserve mirror filtering */
    class_filter_think(4);
    assert(descriptors[4][3]!=0x434d00);
    assert(descriptors[4][7]!=0x434ed0);
    for(f=0;f<FILTER_SLOTS;f++) if(filter_slot[f]==3) attack=f;
    assert(attack>=0 && slot_filter(attack,frame)==0x434d00 && scoped==1);
    mirrored=1;
    assert(slot_filter(attack,frame)==0 && scoped==1);
    puts("PASS native slots: actual strike callback scoped; mirrored attacks skipped; collision filter retained");
}
