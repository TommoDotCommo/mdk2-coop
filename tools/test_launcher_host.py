#!/usr/bin/env python3
"""Hosting checks of the launcher lobby, game-free (T0): router wire formats (NAT-PMP, UPnP-IGD), the CGNAT
check, Windows Firewall verdicts, invite routes, the lobby key, kicks and probe visibility, "Waiting for" and
the pad from recorded XInput states, with sanitisers, fuzzing and source mutants that must fail. With Windows
interop, the router code runs against a fake UPnP IGD and NAT-PMP responder on loopback (mutant: the mapping
is not removed when the lobby closes) and the firewall reader reads the real rules (read only).
Usage: test_launcher_host.py [--quick]"""
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

CORE_MUTANTS = {
    'secret-not-checked': ('if (invite_only && (!h->has_key || h->key != secret)) return LH_REFUSE_INVITE;',
                           'if (invite_only && !h->has_key) return LH_REFUSE_INVITE;'),
    'invite-only-ignored': ('if (invite_only && (!h->has_key || h->key != secret)) return LH_REFUSE_INVITE;', ''),
    'game-gate-open': ('if (!invite_only || known) return 1;', 'return 1;'),
    'game-gate-any-key': ('    if (key != secret) return 0;\n    if (lh_gate_find', '    if (lh_gate_find'),
    'kick-forgotten': ('if (lh_banned(bans, h->id, ip, port, h->name)) return LH_REFUSE_KICKED;', ''),
    'kick-rejoin-by-name': ('if (name && name[0] && b->b[i].name[0] && lh_path_same(b->b[i].name, name)) return 1;', ''),
    'kick-empty-name-matches': ('if (name && name[0] && b->b[i].name[0] && lh_path_same(b->b[i].name, name)) return 1;',
                                'if (name && lh_path_same(b->b[i].name, name)) return 1;'),
    'kick-by-id-only': ('if (b->b[i].id == id || (b->b[i].ip == ip && b->b[i].port == port)) return 1;', 'if (b->b[i].id == id) return 1;'),
    'kick-whole-address': ('if (b->b[i].id == id || (b->b[i].ip == ip && b->b[i].port == port)) return 1;',
                           'if (b->b[i].id == id || b->b[i].ip == ip) return 1;'),
    'cgnat-not-detected': ('if ((ip & 0xffc00000u) == 0x64400000u) return LH_WAN_CGNAT;', ''),
    'private-wan-public': ('if (lh_private_ip(ip) || (ip & 0xffff0000u) == 0xa9fe0000u) return LH_WAN_PRIVATE;', ''),
    'internet-without-check': ('if (internet_ok && lh_wan_class(internet_ip) == LH_WAN_PUBLIC && v->n < CI_MAX_ROUTES) {',
                               'if (internet_ip && v->n < CI_MAX_ROUTES) {'),
    'tailscale-route-lost': ('if (tailscale_ip && (tailscale_ip & 0xffc00000u) == 0x64400000u) {', 'if (0) {'),
    'block-not-winning': ('if (!r[i].allow) return LH_FW_BLOCKED;', 'if (!r[i].allow) continue;'),
    'firewall-tcp-counts': ('if (r[i].protocol != 17 && r[i].protocol != 256) continue;', ''),
    'firewall-profile-ignored': ('if (!r[i].enabled || !r[i].inbound || !(r[i].profiles & profile)) continue;',
                                 'if (!r[i].enabled || !r[i].inbound) continue;'),
    'firewall-any-program': ('if (r[i].app[0] && !lh_path_same(r[i].app, exe)) continue;', ''),
    'firewall-public-not-needed': ('(public_verdict == LH_FW_ALLOWED || public_verdict == LH_FW_OFF);', '1;'),
    'natpmp-other-port-accepted': ('|| lh_rd16(b + 8) != internal) return 0;', ') return 0;'),
    'ssdp-non-200-accepted': ('|| (n < 12 || (memcmp(s, "HTTP/1.1 200", 12) && memcmp(s, "HTTP/1.0 200", 12)))) return 0;', ') return 0;'),
    'relative-control-url': ("if (control[0] != '/') { memmove(control + 1, control, strlen(control) + 1); control[0] = '/'; }", ''),
    'waiting-counts-ready': ('for (i = 0; i < n; i++) waiting += !ready[i];', 'waiting = n;'),
    'pad-no-repeat': ('p->next[k] = now + LH_PAD_REPEAT;', 'p->next[k] = now + 100000u;'),
    'pad-start-not-primary': ('{ LH_PAD_START, LH_ACT_PRIMARY }', '{ LH_PAD_START, LH_ACT_CONFIRM }'),
    'pad-stick-ignored': ('if (ly < -LH_PAD_DEADZONE) buttons |= LH_PAD_DPAD_DOWN;', ''),
    'pad-held-repeats-a': ('unsigned cur = lh_pad_buttons(buttons, lx, ly), down = cur & ~p->held;',
                           'unsigned cur = lh_pad_buttons(buttons, lx, ly), down = cur;'),
    'tabs-no-wrap': ('if (action == LH_ACT_TAB_NEXT) return (tab + 1) % tabs;', 'if (action == LH_ACT_TAB_NEXT) return tab + 1;'),
}
WIN_MUTANTS = {
    'mapping-not-removed-on-close': ('    if (r->method == LHW_UPNP) lhw_upnp_call(r, c, LH_SOAP_DELETE, 0, &a);\n'
                                     '    else if (r->method == LHW_NATPMP) lhw_natpmp_map(r, c, 0);\n', '    (void)a;\n'),
    'no-natpmp-fallback': ('    if (r->method == LHW_NONE) lhw_natpmp_map(r, c, c->lease ? c->lease : LH_LEASE_S);\n', ''),
    'never-renewed': ('if (r->method == LHW_UPNP ? lhw_upnp_map(r, c) : lhw_natpmp_map(r, c, r->lease)) r->mapped_at = now;', ''),
    'kept-mapping-not-removed-on-quit': ('        lhw_router_renew(r, c, GetTickCount());\n    }\n    lhw_router_close(r, c);\n',
                                         '        lhw_router_renew(r, c, GetTickCount());\n    }\n'),
    'no-permanent-retry': ('if (a.error == 725 && lhw_upnp_call(r, c, LH_SOAP_ADD, 0, &a)) lease = 0;', 'if (0) lease = 0;'),
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


with tempfile.TemporaryDirectory(prefix='launcher-host-') as temp:
    temp = Path(temp)
    exe = temp / 'launcher_host_test'
    build(exe, BRIDGE / 'tests/launcher_host_test.c', BRIDGE, SANITIZE)
    r = run([str(exe), str(20000 if QUICK else 200000)])
    print(r.stdout.strip())
    if r.returncode:
        raise SystemExit(f'FAIL: launcher_host_test\n{r.stderr[-3000:]}')
    # the hosting core also compiles as C++ (the Witness vendors the invite headers that way)
    if shutil.which('g++'):
        r = run(['g++', '-std=c++11', '-fsyntax-only', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-x', 'c++',
                 str(BRIDGE / 'launcher_host_core.h')])
        if r.returncode:
            raise SystemExit(f'FAIL: launcher_host_core.h as C++\n{r.stderr[-2000:]}')
    for name, (before, after) in CORE_MUTANTS.items():
        folder = temp / f'm-{name}'
        folder.mkdir()
        mutate(folder, 'launcher_host_core.h', before, after)
        shutil.copy(BRIDGE / 'coop_invite.h', folder)
        exe = folder / 'test'
        build(exe, BRIDGE / 'tests/launcher_host_test.c', folder, ['-O1', '-Wno-unused-but-set-variable', '-Wno-unused-variable'])
        r = run([str(exe), '2000'], timeout=120)
        if r.returncode == 0:
            raise SystemExit(f'SURVIVED: {name}\n{r.stdout[-2000:]}')
        print('KILLED:', name)
    gcc = 'i686-w64-mingw32-gcc'
    if not (shutil.which('powershell.exe') and shutil.which(gcc)):
        print('SKIP: Windows router and firewall test (needs MinGW and Windows interop)')
        print('PASS: launcher host checks (core only)')
        sys.exit(0)
    wintemp = run(['powershell.exe', '-NoProfile', '-Command', '[IO.Path]::GetTempPath()']).stdout.strip()
    stage_win = wintemp + 'launcher-host-' + temp.name.split('-')[-1]
    stage = Path(run(['wslpath', '-u', stage_win]).stdout.strip())
    stage.mkdir(parents=True, exist_ok=True)
    try:
        def win_suite(header_dir, label, extra=()):
            out = stage / f'host-{label}.exe'
            build(out, BRIDGE / 'tests/launcher_host_win_test.c', header_dir, ['-O2', '-static-libgcc', *extra], gcc, ['-lws2_32'])
            return run(['powershell.exe', '-NoProfile', '-Command', f"& '{stage_win}\\{out.name}'; exit $LASTEXITCODE"], timeout=180)
        r = win_suite(BRIDGE, 'baseline')
        print(r.stdout.replace('\r', '').strip())
        if r.returncode:
            raise SystemExit('FAIL: Windows router and firewall test')
        for name, (before, after) in WIN_MUTANTS.items():
            folder = temp / f'w-{name}'
            folder.mkdir()
            mutate(folder, 'launcher_host_win.h', before, after)
            shutil.copy(BRIDGE / 'launcher_host_core.h', folder)
            r = win_suite(folder, name, ['-Wno-unused-but-set-variable', '-Wno-unused-variable'])
            if r.returncode == 0:
                raise SystemExit(f'SURVIVED: {name}\n{r.stdout[-2000:]}')
            print('KILLED:', name)
    finally:
        shutil.rmtree(stage, ignore_errors=True)
    print('PASS: launcher host checks')
