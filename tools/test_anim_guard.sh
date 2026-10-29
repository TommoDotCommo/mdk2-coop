#!/usr/bin/env bash
# The guarded animation lookup (bridge/anim_guard.h) against fake gobs; a guard that skips the model's
# animation-table check must fail. Needs MinGW and Windows executable interop.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
out="$here/local/tests/anim-guard"
mkdir -p "$out"
python3 - "$here" "$out" <<'PY'
import pathlib, subprocess, sys
root, out = map(pathlib.Path, sys.argv[1:])
source = (root/'bridge/anim_guard.h').read_text()
variants = {
    'baseline': source,
    'no-table-check': source.replace('!*(BYTE **)(res + ANIM_MODEL_TABLE)', '0'),
    'no-model-check': source.replace('if (model) {', 'if (0) {'),
    'no-id-range': source.replace('id < 0 || id >= ANIM_ID_MAX', '0'),
}
for name, text in variants.items():
    header = out/(name+'.h')
    header.write_text(text)
    exe = out/(name+'.exe')
    subprocess.run(['i686-w64-mingw32-gcc', '-O2', '-Wall', '-Wextra', '-DANIM_GUARD_HEADER="'+str(header)+'"',
                    str(root/'bridge/tests/anim_guard_test.c'), '-o', str(exe)], check=True)
    result = subprocess.run([str(exe)], capture_output=True, text=True)
    if (result.returncode == 0) != (name == 'baseline'):
        sys.exit(name+' unexpected result: '+result.stdout+result.stderr)
    print(result.stdout.strip() if name == 'baseline' else 'PASS: mutant '+name+' rejected')
PY
