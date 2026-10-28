#!/usr/bin/env bash
# No game or microphone use: production WinMM enumeration and synthetic capture.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
python3 - "$here" <<'PY'
from pathlib import Path
import subprocess
import sys
import tempfile
root = Path(sys.argv[1])
out = root/'local/tests'
out.mkdir(parents=True, exist_ok=True)
source = (root/'bridge/launcher_voice_win.h').read_text()
mutants = {
    'pacing': ('lv_next_tone_ms = now + 40;', 'lv_next_tone_ms = now;'),
    'expiry': ('(lv_tone_until && (int32_t)(lv_tone_until - now) > 0) || ptt_key_down', '1'),
}
with tempfile.TemporaryDirectory(dir=out) as temp:
    directory = Path(temp)
    for name, change in [('baseline', None), *mutants.items()]:
        header = source
        if change:
            before, after = change
            assert header.count(before) == 1
            header = header.replace(before, after)
        (directory/'launcher_voice_win.h').write_text(header)
        exe = directory/'launcher_audio_test.exe'
        subprocess.run(['i686-w64-mingw32-gcc','-std=c99','-O2','-Wall','-Wextra',
                        '-Wno-unused-function','-I',str(directory),'-I',str(root/'bridge'),
                        str(root/'bridge/tests/launcher_audio_test.c'),'-o',str(exe),'-lwinmm'], check=True)
        result = subprocess.run([str(exe)], capture_output=True, text=True)
        if change:
            assert result.returncode != 0, 'SURVIVED: '+name
            assert 'Assertion failed:' in result.stderr, result.stderr
            print('KILLED: synthetic source '+name)
        else:
            assert result.returncode == 0, result.stderr
            print(result.stdout.strip())
PY
