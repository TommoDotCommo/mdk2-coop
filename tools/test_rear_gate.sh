#!/usr/bin/env bash
# Game-free production puzzle routing, including authority/history mutants.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
python3 - "$root" <<'PY'
from pathlib import Path
import subprocess, sys, tempfile
root = Path(sys.argv[1])
out = root / 'local/tests/rear-gate'
out.mkdir(parents=True, exist_ok=True)
source = (root / 'bridge/remote.c').read_text()
mutants = {
    'client-commits': ('cur_level == 2 && index == 4 && role_host', 'cur_level == 2 && index == 4'),
    'client-forges-result': ('e->type == 4 && !role_host && from == 0', 'e->type == 4'),
    'missing-history': ('cur_level == 1 || cur_level == 2', 'cur_level == 1'),
}
with tempfile.TemporaryDirectory(dir=out) as folder:
    folder = Path(folder)
    for name, mutation in [('baseline', None), *mutants.items()]:
        binary = folder / (name + '.exe')
        cmd = ['i686-w64-mingw32-gcc', '-O2', '-Wall', '-Wextra', '-Wno-unused-parameter',
               '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
               '-I', str(root / 'bridge'), str(root / 'bridge/tests/rear_gate_test.c'), '-o', str(binary)]
        if mutation:
            old, new = mutation
            assert source.count(old) == 1, name
            variant = folder / (name + '.c')
            variant.write_text(source.replace(old, new))
            cmd.insert(1, '-DREMOTE_SOURCE="' + str(variant) + '"')
        subprocess.run(cmd, check=True, timeout=30)
        run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
        if mutation:
            assert run.returncode and 'FAIL line' in run.stderr, (name, run.stdout, run.stderr)
            print('PASS mutant caught: ' + name)
        else:
            print(run.stdout, end='')
            assert run.returncode == 0, run.stderr
PY
