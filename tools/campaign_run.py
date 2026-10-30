#!/usr/bin/env python3
"""Run campaign bot segments in parallel: each segment is a fresh game started at a checkpoint and
played by the bot to the next checkpoint, on its own pair of game copies from the resource hook (the
same protocol as qa_all.py: `hook acquire N ttl` prints the copies' environment as JSON).

    campaign_run.py --level 2 --segments 3-5 5-6 6-7 [--shards 4] [--players 2]
    campaign_run.py --segments 1:1-2 2:3-4 4:1-2 ...       segments of several levels (level:start-end)
    campaign_run.py --campaign [--from 1] [--to 9]          every launcher checkpoint to the next
    campaign_run.py --level 2 --segments 5-6 --no-god --repeat 3 --timescale 1   co-op play without God mode
    campaign_run.py --level 2 --segments 5-6 --players 3 --roles               split objectives, regroup
    campaign_run.py --level 2 --segments 5-6 --mutant desync|crash            must be reported (a finding, CRASH)
    campaign_run.py --level 2 --segments 5-6 --no-god --mutant down|down-stay  revived / not revived (FAIL)

A segment "A-B" starts at checkpoint A and must reach checkpoint B's position (from the mined model,
local/campaign/levelN.json). Results and evidence go under local/campaign/runs/<time>/: one folder per
segment with the bot's log, the game logs and results.json; a summary at the end.

Squads compare the worlds after every step (results.json "steps", findings apart from the known
differences in tools/campaign_coop.py). A crash fails the segment as CRASH, with the dump: a new
dump%04i.dmp of one of the segment's copies, the Overhaul build's error dialog ("Jinkies!", closed),
a coop/logs/*.crash marker, or a game process gone (tools/crashwatch.py, the guard tools/mdk.sh uses).
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


def crash_lines(env):
    r = subprocess.run([sys.executable, '-I', str(ROOT / 'tools/crashwatch.py'), 'check', '--state',
                        env['MDK2_CRASH_STATE']], env=env, capture_output=True, text=True)
    return [l for l in (r.stdout + r.stderr).splitlines() if l.startswith(('CRASH', 'expected', 'note'))]


def gone_games(env):
    """Pids of this segment's games that are no longer running."""
    try:
        pids = Path(subprocess.run(['wslpath', '-u', env['MDK2_PIDS']], capture_output=True, text=True).stdout.strip()
                    if ':' in env['MDK2_PIDS'] else env['MDK2_PIDS']).read_text().split()
    except OSError:
        return []
    out = []
    for p in pids:
        r = subprocess.run(['tasklist.exe', '/FI', f'PID eq {p}'], capture_output=True, text=True)
        if 'mdk2hd' not in r.stdout.lower():
            out.append(p)
    return out


def copy_info(folder):
    import hashlib
    exe = Path(folder) / 'mdk2hd.exe'
    ini = Path(folder) / 'coop' / 'coop.ini'
    text = ini.read_text(encoding='latin-1') if ini.exists() else ''
    save = next((l.split('=', 1)[1].strip() for l in text.splitlines() if l.startswith('save_dir=')), None)
    return {'exe_sha256': hashlib.sha256(exe.read_bytes()).hexdigest()[:8] if exe.exists() else None,
            'save_dir': save, 'timescale': next((l.split('=', 1)[1].strip() for l in text.splitlines()
                                                 if l.startswith('timescale=')), None)}


