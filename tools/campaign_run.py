#!/usr/bin/env python3
"""Run campaign bot segments in parallel: each segment is a fresh game started at a checkpoint and
played by the bot to the next checkpoint, on its own pair of game copies from the resource hook (the
same protocol as qa_all.py: `hook acquire N ttl` prints the copies' environment as JSON).

    campaign_run.py --level 2 --segments 3-5 5-6 6-7 [--shards 4] [--players 2]
    campaign_run.py --segments 1:1-2 2:3-4 4:1-2 ...       segments of several levels (level:start-end)
    campaign_run.py --campaign [--from 1] [--to 9]          every launcher checkpoint to the next

A segment "A-B" starts at checkpoint A and must reach checkpoint B's position (from the mined model,
local/campaign/levelN.json). Results and evidence go under local/campaign/runs/<time>/: one folder per
segment with the bot's log, the game logs and results.json; a summary at the end.
"""
import argparse
import concurrent.futures
import json
import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def hook_path(arg):
    hook = arg or os.environ.get('MDK2_QA_RESOURCES')
    if not hook and (ROOT / 'local/qa_resources.py').is_file():
        hook = str(ROOT / 'local/qa_resources.py')
    if not hook:
        raise SystemExit('no resource hook: set MDK2_QA_RESOURCES or pass --resources (see docs/TESTING.md)')
    return hook


CRASH = ROOT / 'tools/crashwatch.py'
# the non-Steam Overhaul build's mdk2hd.exe (sha256 prefix), the build players use
OVERHAUL_SHA = '6b8b2400'


def build_of(wsl_dir):
    import hashlib
    exe = Path(wsl_dir) / 'mdk2hd.exe'
    if not exe.exists():
        return 'unknown'
    sha = hashlib.sha256(exe.read_bytes()).hexdigest()
    return 'overhaul' if sha.startswith(OVERHAUL_SHA) else f'other:{sha[:8]}'


