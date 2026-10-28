#!/usr/bin/env python3
"""Compare process state ledgers with measured enemy HP loss; never parse game logs."""
import argparse
import math
from pathlib import Path


def load(path):
    state = {'events': {}, 'players': {}, 'entities': {}, 'meter': None, 'me': None, 'role': None}
    for line in Path(path).read_text(encoding='latin-1').splitlines():
        f = line.split()
        if not f:
            continue
        if f[0] == 'me':
            state['me'] = int(f[1])
        elif f[0] == 'role':
            state['role'] = f[1]
        elif f[0] == 'combat':
            if len(f) != 6 or state['meter'] is not None:
                raise ValueError('invalid combat header')
            state['epoch'], state['size'], state['meter'], state['scroll'], state['page'] = map(int, f[1:])
        elif f[0] == 'combat_player':
            if len(f) != 6:
                raise ValueError('invalid player row')
            slot = int(f[1])
            values = (int(f[2]), int(f[3]), float(f[4]), float(f[5]))
            if slot in state['players'] or not 1 <= slot <= 16 or any(v < 0 or not math.isfinite(v) for v in values):
                raise ValueError('invalid or duplicate player')
            state['players'][slot] = values
        elif f[0] == 'combat_event':
            if len(f) != 12:
                raise ValueError('invalid event row')
            values = tuple(map(int, f[1:9])) + tuple(map(float, f[9:12]))
            epoch, serial, slot, target, kind, amount, weapon, flags, *pos = values
            key = (epoch, serial)
            if key in state['events'] or serial <= 0 or not 1 <= slot <= 16 or target <= 0:
                raise ValueError('invalid or duplicate event identity')
            if not 200 <= kind <= 259 or not 1 <= amount <= 32767 or not 0 <= weapon <= 4095 or flags & ~3:
                raise ValueError('invalid event values')
            if any(not math.isfinite(p) or abs(p) > 1000000 for p in pos):
                raise ValueError('invalid event position')
            state['events'][key] = values
        elif f[0] == 'ent' and len(f) >= 8 and f[2] != 'gone':
            state['entities'][' '.join(f[7:])] = (int(f[1], 16), int(f[3]))
    if state['meter'] not in (0, 1) or len(state['players']) != 16 or state['size'] != len(state['events']):
        raise ValueError('missing or inconsistent combat state')
    if state['me'] not in range(1, 17) or state['role'] not in ('host', 'client'):
        raise ValueError('missing process identity')
    if any(key[0] != state['epoch'] for key in state['events']):
        raise ValueError('stale ledger epoch')
    if not 0 <= state['size'] <= 512 or not 0 <= state['epoch'] <= 65535:
        raise ValueError('history overflow')
    return state


def verify(before, after, attacker=None, target=None, expected=None, tol=1, idle=False, reset=False, meter=None):
    if len(before) != len(after) or len(after) < 2:
        raise ValueError('need matching before/after snapshots for at least two processes')
    if len({s['me'] for s in after}) != len(after) or after[0]['me'] != 1 or after[0]['role'] != 'host':
        raise ValueError('duplicate or invalid participant identities')
    if any(b['me'] != a['me'] or (i and a['role'] != 'client') for i, (b, a) in enumerate(zip(before, after))):
        raise ValueError('participant changed between snapshots')
    h = after[0]
    for state in after:
        if state['events'] != h['events']:
            raise ValueError('event ledgers differ across processes')
        if state['epoch'] != h['epoch']:
            raise ValueError('epochs differ')
        if meter is not None and state['meter'] != meter:
            raise ValueError('meter toggle did not reach the expected state')
        for p in range(1, 17):
            if state['players'][p][:2] != h['players'][p][:2]:
                raise ValueError('per-player damage totals differ')
            if idle and state['players'][p][2] != 0:
                raise ValueError('rolling DPS did not expire')
            if reset and any(state['players'][p]):
                raise ValueError('meter did not reset')
    if idle:
        for b, a in zip(before, after):
            if b['events'] != a['events'] or any(b['players'][p][:2] != a['players'][p][:2] for p in range(1, 17)):
                raise ValueError('idle interval changed damage history/totals')
    if attacker is not None:
        if not target or target not in before[0]['entities'] or target not in h['entities']:
            raise ValueError('target is missing from process snapshots')
        target_id, hp = before[0]['entities'][target]
        after_id, after_hp = h['entities'][target]
        loss = hp - max(0, after_hp)
        if after_id != target_id or loss <= 0:
            raise ValueError('fixture target changed or took no damage')
        if expected is not None and abs(loss - expected) > tol:
            raise ValueError(f'fixture HP loss {loss} differs from expected {expected}')
        new = {k: v for k, v in h['events'].items() if k not in before[0]['events']}
        matching = [v for v in new.values() if v[2] == attacker and v[3] == target_id]
        if not matching or abs(sum(v[5] for v in matching) - loss) > tol:
            raise ValueError('confirmed damage does not match measured target HP loss')
        for b, a in zip(before, after):
            for p in range(1, 17):
                delta = a['players'][p][0] - b['players'][p][0]
                if abs(delta - (loss if p == attacker else 0)) > tol:
                    raise ValueError(f'player {p} attribution differs from HP loss')
    return 'PASS combat state: all-process events, attribution and requested assertions match'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--before', nargs='+', required=True)
    parser.add_argument('--after', nargs='+', required=True)
    parser.add_argument('--attacker', type=int, choices=range(1, 17))
    parser.add_argument('--target')
    parser.add_argument('--expected', type=int)
    parser.add_argument('--tol', type=float, default=1)
    parser.add_argument('--idle', action='store_true')
    parser.add_argument('--reset', action='store_true')
    parser.add_argument('--meter', type=int, choices=(0, 1))
    args = parser.parse_args()
    try:
        if not math.isfinite(args.tol) or args.tol < 0:
            raise ValueError('invalid tolerance')
        print(verify([load(p) for p in args.before], [load(p) for p in args.after],
                     args.attacker, args.target, args.expected, args.tol, args.idle, args.reset, args.meter))
    except (OSError, ValueError, IndexError, KeyError) as exc:
        print(f'FAIL combat state: {exc}')
        return 1
    return 0
if __name__ == '__main__':
    raise SystemExit(main())
