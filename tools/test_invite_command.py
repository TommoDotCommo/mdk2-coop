#!/usr/bin/env python3
"""Setup invite argument, bounds and mutants; actual Windows argv quoting when interop is available."""
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / 'bridge/launcher_invite_command.h').read_text()
MUTANTS = {
    'substring-flag': ('wcscmp(argv[i], L"--coop-invite")', '!wcsstr(argv[i], L"--coop-invite")'),
    'duplicate-invite': ('found || i + 1 == argc', 'i + 1 == argc'),
    'truncate-invite': ('!n || n >= cap || n >= LI_INPUT_CAP', '!n || n >= LI_INPUT_CAP'),
    'control-input': ("argv[i][k] < 32 || argv[i][k] > 126 || argv[i][k] == L'\"'", '0'),
}
killed = 0
with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)
    for name, change in [('real', None), *MUTANTS.items()]:
        source = SOURCE
        if change:
            assert source.count(change[0]) == 1, name
            source = source.replace(*change)
        (tmp / 'launcher_invite_command.h').write_text(source)
        binary = tmp / 'invite-test'
        subprocess.run(['cc', '-std=c99', '-Wall', '-Wextra', '-Werror', '-I', str(tmp),
                        str(ROOT / 'bridge/tests/invite_command_test.c'), '-o', str(binary)], check=True)
        result = subprocess.run([str(binary)], capture_output=True)
        if name == 'real':
            assert result.returncode == 0, result.stderr
        else:
            assert result.returncode != 0, f'{name} survived'
            killed += 1
            print(f'KILLED: {name}')
    if shutil.which('i686-w64-mingw32-gcc'):
        (tmp / 'launcher_invite_command.h').write_text(SOURCE)
        binary = tmp / 'invite-test.exe'
        subprocess.run(['i686-w64-mingw32-gcc', '-Wall', '-Wextra', '-Werror', '-I', str(tmp),
                        str(ROOT / 'bridge/tests/invite_command_test.c'), '-lshell32', '-o', str(binary)], check=True)
        if Path('/proc/sys/fs/binfmt_misc/WSLInterop').exists():
            subprocess.run([str(binary), '--coop-invite', 'mdk2-coop://MDK2-ABC12-DE345'], check=True)
            print('PASS: Windows command-line quotes preserve the invite URL')
            # Compile the actual launcher hook with a paste-box adapter, without starting the game.
            # Verify text transfer, recognition and overflow handling at the Windows boundary.
            launcher = (ROOT / 'bridge/launcher.c').read_text()
            start = launcher.index('    {\n        int argc = 0;', launcher.index('void launcher_run(void)'))
            end = launcher.index('    snprintf(test_ini', start)
            hook = launcher[start:end]
            gate_start = launcher.index('if (!cfg.show', end) + 4
            gate_end = launcher.index(') {', gate_start)
            gate = launcher[gate_start:gate_end]
            prelude = r'''
#include <windows.h>
#include <shellapi.h>
#include <assert.h>
#include <string.h>
#include "launcher_invite_command.h"
#include "coop_invite.h"
#define LAUNCHER_JOIN_H 1
#define LJ_GAME "MDK2"
enum { MODE_HOST, MODE_JOIN, MODE_SOLO };
static struct { int mode, show; } cfg;
static char launcher_invite_text[LI_INPUT_CAP];
static struct { char paste[256], message[256]; struct cr_result rec; int selected; } lju;
static int initialized;
static void lj_init(void) { initialized++; }
int main(void) {
'''
            prelude = prelude.replace('int main(void) {', 'static int should_open(void) { int shift = 0, headless = 0; return !(' + gate + '); }\nint main(void) {')
            assertions = r'''
    int argc;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    char expected[LI_INPUT_CAP];
    int present = li_invite_argument(argc, (const wchar_t *const *)argv, expected, sizeof(expected));
    if (present == 1) {
        struct cr_result rec;
        assert(cfg.mode == MODE_JOIN && initialized == 1 && should_open());
        assert(!strcmp(launcher_invite_text, expected));
        if (strlen(expected) < sizeof(lju.paste)) {
            assert(!strcmp(lju.paste, expected));
            coop_recognise(expected, strlen(expected), LJ_GAME, &rec);
            assert(lju.rec.kind == rec.kind && lju.rec.error == rec.error);
            assert(lju.selected == -1);
        } else assert(!lju.paste[0] && strstr(lju.message, "too long"));
    } else assert(cfg.mode == MODE_HOST && !initialized && !launcher_invite_text[0] && !should_open());
    LocalFree(argv);
    return 0;
}
'''
            twins = {
                'invite-mode-lost': ('cfg.mode = MODE_JOIN;', ''),
                'paste-recognition-lost': ('coop_recognise(lju.paste, strlen(lju.paste), LJ_GAME, &lju.rec);', ''),
                'invite-skips-launcher': (' && !launcher_invite_text[0]', ''),
                'paste-handoff-lost': ('#ifdef LAUNCHER_JOIN_H', '#if 0'),
            }
            for label, twin in [('real', None), *twins.items()]:
                original = prelude + hook + assertions
                code = original if twin is None else original.replace(*twin)
                if twin:
                    assert code != original, label
                src = tmp / 'hook-test.c'
                src.write_text(code)
                exe = tmp / 'hook-test.exe'
                subprocess.run(['i686-w64-mingw32-gcc', '-Wall', '-Wextra', '-Werror',
                                '-Wno-unused-function', '-I', str(tmp), '-I', str(ROOT / 'bridge'),
                                str(src), '-lshell32', '-o', str(exe)], check=True)
                results = [subprocess.run([str(exe), *args], capture_output=True).returncode for args in (
                    [], ['--coop-invite', 'mdk2-coop://MDK2-ABC12-DE345'],
                    ['--coop-invite', 'x' * 300], ['--coop-invite'])]
                if twin:
                    assert any(results), f'{label} survived'
                    killed += 1
                    print(f'KILLED: {label}')
                else:
                    assert not any(results), results
            print('PASS: actual launcher hook hands URL text to the Join paste box without truncation')

    print(f'PASS: invite argument extraction; {killed} mutants killed')