def segment(level, seg, players, owner, hook, out, god, limit=1500, need_build=None, routes_on=True, chain=()):
    if ':' in seg:
        level, seg = int(seg.split(':')[0]), seg.split(':')[1]
    start, end = (int(v) for v in seg.split('-'))
    folder = out / f'{level}-{seg}'
    seg = f'{level}:{seg}'
    folder.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, MDK2_QA_OWNER=owner, LAB_OWNER=owner)
    # the lab queues fairly and answers BUSY at once while another lease is ahead: try again for a while
    deadline = time.monotonic() + 1800
    while True:
        r = subprocess.run([sys.executable, hook, 'acquire', str(players), str(14400 if chain else 5400)], env=env,
                           capture_output=True, text=True)
        if r.returncode == 0:
            break
        # exit 3: busy, or too little free RAM for another game; both pass
        if time.monotonic() > deadline or (r.returncode != 3 and 'BUSY' not in (r.stderr + r.stdout)):
            return {'segment': seg, 'result': 'BUSY', 'detail': r.stderr.strip()[-300:]}
        time.sleep(20)
    env.update(json.loads(r.stdout))
    env.update(MDK2_N=str(players), MDK2_BACKGROUND='1', MDK2_START=f'{level} {start}')
    dirs = [env[f'MDK2_DIR{i}'] for i in range(1, players + 1)]
    wsl = [subprocess.run(['wslpath', '-u', d], capture_output=True, text=True).stdout.strip() for d in dirs]
    t0 = time.monotonic()
    result = {'segment': seg, 'result': 'BOOT'}
    crash_state = None
    watching = None
    try:
        build = build_of(wsl[0])
        if need_build and build != need_build:
            result = {'segment': seg, 'result': 'BUILD', 'detail': f'copies run {build}, not {need_build}'}
            return result
        # crashes fail a segment: a new dump%04i.dmp, a coop/logs/*.crash marker or the error dialog
        # (tools/crashwatch.py, through mdk.sh's guard so the copies' own crash folders are watched)
        c = subprocess.run(['bash', '-c', f'source "{ROOT}/tools/mdk.sh" && mdk_crash_begin && echo "$MDK2_CRASH_STATE"'],
                           env=env, capture_output=True, text=True)
        crash_state = (c.stdout.strip().splitlines() or [''])[-1] or None
        if crash_state:
            import threading
            watching = threading.Event()

            def dialogs():
                # an error dialog stalls the game: close and record it as soon as it shows
                while not watching.wait(20):
                    subprocess.run([sys.executable, '-I', str(CRASH), 'dialogs', '--state', crash_state, '--close'],
                                   capture_output=True)
            threading.Thread(target=dialogs, daemon=True).start()
        boot = f'source "{ROOT}/tools/mdk.sh" && "{ROOT}/tools/install.sh" >/dev/null && mdk_boot >/dev/null'
        b = subprocess.run(['bash', '-c', boot], env=env, capture_output=True, text=True, timeout=600)
        if b.returncode:
            result = {'segment': seg, 'result': 'BOOT', 'detail': b.stderr[-300:]}
            return result
        # the host loads the start checkpoint by itself a while after the title screen: wait for it
        wait = ('import sys, time; sys.path.insert(0, sys.argv[1]); import campaign_bot as c\n'
                'g = c.Game(sys.argv[2], 1); t0 = time.monotonic()\n'
                'while time.monotonic() - t0 < 240:\n'
                '    try:\n'
                '        if g.note("lvl", "coop_note(\'lvl\', tostring(coop.level))").strip() == sys.argv[3]: break\n'
                '    except Exception: pass\n'
                '    time.sleep(3)\n'
                'else: sys.exit("the start checkpoint did not load")\n'
                'time.sleep(5)')
        w = subprocess.run([sys.executable, '-c', wait, str(ROOT / 'tools'), wsl[0], str(level)],
                           capture_output=True, text=True)
        if w.returncode:
            result = {'segment': seg, 'result': 'BOOT', 'detail': (w.stderr or w.stdout)[-300:]}
            return result
        setup = 'coop.save_interval=999999' + (' God(1)' if god else '')
        subprocess.run([sys.executable, '-c', 'import sys; sys.path.insert(0, sys.argv[1]); import campaign_bot as c\n'
                        'for i, d in enumerate(sys.argv[3:], 1): c.Game(d, i).lua(sys.argv[2])',
                        str(ROOT / 'tools'), setup, *wsl], check=True)
        # chained: the later segments go on in the same games, each from where the one before ended
        legs = []
        for leg, (a, b) in enumerate(zip([start, end, *chain], [end, *chain])):
            logname = 'bot.log' if not leg else f'bot_{a}_{b}.log'
            run, args, lines = play(level, a, b, players, wsl, folder, routes_on, logname, limit)
            legs.append({'segment': f'{level}:{a}-{b}', 'result': 'PASS' if run.returncode == 0 else 'FAIL',
                         'seconds': round(time.monotonic() - t0), 'last': (lines or [''])[-1][:200]})
            if run.returncode:
                break
        tail = lines[-1:]
        result = {'segment': seg if not chain else f'{level}:{start}-{chain[-1]}',
                  'result': 'PASS' if run.returncode == 0 and len(legs) == len(chain) + 1 else 'FAIL',
                  'mode': ('chain ' if chain else '') + ('recorded' if '--route' in args else 'autonomous'),
                  'seconds': round(time.monotonic() - t0), 'last': tail[0] if tail else '', 'build': build}
        if chain:
            result['legs'] = legs
        stall = [l for l in lines if ' lost at point ' in l or 'ARRIVED' in l or 'gave up' in l.lower()]
        if stall and run.returncode:
            result['stall'] = (f'{legs[-1]["segment"]}: ' if chain else '') + stall[-1].strip()[:240]
        diffs = [l.strip() for l in lines if l.strip().startswith('worlds at ')]
        if diffs:
            result['worlds'] = diffs[-1][:240]
        if "'world_ok': False" in result['last']:
            result['coop_finding'] = 'worlds differ (see statecheck_end_*.txt)'
    except subprocess.TimeoutExpired as e:
        result = {'segment': seg, 'result': 'TIMEOUT', 'seconds': round(time.monotonic() - t0), 'detail': str(e)[:200]}
    finally:
        if watching:
            watching.set()
        for i, d in enumerate(wsl, 1):
            log = Path(d) / 'coop' / 'bridge.log'
            if log.exists():
                (folder / f'bridge_{i}.log').write_text(log.read_text(encoding='latin-1'), encoding='latin-1')
        subprocess.run(['bash', '-c', f'source "{ROOT}/tools/mdk.sh" && mdk_kill'], env=env, capture_output=True)
        if crash_state:
            c = subprocess.run([sys.executable, '-I', str(CRASH), 'check', '--state', crash_state],
                               capture_output=True, text=True)
            crashes = [l for l in c.stdout.splitlines() if l.startswith('CRASH')]
            result['crash'] = crashes
            if crashes:
                result['result'] = 'CRASH'
            Path(crash_state).unlink(missing_ok=True)
        else:
            result['crash'] = 'unwatched'
        subprocess.run([sys.executable, hook, 'release'], env=env, capture_output=True)
        # each copy keeps its own saves ([test] save_dir): checkpoint loads touch no player's saves
        ini = Path(wsl[0]) / 'coop' / 'coop.ini'
        result['save_dir'] = next((l.split('=', 1)[1].strip() for l in
                                   (ini.read_text(errors='replace').splitlines() if ini.exists() else [])
                                   if l.startswith('save_dir=')), 'none')
        result.update(stall_of(folder / 'bot.log'))
        (folder / 'results.json').write_text(json.dumps(result, indent=1))
    return result


