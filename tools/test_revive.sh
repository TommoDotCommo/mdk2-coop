#!/usr/bin/env bash
# No game files, desktop input or lab slots. Exercise revive.c and require broken twins to fail.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
python3 - "$here" <<'PY'
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(sys.argv[1])
out = root / 'local/tests'
out.mkdir(parents=True, exist_ok=True)
source = (root / 'bridge/revive.c').read_text()
mutants = {
    'lava-downs-instead-of-killing': ('#define UNREACHABLE     (64 | 128 | 256 | 2048)', '#define UNREACHABLE     0'),
    'spectate-never-starts': ('#define RIDE_DELAY_MS   2500.0', '#define RIDE_DELAY_MS   25000.0'),
    'revive-out-of-range': ('#define REVIVE_RANGE    5.0f', '#define REVIVE_RANGE    50.0f'),
    'wipe-never-starts': ('#define WIPE_MS         3000.0', '#define WIPE_MS         30000.0'),
    'bleeds-during-revive': ('if (by < 0)\n        down_ms += dt;', 'if (1)\n        down_ms += dt;'),
    'full-health-respawn': ('local_max_hp / 2 > 0 ? local_max_hp / 2 : 1', 'local_max_hp'),
    'client-checkpoint-authority': ('if (!role_host)\n        return;\n    for (s = 0;', 'for (s = 0;'),
    'held-key-revives-again': ('revive_lock = 1;', 'revive_lock = 0;'),
    'pause-causes-bleedout': ('dt = 250.0;', 'dt = dt;'),
    'solo-intercepts-death': (' || !have_remote || players_connected() == 0)', ')'),
}
with tempfile.TemporaryDirectory(prefix='revive-', dir=out) as temp:
    temp = Path(temp)
    for name, mutation in [('baseline', None), *mutants.items()]:
        binary = temp / (name + '.exe')
        command = ['i686-w64-mingw32-gcc', '-O2', '-Wall', '-Wextra', '-Wno-unused-parameter',
                   '-I', str(root / 'bridge'), str(root / 'bridge/tests/revive_test.c'),
                   '-static-libgcc', '-o', str(binary)]
        if mutation:
            old, new = mutation
            if source.count(old) != 1:
                raise SystemExit(f'FAIL mutant {name}: expected exactly one source match')
            variant = temp / (name + '.c')
            variant.write_text(source.replace(old, new))
            command.insert(1, '-DREVIVE_SOURCE="' + str(variant) + '"')
        subprocess.run(command, check=True, timeout=30)
        run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
        if not mutation:
            print(run.stdout, end='')
            if run.returncode:
                raise SystemExit(run.stderr or 'FAIL revive baseline')
        elif run.returncode == 0 or 'FAIL line ' not in run.stderr:
            raise SystemExit(f'FAIL mutant {name}: not rejected by a state assertion: {run.stdout}{run.stderr}')
        else:
            print(f'PASS mutant caught: {name}')
print(f'PASS: {len(mutants)} revive mutation twins rejected')
PY