def segment(level, seg, players, owner, hook, out, god, opts=None):
    opts = opts or {}
    if ':' in seg:
        level, seg = int(seg.split(':')[0]), seg.split(':')[1]
    start, end = (int(v) for v in seg.split('-'))
    model = json.loads((ROOT / f'local/campaign/level{level}.json').read_text())
    goal = model['checkpoints'][str(end)]['pos']
    folder = out / f'{level}-{seg}'
    seg = f'{level}:{seg}'
    folder.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, MDK2_QA_OWNER=owner, LAB_OWNER=owner)
    # the lab queues fairly and answers BUSY at once while another lease is ahead: try again for a while
    deadline = time.monotonic() + 1800
    while True:
        r = subprocess.run([sys.executable, hook, 'acquire', str(players), '5400'], env=env, capture_output=True, text=True)
        if r.returncode == 0:
            break
        if time.monotonic() > deadline or 'BUSY' not in (r.stderr + r.stdout):
            return {'segment': seg, 'result': 'BUSY', 'detail': r.stderr.strip()[-300:]}
        time.sleep(20)
    env.update(json.loads(r.stdout))
    env.update(MDK2_N=str(players), MDK2_BACKGROUND='1', MDK2_START=f'{level} {start}')
    if opts.get('timescale'):
        env['MDK2_TIMESCALE'] = str(opts['timescale'])
    dirs = [env[f'MDK2_DIR{i}'] for i in range(1, players + 1)]
    wsl = [subprocess.run(['wslpath', '-u', d], capture_output=True, text=True).stdout.strip() for d in dirs]
    t0 = time.monotonic()
    result = {'segment': seg}
    try:
        # the crash guard of tools/mdk.sh, kept for the whole segment: a snapshot of the crash folders now
        begin = 'source "$1/tools/mdk.sh" >/dev/null && export MDK2_CRASH_OWNER=campaign_run && echo "${MDK2_CRASH_STATE:-}"'
        cs = subprocess.run(['bash', '-c', begin, 'bash', str(ROOT)], env=env, capture_output=True, text=True)
        if cs.stdout.strip():
            env['MDK2_CRASH_STATE'] = cs.stdout.strip().splitlines()[-1]
        boot = f'source "{ROOT}/tools/mdk.sh" && "{ROOT}/tools/install.sh" >/dev/null && mdk_boot >/dev/null'
        b = subprocess.run(['bash', '-c', boot], env=env, capture_output=True, text=True, timeout=600)
        if b.returncode:
            result.update(result='BOOT', detail=b.stderr[-300:])
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
            result.update(result='BOOT', detail=(w.stderr or w.stdout)[-300:])
            return result
        # which build each copy runs and where its saves go (each copy its own: [test] save_dir)
        result['copies'] = [copy_info(d) for d in wsl]
        setup = 'coop.save_interval=999999' + (' God(1)' if god else '')
        subprocess.run([sys.executable, '-c', 'import sys; sys.path.insert(0, sys.argv[1]); import campaign_bot as c\n'
                        'for i, d in enumerate(sys.argv[3:], 1): c.Game(d, i).lua(sys.argv[2])',
                        str(ROOT / 'tools'), setup, *wsl], check=True)
        with open(folder / 'bot.log', 'w') as log:
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
                routes = ROOT / 'local/routes'
                known = routes / f'seg_{level}_{start}_{end}.route'
                if opts.get('explore'):
                    pass                    # the bot plays on its own (a recording that fails is not the test)
                elif known.exists():
                    args += ['--route', str(known)]
                else:
                    args += ['--record', f'seg_{level}_{start}_{end}']
            else:
                args += ['--scratch', str(folder / 'nav.json')]
            for k in ('no_god', 'roles'):
                if opts.get(k) and cmd == 'squad':
                    args.append('--' + k.replace('_', '-'))
            if opts.get('tactic') and cmd == 'squad':
                args += ['--tactic', opts['tactic']]
            if opts.get('mutant') in ('desync', 'down', 'down-stay') and cmd == 'squad':
                args += ['--mutant', opts['mutant']]
            run = watched(args, log, env, wsl, opts, timeout=1500)
            result['gone'] = run.gone
        name = f'seg_{level}_{start}_{end}.route'
        made = Path(wsl[0]) / 'coop' / name
        if run.returncode == 0 and made.exists() and not (ROOT / 'local/routes' / name).exists():
            (ROOT / 'local/routes').mkdir(parents=True, exist_ok=True)
            (ROOT / 'local/routes' / name).write_bytes(made.read_bytes())
        tail = (folder / 'bot.log').read_text().strip().splitlines()[-1:]
        result.update({'result': 'PASS' if run.returncode == 0 else 'FAIL',
                       'seconds': round(time.monotonic() - t0), 'last': tail[0] if tail else ''})
        if "'world_ok': False" in result['last']:
            result['coop_finding'] = 'worlds differ (see statecheck_end_*.txt)'
        squad_json = sorted(folder.glob('*/squad.json'))
        if squad_json:
            sq = json.loads(squad_json[-1].read_text())
            steps = json.loads((squad_json[-1].parent / 'steps.json').read_text())
            result['steps'] = [{'step': s['step'], 't': s.get('t'), 'leader': s.get('leader'), 'findings': s['findings'],
                                'known': len(s['known']), 'warn': len(s['warn']), 'summary': s['summary']} for s in steps]
            result['findings'] = sq.get('findings', [])
            for k in ('lives', 'care', 'regroup_s', 'mutant'):
                if k in sq:
                    result[k] = sq[k]
            result['roles'] = {k: v for k, v in sq.items() if k.startswith('role_')}
            if (opts.get('roles') or players > 2) and 'regroup_s' in sq and sq['regroup_s'] is None:
                # the squad did not meet at the portal within 60 s: a co-op finding, the segment fails
                result['findings'] = result['findings'] + ['regroup: the players did not all reach the goal in 60 s']
                if result['result'] == 'PASS':
                    result['result'] = 'FAIL'
                    result['coop_finding'] = 'the squad did not regroup at the goal'
                result['last'] = result['findings'][-1]
            m = sq.get('mutant') or {}
            if opts.get('mutant') in ('down', 'down-stay') and not m.get('ok'):
                # the downed player was not revived (or died): a finding, the segment fails
                result['findings'] = result['findings'] + [
                    f"revive: player {m.get('downed')} down seen {m.get('down_seen')}, revived {m.get('revived')}, "
                    f"died {m.get('died')}"]
                if result['result'] == 'PASS':
                    result['result'] = 'FAIL'
                    result['coop_finding'] = 'a downed player was not revived'
                result['last'] = result['findings'][-1]
    except subprocess.TimeoutExpired as e:
        result.update({'result': 'TIMEOUT', 'seconds': round(time.monotonic() - t0), 'detail': str(e)[:200]})
    finally:
        if env.get('MDK2_CRASH_STATE'):
            subprocess.run([sys.executable, '-I', str(ROOT / 'tools/crashwatch.py'), 'dialogs', '--state',
                            env['MDK2_CRASH_STATE'], '--close'], env=env, capture_output=True, timeout=90)
        for i, d in enumerate(wsl, 1):
            log = Path(d) / 'coop' / 'bridge.log'
            if log.exists():
                (folder / f'bridge_{i}.log').write_text(log.read_text(encoding='latin-1'), encoding='latin-1')
        try:
            import campaign_coop
            result['bridge_lives'] = campaign_coop.bridge_lives(
                [p.read_text(encoding='latin-1') for p in sorted(folder.glob('bridge_*.log'))])
        except Exception as e:
            result['bridge_lives'] = {'error': str(e)[:100]}
        subprocess.run(['bash', '-c', f'source "{ROOT}/tools/mdk.sh" && mdk_kill'], env=env, capture_output=True)
        if env.get('MDK2_CRASH_STATE'):
            # a crash fails the segment whatever the bot reported: the dump, dialog or marker is the evidence
            lines = crash_lines(env)
            if result.get('gone'):
                lines += [f'CRASH exit: game process {p} is gone' for p in result['gone']]
            crashed = [l for l in lines if l.startswith('CRASH')]
            if crashed:
                result['crash'] = crashed
                result['result'] = 'CRASH'
                moved = subprocess.run([sys.executable, '-I', str(ROOT / 'tools/crashwatch.py'), 'collect', '--state',
                                        env['MDK2_CRASH_STATE'], str(folder / 'dumps')], env=env,
                                       capture_output=True, text=True)
                result['dumps_kept'] = str(folder / 'dumps')
            elif lines:
                result['crash_notes'] = lines
            Path(env['MDK2_CRASH_STATE']).unlink(missing_ok=True)
        subprocess.run([sys.executable, hook, 'release'], env=env, capture_output=True)
    (folder / 'results.json').write_text(json.dumps(result, indent=1))
    return result


