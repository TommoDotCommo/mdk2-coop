#!/usr/bin/env bash
# No game, no desktop. Exercise plist.c (the hold-Tab player list) and require broken twins to fail.
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
source = (root / 'bridge/plist.c').read_text()
mutants = {
    'release-keeps-it-open': ('    if (!want) {\n        close_list();\n        return;\n    }', '    if (!want) {\n        return;\n    }'),
    'mouse-not-held': ('        input_mouse_hold(1);\n        input_buttons_hold(1);', '        input_buttons_hold(1);'),
    'clicks-still-shoot': ('        input_buttons_hold(1);\n        px', '        px'),
    'held-click-repeats': ('    click_was = down;\n}', '    click_was = 0;\n}'),
    'watch-while-alive': ('if (out_of_fight && player_in_level(s)', 'if (player_in_level(s)'),
    'own-row-has-buttons': ('        if (s != me) {\n            int muted', '        if (1) {\n            int muted'),
    'mute-does-nothing': ('voice_set_muted(hit[i].slot, !voice_muted(hit[i].slot));', ''),
    'volume-step-wrong': ('(hit[i].kind == H_VOLUP ? 20 : -20)', '(hit[i].kind == H_VOLUP ? 20 : 20)'),
    'opens-outside-level': ('int want = hud_ingame() && have_remote', 'int want = have_remote'),
    'opens-over-guide': ('&& !guide_on() && !ctrlview_on()', '&& !ctrlview_on()'),
    'opens-over-wheel': ('!emote_wheel_open() && local_player', 'local_player'),
    'menu-keeps-the-mouse': ('    if (is_on && (!hud_ingame() || !have_remote))', '    if (0)'),
    'pointer-unclamped': ('    if (px > win_w - 1.0f) px = win_w - 1.0f;', ''),
    'dead-rows-listed': ('static int listed(int s)\n{\n    return s == my_slot || player_connected(s);', 'static int listed(int s)\n{\n    return 1;'),
}
with tempfile.TemporaryDirectory(prefix='plist-', dir=out) as temp:
    temp = Path(temp)
    for name, mutation in [('baseline', None), *mutants.items()]:
        binary = temp / (name + '.exe')
        command = ['i686-w64-mingw32-gcc', '-O2', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-unused-function',
                   '-I', str(root / 'bridge'), str(root / 'bridge/tests/plist_test.c'),
                   '-static-libgcc', '-o', str(binary)]
        if mutation:
            old, new = mutation
            if source.count(old) != 1:
                raise SystemExit(f'FAIL mutant {name}: expected exactly one source match, found {source.count(old)}')
            variant = temp / (name + '.c')
            variant.write_text(source.replace(old, new))
            command.insert(1, '-DPLIST_SOURCE="' + str(variant) + '"')
        subprocess.run(command, check=True, timeout=30)
        run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
        if not mutation:
            print(run.stdout, end='')
            if run.returncode:
                raise SystemExit(run.stderr or 'FAIL plist baseline')
        elif run.returncode == 0 or 'FAIL line ' not in run.stderr:
            raise SystemExit(f'FAIL mutant {name}: not rejected by a state assertion: {run.stdout}{run.stderr}')
        else:
            print(f'PASS mutant caught: {name}')
print(f'PASS: {len(mutants)} player list mutation twins rejected')
PY
