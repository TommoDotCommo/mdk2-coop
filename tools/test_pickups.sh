#!/usr/bin/env bash
# Game-free production pickup routing checks, including broken behavior twins.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
python3 - "$root" <<'PY'
import pathlib, subprocess, sys, tempfile
root=pathlib.Path(sys.argv[1]); out=root/'local/tests/pickups'; out.mkdir(parents=True,exist_ok=True)
source=(root/'bridge/pickups.c').read_text()
mutants={
 'duplicate-collectors': ('if (e->type && pickup_seen(e))','if (0 && e->type && pickup_seen(e))'),
 'reward-echo': ('if (strncmp(name, "coop_reward_", 12)) {','if (1) {'),
 'lost-unnamed-reward': ('if (type >= ITEM_TYPE_MIN && type <= ITEM_TYPE_MAX)','if (0 && type >= ITEM_TYPE_MIN && type <= ITEM_TYPE_MAX)'),
 'regrant-on-resend': ('snprintf(e.name, EV_NAME, "%s", taken[i]);','e.type = 325; snprintf(e.name, EV_NAME, "%s", taken[i]);'),
}
with tempfile.TemporaryDirectory(dir=out) as folder:
 folder=pathlib.Path(folder)
 for name,mutation in [('baseline',None),*mutants.items()]:
  cmd=['i686-w64-mingw32-gcc','-O2','-Wall','-Wextra','-Wno-unused-parameter','-I',str(root/'bridge'),str(root/'bridge/tests/pickups_test.c'),'-o',str(folder/(name+'.exe'))]
  if mutation:
   old,new=mutation
   if source.count(old)!=1: raise SystemExit('FAIL source match: '+name)
   variant=folder/(name+'.c'); variant.write_text(source.replace(old,new)); cmd.insert(1,'-DPICKUPS_SOURCE="'+str(variant)+'"')
  subprocess.run(cmd,check=True,timeout=30)
  run=subprocess.run([str(folder/(name+'.exe'))],capture_output=True,text=True,timeout=10)
  if not mutation:
   print(run.stdout,end='')
   if run.returncode: raise SystemExit(run.stderr)
  elif run.returncode==0 or 'FAIL line' not in run.stderr: raise SystemExit('FAIL mutant not detected: '+name+' '+run.stderr)
  else: print('PASS mutant caught: '+name)
print('PASS: 4 pickup mutation twins rejected')
PY
