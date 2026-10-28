#!/usr/bin/env python3
"""Strict process-state oracles for client targeting, attached canopies and joining."""
import argparse
import math
from pathlib import Path


def load(path):
    rows = {}
    for line in Path(path).read_text().splitlines():
        fields = line.split()
        if fields:
            rows.setdefault(fields[0], []).append(fields[1:])
    for key in ('role', 'level', 'me', 'life', 'join', 'join_screen', 'ai_hook', 'melee_only', 'direct_receipts'):
        if len(rows.get(key, [])) != 1:
            raise ValueError(f'missing/duplicate {key}')
    for key in ('player', 'puppet', 'ent', 'canopy'):
        for row in rows.get(key, []):
            if key == 'ent' and len(row) == 2 and row[1] == 'gone':
                continue
            values = row[1:-1] if key == 'ent' else row
            if any(not math.isfinite(float(v)) for v in values):
                raise ValueError(f'nonfinite {key}')
    return rows


def number(rows, key):
    return int(rows[key][0][0])


def player(rows):
    assert len(rows.get('player', [])) == 1, 'missing player'
    row = rows['player'][0]
    assert len(row) == 5, 'truncated player'
    return int(row[1]), tuple(map(float, row[2:]))


def ai(before_host, before_client, host, client, host_near=False):
    assert host['role'] == [['host']] and client['role'] == [['client']]
    slot = number(client, 'me')
    assert slot > 1 and number(host, 'me') == 1
    assert number(host, 'level') == number(client, 'level') > 0
    assert number(before_host, 'level') == number(host, 'level')
    assert number(before_client, 'me') == slot and number(before_client, 'level') == number(client, 'level')
    assert number(host, 'ai_hook') == 1 and number(host, 'melee_only') == 1
    assert number(host, 'life') == number(client, 'life') == 0
    target, before_target = (host, before_host) if host_near else (client, before_client)
    other, before_other = (client, before_client) if host_near else (host, before_host)
    target_slot = 1 if host_near else slot
    assert math.dist(player(host)[1], player(client)[1]) > 100, 'other player must be out of strike range'
    assert player(target)[0] < player(before_target)[0], 'selected player did not lose hp'
    assert player(other)[0] == player(before_other)[0], 'distant player was damaged'
    assert number(target, 'direct_receipts') > number(before_target, 'direct_receipts'), 'no applied direct HURT'
    matches = [r for r in host.get('ai', []) if r[0] == 'qa_melee']
    assert len(matches) == 1 and len(matches[0]) == 4, 'missing target memory'
    row = matches[0]
    old = [r for r in before_host.get('ai', []) if r[0] == 'qa_melee']
    assert row[1] == f'@p{target_slot}' and int(row[2]) > (int(old[0][2]) if old else 0), 'no native target query'
    assert int(row[3]) > (int(old[0][3]) if old else 0), 'no new direct strike'
    enemies = [r for r in host.get('ent', []) if r[-1] == 'qa_melee']
    initial = [r for r in before_host.get('ent', []) if r[-1] == 'qa_melee']
    assert len(enemies) == len(initial) == 1
    assert enemies[0][0] == initial[0][0], 'enemy was replaced'
    assert target.get('direct_source') == [['n' + enemies[0][0]]], 'HURT came from another actor'
    assert int(enemies[0][1]) == 202 and int(enemies[0][2]) > 0
    start = tuple(map(float, initial[0][3:6]))
    end = tuple(map(float, enemies[0][3:6]))
    before_distance = math.dist(start, player(before_target)[1])
    after_distance = math.dist(end, player(target)[1])
    # Native Grunt strike checks 50 units and pi/4 at 0x434e25..0x434e3d.
    assert before_distance > 50 and after_distance < 50, 'enemy did not enter native strike range'
    assert after_distance < before_distance - 8 and math.dist(start, end) > 8, 'enemy did not approach'
    assert math.dist(player(before_target)[1], player(target)[1]) < 3, 'player moved toward enemy'


def canopy(dumps, owner, expected):
    assert len(dumps) >= 2
    assert len({number(d, 'me') for d in dumps}) == len(dumps)
    assert sum(number(d, 'me') == owner for d in dumps) == 1
    assert len({number(d, 'level') for d in dumps}) == 1 and number(dumps[0], 'level') > 0
    samples = []
    for d in dumps:
        rows = [r for r in d.get('canopy', []) if int(r[0]) == owner]
        assert len(rows) == 1 and len(rows[0]) == 8, 'missing native canopy'
        r = rows[0]
        assert int(r[2]) == expected, 'wrong canopy controller animation'
        samples.append(r)
    source = samples[[number(d, 'me') for d in dumps].index(owner)]
    for r in samples:
        assert r[1:4] == source[1:4], 'canopy flags/current/previous differ'
        assert r[4] == source[4] and r[6] == source[6], 'canopy active blocks differ'
        assert abs(float(r[5])-float(source[5])) < .25
        assert abs(float(r[7])-float(source[7])) < .25


def join(dump, phase):
    assert dump['role'] == [['client']]
    assert number(dump, 'join') == phase
    if phase in (0, 1, 5, 6):
        assert number(dump, 'level') == 0 and number(dump, 'join_screen') == 1
    if phase == 3:
        assert number(dump, 'level') > 0 and number(dump, 'join_screen') == 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('kind', choices=('ai', 'ai-host', 'canopy', 'join'))
    parser.add_argument('files', nargs='+')
    parser.add_argument('--owner', type=int)
    parser.add_argument('--expect', type=int)
    args = parser.parse_args()
    try:
        rows = [load(p) for p in args.files]
        if args.kind in ('ai', 'ai-host'):
            assert len(rows) == 4
            ai(*rows, host_near=args.kind == 'ai-host')
        elif args.kind == 'canopy': canopy(rows, args.owner, args.expect)
        else:
            assert len(rows) == 1
            join(rows[0], args.expect)
    except (AssertionError, ValueError, KeyError, IndexError, OSError) as exc:
        print(f'FAIL presence {args.kind}: {exc}')
        return 1
    print(f'PASS presence {args.kind}')
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
