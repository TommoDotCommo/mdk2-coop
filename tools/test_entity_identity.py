#!/usr/bin/env python3
"""Production event identities must survive duplicate-name spawn/death transitions."""
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
s = (ROOT/'bridge/game.c').read_text()
slot_start=s.index('int gob_slot(')
slot_source=s[slot_start:s.index('unsigned int gob_handle(',slot_start)]
start=s.index('void gob_peername(')
s=s[start:s.index('/* The reverse',start)]
e = (ROOT/'bridge/entities.c').read_text()
start=e.index('static int replicated_object(')
replicated=e[start:e.index('static struct ent ents',start)]
prelude=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#define GOB_SLOTS 4
static uintptr_t test_table[8];
#define GOB_TABLE test_table
typedef unsigned char BYTE;
static BYTE bodies[4];
static int types[4]={100,207,511,540};
static int gob_type(BYTE *g) {return types[g-bodies];}
static int gob_is_actor(BYTE *g) {return gob_type(g)>=200 && gob_type(g)<=259;}
static BYTE *local_player=&bodies[0];
static int my_slot=0, repeated;
static unsigned int bound=0x40022;
static int puppet_slot(BYTE *g) {return g==&bodies[1]?1:-1;}
static unsigned int ent_netid(BYTE *g) {return g==&bodies[2]?bound:0;}
static void gob_netname(BYTE *g,char *out,int size) {
 snprintf(out,size,"%s",g==&bodies[2]?(repeated?"n40022":"generator_spawn"):"generator");
}
'''
test=r'''
int main(void){char name[32];
 assert(gob_slot(NULL)==-1);
 assert(!replicated_object(NULL));
 assert(!replicated_object(&bodies[0]) && replicated_object(&bodies[1]));
 assert(replicated_object(&bodies[2]) && !replicated_object(&bodies[3]));
 test_table[2]=(uintptr_t)&bodies[0];assert(gob_slot(&bodies[0])==1);
 for(int duplicates=0;duplicates<2;duplicates++) {
  repeated=duplicates;
  gob_peername(&bodies[2],name,sizeof(name));assert(!strcmp(name,"n40022"));
 }
 /* Removing the final sibling cannot change the survivor's event destination. */
 repeated=0;gob_peername(&bodies[2],name,sizeof(name));assert(!strcmp(name,"n40022"));
 gob_peername(local_player,name,sizeof(name));assert(!strcmp(name,"@p1"));
 gob_peername(&bodies[1],name,sizeof(name));assert(!strcmp(name,"@p2"));
 gob_peername(&bodies[3],name,sizeof(name));assert(!strcmp(name,"generator"));
 gob_peername(NULL,name,sizeof(name));assert(!name[0]);
 bound=0;gob_peername(&bodies[2],name,sizeof(name));assert(!strcmp(name,"generator_spawn"));
 puts("PASS stable entity IDs across duplicate spawn/death; player and scenery identities preserved");
}
'''
with tempfile.TemporaryDirectory() as temp:
 p=Path(temp)
 def run(code, slots=slot_source, objects=replicated):
  (p/'test.c').write_text(prelude+slots+objects+code+test)
  subprocess.run(['cc','-std=c99',str(p/'test.c'),'-o',str(p/'test')],check=True)
  return subprocess.run([str(p/'test')],capture_output=True,cwd=temp)
 result=run(s);assert result.returncode==0,result.stderr.decode();print(result.stdout.decode().strip())
 mutant=run(s.replace('if (id) snprintf','if (0) snprintf'))
 assert mutant.returncode!=0,'SURVIVED: unstable-event-name'
 print('KILLED: unstable-event-name')

 for name, slots, objects in [
  ('null-is-empty-slot',slot_source.replace('if (!gob) return -1;', ''),replicated),
  ('generator-not-replicated',slot_source,replicated.replace(' || gob_type(gob) == 511',''))]:
  result=run(s, slots, objects)
  assert result.returncode!=0,'SURVIVED: '+name
  print('KILLED:',name)
