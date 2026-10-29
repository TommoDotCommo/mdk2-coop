#!/usr/bin/env python3
"""Run campaign bot segments in parallel: each segment is a fresh game started at a checkpoint and
played by the bot to the next checkpoint, on its own pair of game copies from the resource hook (the
same protocol as qa_all.py: `hook acquire N ttl` prints the copies' environment as JSON).

    campaign_run.py --level 2 --segments 3-5 5-6 6-7 [--shards 4] [--players 2]

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


def segment(level, seg, players, owner, hook, out, god):
    start, end = (int(v) for v in seg.split('-'))
    model = json.loads((ROOT / f'local/campaign/level{level}.json').read_text())
    goal = model['checkpoints'][str(end)]['pos']
    folder = out / f'{level}-{seg}'
    folder.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, MDK2_QA_OWNER=owner, LAB_OWNER=owner)
    r = subprocess.run([sys.executable, hook, 'acquire', str(players), '5400'], env=env, capture_output=True, text=True)
    if r.returncode:
        return {'segment': seg, 'result': 'BUSY', 'detail': r.stderr.strip()[-300:]}
    env.update(json.loads(r.stdout))
    env.update(MDK2_N=str(players), MDK2_BACKGROUND='1', MDK2_START=f'{level} {start}')
    dirs = [env[f'MDK2_DIR{i}'] for i in range(1, players + 1)]
    wsl = [subprocess.run(['wslpath', '-u', d], capture_output=True, text=True).stdout.strip() for d in dirs]
    t0 = time.monotonic()
    try:
        boot = f'source "{ROOT}/tools/mdk.sh" && "{ROOT}/tools/install.sh" >/dev/null && mdk_boot >/dev/null'
        b = subprocess.run(['bash', '-c', boot], env=env, capture_output=True, text=True, timeout=600)
        if b.returncode:
            return {'segment': seg, 'result': 'BOOT', 'detail': b.stderr[-300:]}
        setup = 'coop.save_interval=999999' + (' God(1)' if god else '')
        subprocess.run([sys.executable, '-c', 'import sys; sys.path.insert(0, sys.argv[1]); import campaign_bot as c\n'
                        'for i, d in enumerate(sys.argv[3:], 1): c.Game(d, i).lua(sys.argv[2])',
                        str(ROOT / 'tools'), setup, *wsl], check=True)
        with open(folder / 'bot.log', 'w') as log:
            cmd = 'advance' if players == 1 else 'squad'
            args = [sys.executable, '-u', str(ROOT / 'tools/campaign_bot.py'), cmd, '--dirs', *wsl,
                    '--level', str(ROOT / f'local/campaign/level{level}.json'), '--to', *map(str, goal)]
            if cmd == 'squad':
                args += ['--evidence', str(folder)]
            else:
                args += ['--scratch', str(folder / 'nav.json')]
            run = subprocess.run(args, stdout=log, stderr=subprocess.STDOUT, timeout=3600)
        tail = (folder / 'bot.log').read_text().strip().splitlines()[-1:]
        result = {'segment': seg, 'result': 'PASS' if run.returncode == 0 else 'FAIL',
                  'seconds': round(time.monotonic() - t0), 'last': tail[0] if tail else ''}
    except subprocess.TimeoutExpired as e:
        result = {'segment': seg, 'result': 'TIMEOUT', 'seconds': round(time.monotonic() - t0), 'detail': str(e)[:200]}
    finally:
        for i, d in enumerate(wsl, 1):
            log = Path(d) / 'coop' / 'bridge.log'
            if log.exists():
                (folder / f'bridge_{i}.log').write_text(log.read_text(encoding='latin-1'), encoding='latin-1')
        subprocess.run(['bash', '-c', f'source "{ROOT}/tools/mdk.sh" && mdk_kill'], env=env, capture_output=True)
        subprocess.run([sys.executable, hook, 'release'], env=env, capture_output=True)
    (folder / 'results.json').write_text(json.dumps(result, indent=1))
    return result


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--level', type=int, required=True)
    ap.add_argument('--segments', nargs='+', required=True, help='start-end checkpoint pairs')
    ap.add_argument('--shards', type=int, default=2)
    ap.add_argument('--players', type=int, default=2)
    ap.add_argument('--resources')
    ap.add_argument('--no-god', action='store_true', help='play without invulnerability')
    a = ap.parse_args(argv)
    hook = hook_path(a.resources)
    base = os.environ.get('LAB_OWNER') or os.environ.get('MDK2_QA_OWNER')
    if not base:
        raise SystemExit('set LAB_OWNER (a unique identity for this run)')
    out = ROOT / 'local/campaign/runs' / time.strftime('%Y%m%d-%H%M%S')
    out.mkdir(parents=True, exist_ok=True)
    with concurrent.futures.ThreadPoolExecutor(a.shards) as pool:
        jobs = {pool.submit(segment, a.level, seg, a.players, f'{base}-seg{i}', hook, out, not a.no_god): seg
                for i, seg in enumerate(a.segments)}
        results = []
        for job in concurrent.futures.as_completed(jobs):
            r = job.result()
            results.append(r)
            print(f"{r['result']:<7} level {a.level} segment {r['segment']:<6} {r.get('seconds', '-')}s  {r.get('last', r.get('detail', ''))}", flush=True)
    (out / 'summary.json').write_text(json.dumps(results, indent=1))
    passed = sum(r['result'] == 'PASS' for r in results)
    print(f'RESULT: {passed}/{len(results)} segments passed; evidence {out}')
    return 0 if passed == len(results) else 1


if __name__ == '__main__':
    sys.exit(main())
