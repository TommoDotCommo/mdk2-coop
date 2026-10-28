#!/usr/bin/env bash
# Fault-context decoding without game files; both omissions and model/index
# confusion must fail. Needs MinGW and Windows executable interop.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
out="$here/local/tests/crash-context"
mkdir -p "$out"
python3 - "$here" "$out" <<'PY'
import pathlib, subprocess, sys
root, out = map(pathlib.Path, sys.argv[1:])
source = (root/'bridge/crash_context.h').read_text()
variants = {
    'baseline': source,
    'missing-registers': source.replace('if (!c) return;', 'if (c) return;\n    if (!c) return;'),
    'model-as-index': source.replace('c->Esp + 0x2c, &index', 'c->Esp + 0x28, &index'),
}
for name, text in variants.items():
    header = out/(name+'.h')
    header.write_text(text)
    exe = out/(name+'.exe')
    subprocess.run(['i686-w64-mingw32-gcc', '-O2', '-Wall', '-Wextra',
                    '-DCRASH_CONTEXT_HEADER="'+str(header)+'"',
                    str(root/'bridge/tests/crash_context_test.c'), '-o', str(exe)], check=True)
    result = subprocess.run([str(exe)], capture_output=True, text=True)
    if (result.returncode == 0) != (name == 'baseline'):
        sys.exit(name+' unexpected result: '+result.stdout+result.stderr)
    print(result.stdout.strip() if name == 'baseline' else 'PASS: mutant '+name+' rejected')
PY