def play(level, start, end, players, wsl, folder, routes_on, logname, limit=1500):
    """One segment played by the bot in running games: (process, its arguments, its log lines)."""
    model = json.loads((ROOT / f'local/campaign/level{level}.json').read_text())
    goal = model['checkpoints'][str(end)]['pos']
    with open(folder / logname, 'w') as log:
        cmd = 'advance' if players == 1 else 'squad'
        args = [sys.executable, '-u', str(ROOT / 'tools/campaign_bot.py'), cmd, '--dirs', *wsl,
                '--level', str(ROOT / f'local/campaign/level{level}.json'), '--to', *map(str, goal)]
        # Max's levels: from the jetpack's checkpoint on, the bot refuels and flies
        if (level, start) >= (2, 5) and level in (2, 5, 8, 11) and not (level == 5 and start < 9) \
                and not (level == 8 and start < 5):
            args.append('--fly')
        if level in (1, 4, 7, 10):
            args.append('--chute')      # Kurt
        if cmd == 'squad':
            args += ['--evidence', str(folder)]
            # a segment solved before is followed from its recording; otherwise the bot explores and
            # its play is recorded, kept as the segment's route if it passes
            known = ROOT / 'local/routes' / f'seg_{level}_{start}_{end}.route'
            if known.exists() and routes_on:
                args += ['--route', str(known)]
            elif not known.exists():
                args += ['--record', f'seg_{level}_{start}_{end}']
        else:
            args += ['--scratch', str(folder / 'nav.json')]
        if os.environ.get('CAMPAIGN_IDLE'):
            # a start checkpoint left to a person or a script: the copies' folders are listed, and the
            # games stay up until <segment folder>/stop exists (or the limit passes)
            (folder / 'dirs.txt').write_text('\n'.join(wsl) + '\n')
            log.write('idle: games left to a driver\n')
            log.flush()
            t_idle = time.monotonic()
            while not (folder / 'stop').exists() and time.monotonic() - t_idle < limit:
                time.sleep(5)
            run = subprocess.CompletedProcess(args, 1)
        else:
            run = subprocess.run(args, stdout=log, stderr=subprocess.STDOUT, timeout=limit)
    name = f'seg_{level}_{start}_{end}.route'
    made = Path(wsl[0]) / 'coop' / name
    if run.returncode == 0 and made.exists() and not (ROOT / 'local/routes' / name).exists():
        (ROOT / 'local/routes').mkdir(parents=True, exist_ok=True)
        (ROOT / 'local/routes' / name).write_bytes(made.read_bytes())
    return run, args, (folder / logname).read_text().strip().splitlines()


def stall_of(log):
    """Where the leader got to and why it stopped, from the bot's log: its last position and the last
    thing it said before giving up."""
    import re
    if not log.exists():
        return {}
    lines = [l for l in log.read_text(errors='replace').splitlines() if l.startswith('[1]')]
    pos = None
    for l in lines:
        m = re.search(r'now \[([-\d.]+), ([-\d.]+), ([-\d.]+)\]', l)
        if m:
            pos = [float(v) for v in m.groups()]
    why = next((l[3:].strip() for l in reversed(lines)
                if not re.search(r'stalled toward|toward \[|pushed toward|aim |worlds at', l)), '')
    return {'stall_pos': pos, 'stall_why': why[:160]}


