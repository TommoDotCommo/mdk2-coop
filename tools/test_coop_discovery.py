#!/usr/bin/env python3
"""Invites and finding friends, game-free: invite code v1, paste box, clipboard finder, probe v2,
candidates, rows, Tailscale status parser and recents store (T0 with sanitisers and fuzzing),
source mutants that must fail, C++ and MinGW builds of the shared headers, and the Windows
adapter self-test (adapters, tailscale.exe stand-ins, loopback fake hosts) when Windows interop
is available. Usage: test_coop_discovery.py [--quick]"""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BRIDGE = ROOT / 'bridge'
QUICK = '--quick' in sys.argv
CFLAGS = ['-std=c99', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter']
SANITIZE = ['-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all']

INVITE_MUTANTS = {
    'crc-not-checked': ('|| ci_crc8(b, n - 1) != b[n - 1]) return CI_INCOMPLETE;', ') return CI_INCOMPLETE;'),
    'prefix-not-checked': ('if (which != mine) {', 'if (0) {'),
    'route-order-lost': ('t.route[i].kind = b[at];\n        memcpy(t.route[i].ip, b + at + 1, 4);',
                         't.route[t.n - 1 - i].kind = b[at];\n        memcpy(t.route[t.n - 1 - i].ip, b + at + 1, 4);'),
    'padding-not-checked': ('if (bits >= 5 || acc) return CI_INCOMPLETE;', 'if (bits >= 5) return CI_INCOMPLETE;'),
    'length-not-checked': ('    if (n != need) return CI_INCOMPLETE;\n', ''),
    'version-zero-accepted': ('    if (b[0] >> 4 != CI_VERSION) return CI_INCOMPLETE;\n', ''),
    'newer-not-reported': ('if (b[0] >> 4 > CI_VERSION) return CI_NEWER;', 'if (0) return CI_NEWER;'),
    'route-kind': ('if (b[at] < CI_LAN || b[at] > CI_INTERNET) return CI_INCOMPLETE;', 'if (0) return CI_INCOMPLETE;'),
    'alias-I-L': ("if (c == 'I' || c == 'L') return 1;", ''),
    'links-ignored': ('if (i && i + 3 <= len && !memcmp(s + i, "://", 3)) {', 'if (0) {'),
    'short-prefix-host': ('if (chars < 5) i = len;', ''),
    'ts-net-suffix': ('!strcmp(r->host + n - 7, ".ts.net")', '0'),
    'clipboard-boundary': ('if ((i && ci_alnum((unsigned char)text[i - 1])) || ', 'if ('),
    'clipboard-name': ('if (k - s <= CI_NAME_MAX * 2) ci_name_clean(text + s, k - s, name);', ''),
    'name-control-bytes': ('if (cp < 0x20 || cp == 0x7f || ', 'if ('),
    'lobby-digits': ('if (len >= 17 && len <= 19 && ', 'if (len >= 16 && len <= 20 && '),
    'ipv4-leading-zero': ("(i - start > 1 && s[start] == '0') || ", ''),
}
DISCOVER_MUTANTS = {
    'broadcast-only-global': ('broadcast = a[i].ip | ~mask;', 'broadcast = 0xffffffffu;'),
    'reply-wrong-nonce': ('if (cd_le32(b + 4) != nonce || memcmp(b + 8, game_id, 4)) return 0;', 'if (memcmp(b + 8, game_id, 4)) return 0;'),
    'reply-game-not-checked': ('if (cd_le32(b + 4) != nonce || memcmp(b + 8, game_id, 4)) return 0;', 'if (cd_le32(b + 4) != nonce) return 0;'),
    'answer-game-not-checked': ('|| memcmp(q + 8, h->game_id, 4)) return 0;', ') return 0;'),
    'invite-only-name-leak': ('nlen = h->invite_only || !h->name ? 0 :', 'nlen = !h->name ? 0 :'),
    'invite-only-listed': ('if ((rep->flags & CD_F_INVITE_ONLY) && group < 0) return -1;', ''),
    'invite-only-name-accepted': ('if ((t.flags & CD_F_INVITE_ONLY) && nlen) return 0;', ''),
    'rate-not-limited': ('if (*tokens <= 0) return 0;', ''),
    'rate-no-total-cap': ('return cd_bucket(&r->total_last, &r->total_tokens, &r->total_used, CD_RATE_TOTAL, now);', 'return 1;'),
    'cpq1-dropped': ('if (n == 8 && !memcmp(q, "CPQ1", 4)) {', 'if (0) {'),
    'any-source-accepted': (': c->ip == ip)', ': 1)'),
    'route-preference-lost': ('cd_row_best(row);\n    return (int)(row - l->r);', 'row->route = route; row->ip = ip; row->rtt = rtt;\n    return (int)(row - l->r);'),
    'stale-kept': ('> CD_STALE_MS) l->r[i].has[k] = 0;', '> CD_STALE_MS * 100u) l->r[i].has[k] = 0;'),
    'refresh-too-often': ('< CD_REFRESH_MS) return 0;', '< 500u) return 0;'),
    'traffic-while-hidden': ('if (!shown) { f->ever = 0; return 0; }', 'if (!shown) f->ever = 0;'),
    'no-preference-window': ('>= CD_PREFER_MS) return best;', '>= 1u) return best;'),
    'tailscale-offline-probed': ('if (!p.online) t->offline++;', 'if (0) t->offline++;'),
    'tailscale-stopped-probed': ('    if (!t->running) t->n = 0;\n', ''),
    'tailscale-trailing-garbage': ('if (j.i != j.n) { memset(t, 0, sizeof(*t)); return 0; }', 'if (0) { return 0; }'),
    'tailscale-adapter-broadcast': (' || cd_tailscale_ip(a[i].ip)) continue;', ') continue;'),
    'migration-dropped': ('else if (!strncmp(buffer, "COOP-PROFILES\\t1\\n", 16)) { format = 1;', 'else if (0) { format = 1;'),
    'pinned-evicted': ('if (!s->r[i].pinned && (oldest < 0', 'if ((oldest < 0'),
    'recents-duplicates': ('for (j = 0; j < t.n; j++) if (cd_same_host(t.r[j].host, e->host) && t.r[j].port == e->port) return 0;', ''),
    'port-range-overflow': ('if (!ip || !port || port > 65535) return 0;', 'if (!ip || !port) return 0;'),
}
WIN_MUTANTS = {
    'tailscale-no-timeout': ('if (GetTickCount() - start >= timeout_ms) { done = -1; break; }', ''),
    'tailscale-no-w6432': ('{ "ProgramW6432", "ProgramFiles" }', '{ "ProgramFiles", "ProgramFiles" }'),
    'scan-any-source': ('at = cd_match(c, ntohl(from.sin_addr.s_addr), ntohs(from.sin_port));', 'at = 0;'),
}


def run(cmd, timeout=600, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, **kw)


def build(out, test, headers, extra=(), compiler='cc', libs=()):
    r = run([compiler, *CFLAGS, *extra, '-I', str(headers), '-I', str(BRIDGE), str(test), '-o', str(out), *libs])
    if r.returncode:
        raise SystemExit(f'FAIL: build {test.name}\n{r.stderr}')


def mutate(folder, name, before, after):
    source = (BRIDGE / name).read_text()
    if source.count(before) != 1:
        raise SystemExit(f'FAIL: mutant text found {source.count(before)} times in {name}: {before!r}')
    (folder / name).write_text(source.replace(before, after))


with tempfile.TemporaryDirectory(prefix='coop-discovery-') as temp:
    temp = Path(temp)
    # T0 suites with sanitisers and fuzzing
    for test, fuzz in (('coop_invite_test.c', 20000 if QUICK else 200000), ('coop_discover_test.c', 10000 if QUICK else 100000)):
        exe = temp / test.replace('.c', '')
        build(exe, BRIDGE / 'tests' / test, BRIDGE, SANITIZE)
        r = run([str(exe), str(fuzz)])
        print(r.stdout.strip())
        if r.returncode:
            raise SystemExit(f'FAIL: {test}\n{r.stderr[-3000:]}')
    # mutants: each must compile and then fail its suite
    for header, test, mutants in (('coop_invite.h', 'coop_invite_test.c', INVITE_MUTANTS),
                                  ('coop_discover.h', 'coop_discover_test.c', DISCOVER_MUTANTS)):
        for name, (before, after) in mutants.items():
            folder = temp / f'm-{name}'
            folder.mkdir()
            mutate(folder, header, before, after)
            exe = folder / 'test'
            build(exe, BRIDGE / 'tests' / test, folder, ['-O1', '-Wno-unused-but-set-variable', '-Wno-unused-variable'])
            r = run([str(exe), '3000'], timeout=120)
            if r.returncode == 0:
                raise SystemExit(f'SURVIVED: {name}\n{r.stdout[-2000:]}')
            print('KILLED:', name)
    # the headers as C++ (The Witness vendors them) and with both MinGW compilers
    cpp = temp / 'cpp.cc'
    cpp.write_text('#include "coop_discover.h"\nint main(){struct cr_result r;struct cd_ts t;'
                   'return coop_recognise("x",1,"WIT",&r)+cd_ts_parse("{}",2,&t);}\n')
    for compiler in ('g++', 'x86_64-w64-mingw32-g++'):
        if shutil.which(compiler):
            r = run([compiler, '-std=c++11', '-Wall', '-Wextra', '-Werror', '-I', str(BRIDGE), '-c', str(cpp), '-o', str(temp / 'cpp.o')])
            if r.returncode:
                raise SystemExit(f'FAIL: {compiler} cannot compile the shared headers\n{r.stderr}')
            print(f'PASS: shared headers compile as C++ ({compiler})')
    win = shutil.which('powershell.exe') and shutil.which('i686-w64-mingw32-gcc')
    if not win:
        print('SKIP: Windows adapter self-test (needs MinGW and Windows interop)')
        sys.exit(0)
    for test in ('coop_invite_test.c', 'coop_discover_test.c'):
        exe = temp / test.replace('.c', '.exe')
        build(exe, BRIDGE / 'tests' / test, BRIDGE, ['-O2', '-static-libgcc'], 'i686-w64-mingw32-gcc')
        r = run([str(exe), '5000'], timeout=120)
        if r.returncode:
            raise SystemExit(f'FAIL: {test} built for Windows\n{r.stdout[-2000:]}')
        print('PASS: 32-bit Windows build of', test)

    # Windows adapter: executables in the Windows temp folder (CreateProcess refuses \\wsl.localhost)
    wintemp = run(['powershell.exe', '-NoProfile', '-Command', '[IO.Path]::GetTempPath()']).stdout.strip()
    stage_win = wintemp + 'coop-discovery-' + temp.name.split('-')[-1]
    stage = Path(run(['wslpath', '-u', stage_win]).stdout.strip())
    try:
        for d in ('print', 'hang', 'w6432/Tailscale', 'empty'):
            (stage / d).mkdir(parents=True, exist_ok=True)
        gcc = 'i686-w64-mingw32-gcc'
        for out, extra in ((stage / 'print/tailscale.exe', []), (stage / 'hang/tailscale.exe', ['-DFAKE_HANG']),
                           (stage / 'w6432/Tailscale/tailscale.exe', [])):
            r = run([gcc, '-O2', *extra, str(BRIDGE / 'tests/coop_fake_tailscale.c'), '-o', str(out), '-static-libgcc'])
            if r.returncode:
                raise SystemExit('FAIL: build tailscale stand-in\n' + r.stderr)
        cases = [('fixture', f"$env:PATH='{stage_win}\\print'", ''),
                 ('timeout', f"$env:PATH='{stage_win}\\hang'", ''),
                 ('missing', "$env:PATH='C:\\nonexistent-coop'", f"Set-Location '{stage_win}\\print'; "),
                 ('fixture', f"$env:PATH='C:\\nonexistent-coop'; $env:ProgramW6432='{stage_win}\\w6432'", '')]

        def windows(exe, args, env='', pre='', timeout=60):
            script = (f"{env}; $env:ProgramFiles='C:\\nonexistent-coop'; " if env else '') + pre + \
                     f"& '{exe}' {args}; exit $LASTEXITCODE"
            return run(['powershell.exe', '-NoProfile', '-Command', script], timeout=timeout)

        def leftovers():
            r = run(['powershell.exe', '-NoProfile', '-Command',
                     f"@(Get-CimInstance Win32_Process | Where-Object {{ $_.ExecutablePath -like '{stage_win}\\*' }}).Count"])
            return int(r.stdout.strip() or 0)

        def adapter_suite(header_dir, label, extra=()):
            exe = stage / f'selftest-{label}.exe'
            build(exe, BRIDGE / 'tests/coop_discover_win_test.c', header_dir, ['-O2', '-static-libgcc', *extra], gcc, ['-lws2_32'])
            exe_win = f'{stage_win}\\{exe.name}'
            outputs, ok = [], True
            r = windows(exe_win, '')
            outputs.append(r.stdout.replace('\r', ''))
            ok &= r.returncode == 0
            for expect, env, pre in cases:
                if 'ProgramW6432' not in env:
                    env += "; $env:ProgramW6432='C:\\nonexistent-coop'"
                r = windows(exe_win, f'--tailscale-expect {expect}', env, pre)
                outputs.append(f'[{expect}] ' + r.stdout.replace('\r', ''))
                ok &= r.returncode == 0
            if leftovers():
                outputs.append('a stand-in tailscale.exe was still running after the suite')
                ok = False
            return ok, ''.join(outputs)

        ok, output = adapter_suite(BRIDGE, 'baseline')
        print(output.strip())
        if not ok:
            raise SystemExit('FAIL: Windows adapter self-test')
        for name, (before, after) in WIN_MUTANTS.items():
            folder = temp / f'w-{name}'
            folder.mkdir()
            mutate(folder, 'coop_discover_win.h', before, after)
            shutil.copy(BRIDGE / 'coop_discover.h', folder)
            shutil.copy(BRIDGE / 'coop_invite.h', folder)
            ok, output = adapter_suite(folder, name, ['-Wno-unused-but-set-variable', '-Wno-unused-variable'])
            if ok:
                raise SystemExit(f'SURVIVED: {name}\n{output[-2000:]}')
            print('KILLED:', name)
    finally:
        if stage.exists() and not leftovers():
            shutil.rmtree(stage, ignore_errors=True)
print(f'PASS: coop discovery: {len(INVITE_MUTANTS) + len(DISCOVER_MUTANTS) + len(WIN_MUTANTS)} mutants killed')
