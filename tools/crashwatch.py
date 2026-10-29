#!/usr/bin/env python3
"""Crash detection for test runs: a run that crashes any of its game copies must fail.

The Overhaul build writes dump%04i.dmp files into one per-user crash folder that every copy
shares, and shows an error dialog ("Jinkies! There was an error...") that stalls the game.
The mod also leaves coop/logs/*.crash next to its session log for every crash it sees, on
every build. This tool watches for all three:

    crashwatch.py begin  --state F --crash-dir D... --dir GAMEDIR...   snapshot before a run
    crashwatch.py expect --state F N                                N deliberate crashes
    crashwatch.py record --state F KIND TEXT                        add a finding by hand
    crashwatch.py dialogs --state F [--close]                       record (and close) error dialogs
    crashwatch.py findings --state F                                list what was seen
    crashwatch.py collect --state F DIR                             move our deliberate-crash dumps out of the player's folder
    crashwatch.py check  --state F                                  report; exit 1 if unexpected

A dump found after `begin` is attributed to a game copy by the image paths inside it, so a
crash of another agent's copy that shares the folder is reported but does not fail this run.
--crash-dir may be given more than once: the real crash folder and each isolated copy's own
(bridge/redirect.c points the Overhaul handler at <game dir>\\profile\\MDK2HD\\crash). A dump in a folder
inside a watched game copy is that copy's by location; one in any other folder is attributed by image path.
Game copy paths are Windows paths (as in MDK2_DIR1); the folders are given as paths this
process can read (WSL or native) and are mapped by --map-win when the two differ.
"""
import argparse
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import minidump  # noqa: E402


def norm(path):
    return path.replace('/', '\\').rstrip('\\').lower()


def wsl_path(win):
    """Windows path -> path this process can open (WSL: /mnt/c/...; native Windows: unchanged)."""
    if os.name == 'nt' or not re.match(r'^[A-Za-z]:[\\/]', win):
        return win
    return '/mnt/%s/%s' % (win[0].lower(), win[3:].replace('\\', '/'))


def list_dumps(crash_dir):
    try:
        return {n: os.stat(os.path.join(crash_dir, n)).st_size
                for n in os.listdir(crash_dir) if n.lower().endswith('.dmp')}
    except OSError:
        return {}


def list_markers(game_dir):
    logs = os.path.join(wsl_path(game_dir), 'coop', 'logs')
    try:
        return sorted(n for n in os.listdir(logs) if n.endswith('.crash'))
    except OSError:
        return []


def load(state):
    with open(state) as f:
        return json.load(f)


def save(state, data):
    tmp = state + '.tmp'
    with open(tmp, 'w') as f:
        json.dump(data, f, indent=1)
    os.replace(tmp, state)


def as_list(crash_dirs):
    return [crash_dirs] if isinstance(crash_dirs, str) else list(crash_dirs)


def all_dumps(crash_dirs):
    """{full path: size} over every watched crash folder."""
    out = {}
    for c in crash_dirs:
        folder = wsl_path(c)
        for name, size in list_dumps(folder).items():
            out[os.path.join(folder, name)] = size
    return out


def inside(folder, dirs):
    f = norm(folder)
    return any(f.startswith(norm(wsl_path(d)) + '\\') for d in dirs)


def begin(state, crash_dir, dirs):
    crash_dirs = as_list(crash_dir)
    save(state, {'crash_dir': crash_dirs[0] if crash_dirs else '', 'crash_dirs': crash_dirs, 'dirs': dirs,
                 'dumps': all_dumps(crash_dirs),
                 'markers': {d: list_markers(d) for d in dirs}, 'expected': 0, 'manual': [],
                 'dialogs': [], 'started': time.time()})


def settle(path, tries=20):
    """Wait for a dump that is still being written to stop growing."""
    last = -1
    for _ in range(tries):
        size = os.path.getsize(path)
        if size == last:
            return
        last = size
        time.sleep(0.25)


def attribute(dump_path, dirs):
    """(owner game dir or None, text) for a dump file, from the image paths recorded in it."""
    try:
        settle(dump_path)
        with open(dump_path, 'rb') as f:
            dump = minidump.Minidump(f.read())
    except (OSError, minidump.MinidumpError, struct.error) as exc:
        return None, 'unreadable (%s)' % exc
    text = '; '.join(minidump.describe(dump, 0)[:2])
    wanted = [norm(d) for d in dirs]
    for module in dump.modules:
        image = norm(module['name'])
        for d, w in zip(dirs, wanted):
            if image.startswith(w + '\\'):
                return d, text
    return '', text


def watched(s):
    return s.get('crash_dirs') or [s['crash_dir']]


