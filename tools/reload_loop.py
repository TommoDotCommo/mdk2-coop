#!/usr/bin/env python3
"""Reload one checkpoint again and again in one running game while the campaign bot walks, to catch a
crash that needs a reload with enemies thinking (the AI sight check fault, mdk2hd.exe+0x2a95).

    reload_loop.py --dir <game dir, WSL path> --pid N [--level 2 --cp 9] [--minutes 40]

Each round reloads the checkpoint (mdkNewGame), alternating between a living hero and a dead one (God off,
a lethal hit, then the game's own checkpoint reload or ours after a few seconds), lets the bot follow the
recorded route for a while, and checks that the process is still there. Stops at the first lost process;
prints one line per round. Run it through tools/reload_loop.sh, which owns the copy and the crash guard.
"""
import argparse
import random
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import campaign_bot as cb  # noqa: E402


def alive(pid):
    r = subprocess.run(['tasklist.exe', '/FI', f'PID eq {pid}'], capture_output=True, text=True)
    return 'mdk2hd' in r.stdout.lower()


def level_started(log, count):
    text = Path(log).read_text(encoding='latin-1') if Path(log).exists() else ''
    return text.count('started') >= count


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--dir', required=True)
    ap.add_argument('--pid', required=True)
    ap.add_argument('--level', type=int, default=2)
    ap.add_argument('--cp', type=int, default=9)
    ap.add_argument('--minutes', type=float, default=40)
    ap.add_argument('--route', help='recorded route the bot follows (default local/routes/seg_<level>_<cp>_<cp+1>.route)')
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--no-bot', action='store_true')
    a = ap.parse_args()
    rnd = random.Random(a.seed)
    game = cb.Game(a.dir, 1)
    route = a.route or str(HERE.parent / f'local/routes/seg_{a.level}_{a.cp}_{a.cp + 1}.route')
    model = str(HERE.parent / f'local/campaign/level{a.level}.json')
    end = time.monotonic() + a.minutes * 60
    n = 0
    game.lua('God(1)')
    while time.monotonic() < end:
        n += 1
        dead = n % 2 == 0
        t0 = time.monotonic()
        if dead:
            game.lua('God(0)')
            game.lua('mdkDealDamage(nil, mdkGetPlayerGob(), 9999, DAMAGE_BADGUY, -1)')
            time.sleep(rnd.uniform(1, 8))          # sometimes the game's own reload comes first
        if not alive(a.pid):
            print(f'round {n}: GAME GONE before the reload', flush=True)
            return 2
        game.lua(f'mdkNewGame({a.level},{a.cp})')
        if dead:
            game.lua('God(1)')
        time.sleep(rnd.uniform(4, 9))              # the level builds; enemies start thinking
        bot = None
        if not a.no_bot:
            bot = subprocess.Popen([sys.executable, '-u', str(HERE / 'campaign_bot.py'), 'follow', '--dirs', a.dir,
                                    '--route', route, '--level', model],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        stop = time.monotonic() + rnd.uniform(8, 25)
        while time.monotonic() < stop and alive(a.pid):
            time.sleep(1)
        if bot:
            bot.terminate()
            try:
                bot.wait(10)
            except subprocess.TimeoutExpired:
                bot.kill()
        up = alive(a.pid)
        print(f'round {n} {"dead" if dead else "alive"} {time.monotonic() - t0:.0f}s process {"up" if up else "GONE"}', flush=True)
        if not up:
            return 2
    print(f'{n} rounds, no crash', flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