def report(out, results):
    """report.md: one row per segment."""
    rows = ['| segment | mode | result | seconds | crash | world | stall at | last word |', '|---|---|---|---|---|---|---|---|']
    def order(r):
        l, se = r['segment'].split(':')
        return (int(l), *map(int, se.split('-')))
    for r in sorted(results, key=order):
        world = 'DIFF' if r.get('coop_finding') else ('match' if r.get('result') in ('PASS', 'FAIL') else '?')
        crash = r.get('crash') or 'none'
        crash = '; '.join(crash) if isinstance(crash, list) else crash
        rows.append(f"| {r['segment']} | {r.get('mode', '-')} | {r['result']} | {r.get('seconds', '-')} | {crash} | "
                    f"{world} | {r.get('stall_pos') or ''} | {(r.get('stall_why') or r.get('detail') or '').replace('|', '/')} |")
    passed = sum(r['result'] == 'PASS' for r in results)
    (out / 'report.md').write_text(f'# Campaign bot run {out.name}\n\n{passed}/{len(results)} segments passed.\n\n'
                                   + '\n'.join(rows) + '\n')


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--level', type=int, default=0)
    ap.add_argument('--segments', nargs='+', default=[], help='start-end checkpoint pairs, or level:start-end')
    ap.add_argument('--campaign', action='store_true', help='every launcher checkpoint to the next one')
    ap.add_argument('--from', dest='first', type=int, default=1)
    ap.add_argument('--to', dest='last', type=int, default=12)
    ap.add_argument('--shards', type=int, default=2)
    ap.add_argument('--players', type=int, default=2)
    ap.add_argument('--resources')
    ap.add_argument('--no-god', action='store_true', help='play without invulnerability')
    ap.add_argument('--no-routes', action='store_true', help='the bot on its own even where a recording exists')
    ap.add_argument('--chain', action='store_true', help='consecutive segments of one level in one game pair, in order')
    ap.add_argument('--timeout', type=int, default=1500, help="seconds for a segment's bot before it is stopped")
    ap.add_argument('--build', choices=('overhaul',), help='refuse copies that run another build')
    a = ap.parse_args(argv)
    if a.campaign:
        # tools/checkpoints.txt: the launcher's checkpoints in play order
        order = [tuple(int(v) for v in t.split('-')) for t in (ROOT / 'tools/checkpoints.txt').read_text().split()]
        a.segments = [f'{l}:{c}-{n}' for (l, c), (l2, n) in zip(order, order[1:])
                      if l == l2 and a.first <= l <= a.last]
    if not a.segments or (not a.level and any(':' not in s for s in a.segments)):
        ap.error('give --segments with --level, level:start-end segments, or --campaign')
    hook = hook_path(a.resources)
    base = os.environ.get('LAB_OWNER') or os.environ.get('MDK2_QA_OWNER')
    if not base:
        raise SystemExit('set LAB_OWNER (a unique identity for this run)')
    out = ROOT / 'local/campaign/runs' / time.strftime('%Y%m%d-%H%M%S')
    out.mkdir(parents=True, exist_ok=True)
    if a.chain:
        # one run: boot at the first segment's start and play every segment in turn in the same games
        legs = [s if ':' in s else f'{a.level}:{s}' for s in a.segments]
        ends = [int(s.split('-')[1]) for s in legs]
        a.segments, chain = [legs[0]], ends[1:]
    else:
        chain = []
    with concurrent.futures.ThreadPoolExecutor(a.shards) as pool:
        jobs = {pool.submit(segment, a.level, seg, a.players, f'{base}-seg{i}', hook, out, not a.no_god,
                            a.timeout, a.build, not a.no_routes, chain): seg
                for i, seg in enumerate(a.segments)}
        results = []
        for job in concurrent.futures.as_completed(jobs):
            r = job.result()
            results.append(r)
            print(f"{r['result']:<7} segment {r['segment']:<8} {r.get('seconds', '-')}s  {r.get('last', r.get('detail', ''))[:160]}", flush=True)
            (out / 'summary.json').write_text(json.dumps(results, indent=1))
            report(out, results)
    passed = sum(r['result'] == 'PASS' for r in results)
    print(f'RESULT: {passed}/{len(results)} segments passed; evidence {out}')
    return 0 if passed == len(results) else 1


if __name__ == '__main__':
    sys.exit(main())
