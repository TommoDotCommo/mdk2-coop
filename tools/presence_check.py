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


def ai(before_host, before_client, host, client):
    assert host['role'] == [['host']] and client['role'] == [['client']]
    slot = number(client, 'me')
    assert slot > 1 and number(host, 'me') == 1
    assert number(host, 'level') == number(client, 'level') > 0
    assert number(before_host, 'level') == number(host, 'level')
    assert number(before_client, 'me') == slot and number(before_client, 'level') == number(client, 'level')
    assert number(host, 'ai_hook') == 1 and number(host, 'melee_only') == 1
    assert number(host, 'life') == number(client, 'life') == 0
    assert math.dist(player(host)[1], player(client)[1]) > 25, 'host must be far away'
    assert player(client)[0] < player(before_client)[0], 'client did not lose hp'
    assert player(host)[0] == player(before_host)[0], 'host was damaged'
    assert client.get('direct_source') == [['qa_melee']], 'HURT came from another actor'
    assert number(client, 'direct_receipts') > number(before_client, 'direct_receipts'), 'no applied direct HURT'
    matches = [r for r in host.get('ai', []) if r[0] == 'qa_melee']
    assert len(matches) == 1 and len(matches[0]) == 4, 'missing target memory'
    row = matches[0]
    assert row[1] == f'@p{slot}' and int(row[2]) > 0 and int(row[3]) > 0, 'enemy did not target and strike client'
    old = [r for r in before_host.get('ai', []) if r[0] == 'qa_melee']
    assert int(row[3]) > (int(old[0][3]) if old else 0), 'stale hit evidence'
    enemies = [r for r in host.get('ent', []) if r[-1] == 'qa_melee']
    assert len(enemies) == 1 and int(enemies[0][1]) == 202 and int(enemies[0][2]) > 0
    assert math.dist(tuple(map(float, enemies[0][3:6])), player(client)[1]) < 5, 'enemy did not chase into melee range'


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
    parser.add_argument('kind', choices=('ai', 'canopy', 'join'))
    parser.add_argument('files', nargs='+')
    parser.add_argument('--owner', type=int)
    parser.add_argument('--expect', type=int)
    args = parser.parse_args()
    try:
        rows = [load(p) for p in args.files]
        if args.kind == 'ai':
            assert len(rows) == 4
            ai(*rows)
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
