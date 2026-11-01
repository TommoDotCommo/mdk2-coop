#!/usr/bin/env python3
"""The one-file Setup (dist/installer.c) as a real Windows exe, driven from WSL; no game, no PowerShell installer UI
(the payload's installer-gui.ps1 is a marker writer). Checks extraction into a fresh per-version folder, the SHA-256 check
of the payload, refusal of links, and the "extract the whole zip" message for the payload-less Setup in a zip.
Mutants (separate builds of installer.c) must each fail the test they belong to:
  SETUP_MUTANT_NO_VERIFY    a damaged payload is trusted
  SETUP_MUTANT_FOLLOW_LINKS a junction in %LOCALAPPDATA%\\mdk2-coop\\setup is followed
Needs Windows interop and a scratch folder on a Windows filesystem (MDK2_RUN, as for tools/qa_installer.sh)."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import payload  # noqa: E402

GUI = ("[IO.File]::WriteAllText((Join-Path $PSScriptRoot 'marker.txt'), $PSScriptRoot)\r\n")
FILES = [('installer-gui.ps1', GUI.encode()), ('mdk2-coop.ps1', b'#backend'), ('install-state.ps1', b'#state'),
         ('install-policy.exe', b'MZ-fake'), ('dinput8.dll', b'MZ-dll' * 50), ('coop/coop.lua', b'-- lua\r\n' * 20)]


def scratch_root():
    base = os.environ.get('MDK2_RUN')       # a disposable folder on a Windows filesystem (see docs/DEVELOPMENT.md)
    if not base:
        sys.exit('set MDK2_RUN to a disposable folder on a Windows filesystem')
    return Path(base)


def win(path):
    return subprocess.check_output(['wslpath', '-w', str(path)], text=True).strip()


def build(work, name, defines=(), with_payload=True, version='v9.9.9-test'):
    work.mkdir(parents=True, exist_ok=True)
    objs = []
    if with_payload:
        pdir = work / ('p-' + name)
        payload.main(['x', 'pack', str(pdir), version] + ['%s=%s' % (n, _w(pdir, n, d)) for n, d in FILES])
        subprocess.run(['i686-w64-mingw32-windres', str(pdir / 'setup.rc'), '-o', str(pdir / 'res.o')], check=True)
        objs = [str(pdir / 'res.o')]
    else:   # the manifest matters: without it Windows treats a file named *Setup* as needing elevation
        subprocess.run(['i686-w64-mingw32-windres', 'installer.rc', '-o', str(work / 'stub-res.o')], cwd=ROOT / 'dist', check=True)
        objs = [str(work / 'stub-res.o')]
    exe = work / (name + '.exe')
    subprocess.run(['i686-w64-mingw32-gcc', '-Os', '-Wall', '-Wextra', '-mwindows'] + ['-D' + d for d in defines] +
                   ['-o', str(exe), str(ROOT / 'dist/installer.c')] + objs + ['-lshell32'], check=True)
    return exe


def _w(pdir, name, data):
    path = pdir / 'src' / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return path


class Run:
    def __init__(self, root):
        self.root = root
        self.local = root / 'local'
        self.temp = root / 'temp'
        self.log = root / 'messages.txt'
        for d in (self.local, self.temp):
            d.mkdir(parents=True, exist_ok=True)

    def run(self, exe, *args, wait_marker=None):
        env = dict(os.environ, LOCALAPPDATA=win(self.local), TEMP=win(self.temp), TMP=win(self.temp),
                   MDK2_COOP_SETUP_LOG=win(self.log), WSLENV='LOCALAPPDATA:TEMP:TMP:MDK2_COOP_SETUP_LOG')
        if self.log.exists():
            self.log.unlink()
        code = subprocess.run([str(exe)] + list(args), env=env, timeout=60).returncode
        if wait_marker is not None:
            deadline = time.time() + 30
            while time.time() < deadline and not wait_marker.exists():
                time.sleep(0.2)
        return code

    def messages(self):
        return self.log.read_bytes().decode('utf-16-le', 'replace') if self.log.exists() else ''


def rmdir_link(path):
    subprocess.run(['cmd.exe', '/c', 'rmdir', win(path)], capture_output=True)


def scenario_failures(exe_ok, exe_stub, work, label):
    bad = []

    def expect(cond, what):
        if not cond:
            bad.append('%s: %s' % (label, what))
    root = work / ('run-' + label + '-' + uuid.uuid4().hex[:6])
    r = Run(root)
    # 1. /extract writes the payload exactly, once
    out = root / 'out'
    expect(r.run(exe_ok, '/extract', win(out)) == 0, '/extract exits 0')
    same = all((out / n).is_file() and (out / n).read_bytes() == d for n, d in FILES)
    expect(same, '/extract writes every file byte for byte')
    (out / 'dinput8.dll').write_bytes(b'KEEP')
    expect(r.run(exe_ok, '/extract', win(out)) != 0, '/extract into an existing folder is refused')
    expect((out / 'dinput8.dll').read_bytes() == b'KEEP', '/extract never overwrites')
    # 2. normal start: fresh per-version folder, then the scripts start from there; a second run gets another folder
    first = r.local / 'mdk2-coop' / 'setup' / 'v9.9.9-test'
    expect(r.run(exe_ok, wait_marker=first / 'marker.txt') == 0, 'start exits 0')
    expect(first.is_dir() and (first / 'dinput8.dll').read_bytes() == FILES[4][1], 'files land in setup\\<version>')
    marker = first / 'marker.txt'
    expect(marker.exists() and marker.read_text().strip().lower().endswith('setup\\v9.9.9-test'), 'installer-gui.ps1 runs from the extracted folder')
    (first / 'dinput8.dll').write_bytes(b'EDITED')
    second = r.local / 'mdk2-coop' / 'setup' / 'v9.9.9-test-2'
    expect(r.run(exe_ok, wait_marker=second / 'marker.txt') == 0 and second.is_dir(), 'second start gets a fresh folder')
    expect((first / 'dinput8.dll').read_bytes() == b'EDITED', 'an earlier folder is never reused or overwritten')
    # 3. a damaged payload is refused before anything is written
    data = bytearray(exe_ok.read_bytes())
    blob = payload.read_exe(exe_ok)[2]
    at = bytes(data).find(blob)
    data[at + len(blob) - 5] ^= 0xFF           # inside the last file's bytes: the container still parses
    damaged = exe_ok.with_name('damaged-' + label + '.exe')
    damaged.write_bytes(bytes(data))
    r2 = Run(root / 'damaged')
    code = r2.run(damaged)
    expect(code == 3 and 'damaged' in r2.messages(), 'a damaged payload exits 3 and says it is damaged')
    expect(not (r2.local / 'mdk2-coop' / 'setup' / 'v9.9.9-test').exists(), 'a damaged payload writes nothing')
    # 4. a junction in place of the setup folder is refused; nothing goes through it
    r3 = Run(root / 'link')
    target = root / 'link-target'
    target.mkdir()
    (r3.local / 'mdk2-coop').mkdir(parents=True)
    subprocess.run(['cmd.exe', '/c', 'mklink', '/J', win(r3.local / 'mdk2-coop' / 'setup'), win(target)], check=True, capture_output=True)
    try:
        code = r3.run(exe_ok)
        leaked = list(target.rglob('*'))
        expect(code == 4, 'a junction as the setup folder exits 4')
        expect(not leaked, 'nothing is written through a junction (%d entries leaked)' % len(leaked))
        # 4b. the version folder itself being a junction: skip it, never write into it
    finally:
        rmdir_link(r3.local / 'mdk2-coop' / 'setup')
    r4 = Run(root / 'link2')
    (r4.local / 'mdk2-coop' / 'setup').mkdir(parents=True)
    target2 = root / 'link2-target'
    target2.mkdir()
    subprocess.run(['cmd.exe', '/c', 'mklink', '/J', win(r4.local / 'mdk2-coop' / 'setup' / 'v9.9.9-test'), win(target2)], check=True, capture_output=True)
    try:
        r4.run(exe_ok, wait_marker=r4.local / 'mdk2-coop' / 'setup' / 'v9.9.9-test-2' / 'marker.txt')
        expect(not list(target2.rglob('*')), 'a junction named like the version folder is not written through')
        expect((r4.local / 'mdk2-coop' / 'setup' / 'v9.9.9-test-2' / 'dinput8.dll').exists(), 'Setup moves on to a fresh folder instead')
    finally:
        rmdir_link(r4.local / 'mdk2-coop' / 'setup' / 'v9.9.9-test')
    return bad, exe_stub


def stub_failures(stub, work):
    bad = []
    root = work / ('stub-' + uuid.uuid4().hex[:6])

    def expect(cond, what):
        if not cond:
            bad.append('stub: ' + what)
    # scripts missing beside the Setup (double-clicked inside the zip, or copied out alone)
    r = Run(root / 'alone')
    lone = r.root / 'MDK2-Coop-Setup.exe'
    shutil.copy(stub, lone)
    code = r.run(lone)
    text = r.messages()
    expect(code == 5, 'missing scripts exit 5 (got %d)' % code)
    expect('Extract the whole zip first' in text and 'single MDK2-Coop-Setup.exe' in text, 'the message says to extract or download the single Setup.exe')
    # everything beside it: starts the script
    r = Run(root / 'whole')
    for name, data in FILES:
        p = r.root / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
    whole = r.root / 'MDK2-Coop-Setup.exe'
    shutil.copy(stub, whole)
    expect(r.run(whole, wait_marker=r.root / 'marker.txt') == 0 and (r.root / 'marker.txt').exists(), 'with the files beside it Setup starts the installer')
    # even with the files beside it, a path inside Explorer's zip temp folder is not a real extraction
    r = Run(root / 'ziptemp')
    zipdir = r.temp / 'Temp1_mdk2-coop-v1.zip' / 'mdk2-coop-v1'
    zipdir.mkdir(parents=True)
    for name, data in FILES:
        p = zipdir / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
    inzip = zipdir / 'MDK2-Coop-Setup.exe'
    shutil.copy(stub, inzip)
    code = r.run(inzip)
    expect(code == 5 and 'Extract the whole zip first' in r.messages(), 'a path under %TEMP% with a .zip folder gets the extract message')
    return bad


def main():
    work = scratch_root() / ('setup-exe-' + uuid.uuid4().hex[:6])
    work.mkdir(parents=True)
    failures = []
    try:
        good = build(work, 'good')
        stub = build(work, 'stub', with_payload=False)
        bad, _ = scenario_failures(good, stub, work, 'real')
        failures += bad
        failures += stub_failures(stub, work)
        for define, label in (('SETUP_MUTANT_NO_VERIFY', 'no-verify'), ('SETUP_MUTANT_FOLLOW_LINKS', 'follow-links')):
            mutant = build(work, label, defines=[define])
            bad, _ = scenario_failures(mutant, stub, work, label)
            if not bad:
                failures.append('mutant %s survived' % define)
            else:
                print('mutant %s caught by: %s' % (define, bad[0]))
    finally:
        for p in work.rglob('*'):
            pass
        shutil.rmtree(work, ignore_errors=True)
    if failures:
        print('\n'.join('FAIL: ' + f for f in failures))
        return 1
    print('PASS: single-file Setup extracts to a fresh folder, verifies its payload, refuses links, and the zip Setup explains itself; 2 mutants')
    return 0


if __name__ == '__main__':
    sys.exit(main())
