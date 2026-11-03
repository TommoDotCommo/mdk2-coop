#!/usr/bin/env python3
"""dinput8.chain.dll (co-op mod standard J3): the mod's proxy loads another mod's dinput8.dll kept under that name, after its
own start-up, and sends DirectInput through it when it is a full dinput8 proxy itself. Runs the real bridge DLL in a small
host exe on a Windows scratch folder (no game, no slot). Fixtures:
  plugin   a chain DLL without DirectInput8Create: loaded, DirectInput from the system dinput8
  proxy    a chain DLL that exports DirectInput8Create: loaded, and the game's DirectInput8Create goes through it
  ours     a copy of the mod's own DLL as the chain file: never loaded (it would run the mod twice)
  none     no chain file: DirectInput from the system dinput8
Mutant (must fail): ignored   the bridge built with -DCOOP_MUTANT_NO_CHAIN never loads the chain file.
Usage: test_chain_load.py [--scratch <WSL path on a Windows filesystem>]"""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CC = 'i686-w64-mingw32-gcc'


def windows_temp():
    out = subprocess.run(['cmd.exe', '/c', 'echo %TEMP%'], capture_output=True, text=True, cwd='/mnt/c').stdout.strip()
    return Path(subprocess.run(['wslpath', '-u', out], capture_output=True, text=True).stdout.strip())


def build_bridge(out, mutant):
    """The bridge DLL from a copy of the tree (so a mutant never replaces bridge/dinput8.dll)."""
    src = out / 'src'
    shutil.copytree(ROOT / 'bridge', src / 'bridge', ignore=shutil.ignore_patterns('*.dll', '*.o', 'version.rc', 'diag_ps1.h', 'tests'))
    (src / 'tools').mkdir()
    shutil.copy(ROOT / 'tools/versioninfo.py', src / 'tools')
    (src / 'dist').mkdir()
    shutil.copy(ROOT / 'dist/diag.ps1', src / 'dist')
    flags = '-O2 -Wall -Wextra -Wno-unused-parameter' + (' -DCOOP_MUTANT_NO_CHAIN' if mutant else '')
    subprocess.run(['make', '-s', '-C', str(src / 'bridge'), 'CFLAGS=' + flags, 'VERSION=chaintest'], check=True, capture_output=True)
    return src / 'bridge/dinput8.dll'


def run_case(base, name, bridge, chain, host):
    d = base / name
    (d / 'coop').mkdir(parents=True)
    # a test copy: its own coop.ini keeps the bridge's files in this folder
    (d / 'coop/coop.ini').write_text('[test]\r\nbackground=1\r\n')
    shutil.copy(bridge, d / 'dinput8.dll')
    if chain:
        shutil.copy(chain, d / 'dinput8.chain.dll')
    shutil.copy(host, d / 'chain_host.exe')
    r = subprocess.run([str(d / 'chain_host.exe')], capture_output=True, text=True, timeout=60, cwd=str(d))
    fields = dict(kv.split('=', 1) for kv in r.stdout.split() if '=' in kv)
    return r.returncode, fields, r.stdout.strip()


def suite(base, bridge, fixtures, host):
    bad = []
    expect = {
        'plugin': (fixtures['plugin'], '1', '0'),
        'proxy': (fixtures['proxy'], '1', '1'),
        'ours': (bridge, '0', '-1'),
        'none': (None, '0', '-1'),
    }
    for name, (chain, loaded, hits) in expect.items():
        code, f, text = run_case(base, name, bridge, chain, host)
        ok = code == 0 and f.get('chain') == loaded and f.get('hits') == hits and f.get('hr') == '00000000'
        if not ok:
            bad.append('%s: want chain=%s hits=%s hr=0, got %r (exit %d)' % (name, loaded, hits, text, code))
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--scratch', type=Path)
    a = ap.parse_args()
    scratch = a.scratch or windows_temp()
    scratch.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix='mdk2-chain-', dir=str(scratch)))
    failures = []
    try:
        fixtures = {}
        for name, extra in (('plugin', []), ('proxy', ['-DFULL_PROXY'])):
            fixtures[name] = work / ('chain-%s.dll' % name)
            subprocess.run([CC, '-O2', '-shared', '-static-libgcc', '-Wl,--kill-at', *extra, '-o', str(fixtures[name]),
                            str(ROOT / 'bridge/tests/chain_fixture.c')], check=True)
        host = work / 'chain_host.exe'
        subprocess.run([CC, '-O2', '-static-libgcc', '-o', str(host), str(ROOT / 'bridge/tests/chain_host.c'), '-ldxguid'], check=True)
        for label, mutant in (('real', False), ('ignored', True)):
            out = work / label
            out.mkdir()
            bad = suite(out, build_bridge(out, mutant), fixtures, host)
            if not mutant:
                failures += bad
            elif not bad:
                failures.append('mutant %s survived' % label)
            else:
                print('KILLED: %s (%s)' % (label, bad[0]))
    finally:
        shutil.rmtree(work, ignore_errors=True)
    if failures:
        print('\n'.join('FAIL: ' + f for f in failures))
        return 1
    print('PASS: dinput8.chain.dll is loaded after the bridge (plug-in and full proxy), DirectInput goes through a full proxy, '
          'a copy of the mod is never chained; 1 mutant killed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
