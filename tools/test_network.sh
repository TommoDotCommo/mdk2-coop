#!/usr/bin/env bash
# Offline Win32 packet tests and epoch mutation twins. No game, Steam account or game copy.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
python3 - "$here" <<'PY'
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(sys.argv[1])
out = root / 'local/tests/network'
out.mkdir(parents=True, exist_ok=True)
source = (root / 'bridge/net.c').read_text()
mutants = {
    'missing-enemy-maximum': ('e.extra[0] = gob_max_hp(gob_by_handle(netid));', 'e.extra[0] = 0;'),
    'adopt-epoch-before-load': ('pending_epoch = (unsigned short)e->netid;',
                              'pending_epoch = (unsigned short)e->netid; cur_epoch = pending_epoch;'),
    'apply-spawns-during-load': ('return cur_epoch != pending_epoch && e->epoch == pending_epoch;', 'return 0;'),
    'block-same-epoch-rejoin': ('return cur_epoch != pending_epoch && e->epoch == pending_epoch;',
                              'return e->epoch == pending_epoch;'),
    'ignore-epoch-zero': ('pending_epoch = (unsigned short)e->netid;',
                         'if (e->netid) pending_epoch = (unsigned short)e->netid;'),
}
with tempfile.TemporaryDirectory(prefix='epochs-', dir=out) as folder:
    folder = Path(folder)
    winfolder = subprocess.check_output(['wslpath', '-w', str(folder)], text=True).strip().replace("'", "''")
    for name, mutation in [('baseline', None), *mutants.items()]:
        binary = folder / (name + '.exe')
        command = ['i686-w64-mingw32-gcc', '-O2', '-Wall', '-Wextra', '-Wno-unused-parameter',
                   '-I', str(root / 'bridge'), str(root / 'tools/test_network.c'),
                   '-lws2_32', '-static-libgcc', '-o', str(binary)]
        if mutation:
            old, new = mutation
            if source.count(old) != 1:
                raise SystemExit(f'FAIL mutant {name}: expected exactly one source match')
            variant = folder / (name + '.c')
            variant.write_text(source.replace(old, new))
            command.insert(1, '-DNET_SOURCE="' + str(variant) + '"')
        subprocess.run(command, check=True, timeout=30)
        script = (f"Push-Location '{winfolder}'; & '.\\{name}.exe'; "
                  "$code = $LASTEXITCODE; Pop-Location; exit $code")
        run = subprocess.run(['powershell.exe', '-NoProfile', '-Command', script],
                             capture_output=True, text=True, timeout=15)
        if not mutation:
            print(run.stdout, end='')
            if run.returncode:
                raise SystemExit(run.stderr or 'FAIL network baseline')
        elif run.returncode == 0 or 'FAIL line ' not in run.stdout + run.stderr:
            raise SystemExit(f'FAIL mutant {name}: not rejected by an assertion: {run.stdout}{run.stderr}')
        else:
            print(f'PASS mutant caught: {name}')
print(f'PASS: {len(mutants)} network mutation twins rejected')
PY