def findings(state_file):
    """All crash findings since `begin`: list of dicts {kind, text, ours}."""
    s = load(state_file)
    out = []
    for path in sorted(all_dumps(watched(s))):
        if path in s['dumps']:
            continue
        owner, text = attribute(path, s['dirs'])
        if owner == '' and inside(os.path.dirname(path), s['dirs']):
            owner = os.path.dirname(path)          # in a copy's own crash folder: that copy's, by location
        if owner is None:
            out.append({'kind': 'dump', 'ours': True, 'text': '%s: %s' % (path, text)})
        elif owner == '':
            out.append({'kind': 'foreign-dump', 'ours': False, 'text': '%s: %s' % (path, text)})
        else:
            out.append({'kind': 'dump', 'ours': True, 'text': '%s: %s' % (path, text)})
    for d in s['dirs']:
        for name in list_markers(d):
            if name not in s['markers'].get(d, []):
                out.append({'kind': 'crash-marker', 'ours': True,
                            'text': os.path.join(wsl_path(d), 'coop', 'logs', name)})
    for d in s['dialogs']:
        out.append({'kind': 'dialog', 'ours': True, 'text': d})
    for kind, text in s['manual']:
        out.append({'kind': kind, 'ours': True, 'text': text})
    return out


def verdict(state_file):
    """(ok, lines). One deliberate crash legitimately produces a dump, a marker and maybe a
    dialog, so `expect N` allows N crash *events* of each kind, not N findings in total.
    Lines start with CRASH (fails the run), expected (allowed) or note (another copy's)."""
    s = load(state_file)
    lines = []
    ok = True
    by_kind = {}
    for f in findings(state_file):
        by_kind.setdefault(f['kind'], []).append(f)
    for kind, items in sorted(by_kind.items()):
        allowed = s['expected']
        for i in items:
            if not i['ours']:
                lines.append('note (other copy) %s: %s' % (kind, i['text']))
            elif allowed > 0:
                allowed -= 1
                lines.append('expected %s: %s' % (kind, i['text']))
            else:
                ok = False
                lines.append('CRASH %s: %s' % (kind, i['text']))
    return ok, lines


def collect(state_file, target):
    """Move the dumps this run made of its own copies into target (deliberate crashes only: the
    shared crash folder belongs to the player, test dumps do not belong in it)."""
    s = load(state_file)
    os.makedirs(target, exist_ok=True)
    moved = []
    for path in sorted(all_dumps(watched(s))):
        if path in s['dumps']:
            continue
        owner, _ = attribute(path, s['dirs'])
        if owner or (owner == '' and inside(os.path.dirname(path), s['dirs'])):
            dest = os.path.join(target, os.path.basename(path))
            n = 1
            while os.path.exists(dest):          # two copies' folders both number their dumps from 0
                dest = os.path.join(target, '%d-%s' % (n, os.path.basename(path))); n += 1
            os.replace(path, dest) if os.stat(path).st_dev == os.stat(target).st_dev else (
                shutil.copy2(path, dest), os.remove(path))
            moved.append(dest)
    return moved


def powershell_dialogs(dirs, close):
    script = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'win', 'dialogs.ps1')
    if os.name != 'nt':
        script = subprocess.check_output(['wslpath', '-w', script]).decode().strip()
    cmd = ['powershell.exe', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', script,
           '-Dirs', ','.join(dirs)] + (['-Close'] if close else [])
    out = subprocess.run(cmd, capture_output=True, text=True, timeout=60).stdout
    return [l.strip() for l in out.replace('\r', '').splitlines() if l.count('|') >= 3]


def record_dialogs(state_file, close, lines=None):
    if lines is None and os.environ.get('MDK2_CRASH_DIALOGS') == '0':
        return []                       # offline tests: no Windows session to look at
    s = load(state_file)
    if lines is None:
        lines = powershell_dialogs(s['dirs'], close)
    for l in lines:
        if l not in s['dialogs']:
            s['dialogs'].append(l)
    save(state_file, s)
    return lines


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('begin'); p.add_argument('--state', required=True); p.add_argument('--crash-dir', action='append', required=True)
    p.add_argument('--dir', action='append', default=[])
    p = sub.add_parser('expect'); p.add_argument('--state', required=True); p.add_argument('count', type=int)
    p = sub.add_parser('record'); p.add_argument('--state', required=True); p.add_argument('kind'); p.add_argument('text')
    p = sub.add_parser('dialogs'); p.add_argument('--state', required=True); p.add_argument('--close', action='store_true')
    p = sub.add_parser('collect'); p.add_argument('--state', required=True); p.add_argument('target')
    p = sub.add_parser('findings'); p.add_argument('--state', required=True)
    p = sub.add_parser('check'); p.add_argument('--state', required=True); p.add_argument('--no-dialogs', action='store_true')
    a = ap.parse_args(argv)
    if a.cmd == 'begin':
        begin(a.state, a.crash_dir, a.dir)
    elif a.cmd == 'expect':
        s = load(a.state); s['expected'] += a.count; save(a.state, s)
    elif a.cmd == 'record':
        s = load(a.state); s['manual'].append([a.kind, a.text]); save(a.state, s)
    elif a.cmd == 'dialogs':
        for l in record_dialogs(a.state, a.close):
            print('dialog: ' + l)
    elif a.cmd == 'collect':
        for m in collect(a.state, a.target):
            print('collected ' + m)
    elif a.cmd == 'findings':
        for f in findings(a.state):
            print('%s %s' % (f['kind'], f['text']))
    elif a.cmd == 'check':
        if not a.no_dialogs:
            try:
                record_dialogs(a.state, True)
            except (OSError, subprocess.SubprocessError) as exc:
                print('crashwatch: dialog scan unavailable: %s' % exc, file=sys.stderr)
        ok, lines = verdict(a.state)
        for l in lines:
            print(l)
        return 0 if ok else 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