def watched(args, log, env, wsl, opts, timeout):
    """Run the bot while watching its games: every 10 s the Overhaul build's error dialogs are closed and
    recorded and the game processes are checked; a game gone ends the run (the squad cannot finish).
    The crash mutant crashes the last game on purpose (coop_crash, a background test build only)."""
    proc = subprocess.Popen(args, stdout=log, stderr=subprocess.STDOUT)
    t0 = time.monotonic()
    proc.gone, crashed = [], 0
    while proc.poll() is None:
        time.sleep(10)
        if time.monotonic() - t0 > timeout:
            proc.kill()
            proc.wait()
            raise subprocess.TimeoutExpired(args, timeout)
        if opts.get('mutant') == 'crash' and crashed < 3 and time.monotonic() - t0 > opts.get('crash_after', 40):
            crashed += 1            # again on the next round if the bot's own command took its place
            target = Path(wsl[-1]) / 'coop' / 'command.lua'
            target.write_text('coop_crash()\n')
            log.write(f'MUTANT crash: coop_crash() sent to game {len(wsl)}\n')
            log.flush()
        if env.get('MDK2_CRASH_STATE'):
            subprocess.run([sys.executable, '-I', str(ROOT / 'tools/crashwatch.py'), 'dialogs', '--state',
                            env['MDK2_CRASH_STATE'], '--close'], env=env, capture_output=True, timeout=90)
        gone = gone_games(env)
        if gone:
            proc.gone = gone
            log.write(f'CRASH: game process(es) {gone} gone; ending the run\n')
            log.flush()
            proc.kill()
            proc.wait()
            break
    return proc


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
    ap.add_argument('--no-god', action='store_true', help='play without invulnerability: heroes revive each other')
    ap.add_argument('--tactic', choices=('fight', 'run'), default=None, help='no-God play style (see campaign_bot.py squad)')
    ap.add_argument('--roles', action='store_true', help="split a step's objectives between the players")
    ap.add_argument('--timescale', type=float, help='game clock factor (MDK2_TIMESCALE); 1 to confirm findings')
    ap.add_argument('--explore', action='store_true', help='the bot plays on its own, ignoring recorded routes')
    ap.add_argument('--repeat', type=int, default=1, help='run each segment this many times')
    ap.add_argument('--mutant', choices=['desync', 'crash', 'down', 'down-stay'],
                    help='desync: an enemy deleted in one client only; crash: a game crashed (both must be reported); '
                         'down: a client is put down and must be revived (needs --no-god); down-stay: nobody revives it, '
                         'the segment must fail')
    a = ap.parse_args(argv)
    if a.mutant in ('down', 'down-stay') and not a.no_god:
        ap.error('--mutant down needs --no-god (God mode ignores the hit)')
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
    with concurrent.futures.ThreadPoolExecutor(a.shards) as pool:
        opts = {'no_god': a.no_god, 'roles': a.roles, 'timescale': a.timescale, 'mutant': a.mutant, 'explore': a.explore, 'tactic': a.tactic}
        runs = [(seg, k) for seg in a.segments for k in range(a.repeat)]
        jobs = {pool.submit(segment, a.level, seg, a.players, f'{base}-seg{i}', hook, out / f'run{k + 1}'
                            if a.repeat > 1 else out, not a.no_god, opts): seg
                for i, (seg, k) in enumerate(runs)}
        results = []
        for job in concurrent.futures.as_completed(jobs):
            r = job.result()
            results.append(r)
            print(f"{r['result']:<7} segment {r['segment']:<8} {r.get('seconds', '-')}s  {r.get('last', r.get('detail', ''))[:160]}", flush=True)
            for f in r.get('findings', []) + r.get('crash', []):
                print(f'        finding: {f}', flush=True)
    (out / 'summary.json').write_text(json.dumps(results, indent=1))
    passed = sum(r['result'] == 'PASS' for r in results)
    print(f'RESULT: {passed}/{len(results)} segments passed; evidence {out}')
    return 0 if passed == len(results) else 1


if __name__ == '__main__':
    sys.exit(main())
