#!/usr/bin/env python3
"""Strict process-state oracles for client targeting, attached canopies and joining."""
import argparse
import math
from pathlib import Path
import targeting_geometry


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
    # The geometry rule (tools/targeting_geometry.py): the player under test is far outside every splash
    # and strike radius from the other player, in the dumps before and after, so only the enemy's own
    # targeting can explain the hit. Read from both games' own player positions.
    for stage, (h, c) in (('before', (before_host, before_client)), ('after', (host, client))):
        targeting_geometry.assert_separation([h, c], target_slot, label=f'ai {stage}')
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


GEAR_FIELDS = {'chute': 6}         # every other visual's row has the twelve fields the equipment dump writes
# How far a peer's copy of an item may sit from where the owner's game puts it: its offset and turn from the bone the
# owner's game puts it on (found by name on the same body at the same moment, so the two sides' animations do not
# matter). Measured 0.000 units on the owner and every peer; the wrong bone is 0.06 units away for Doc, 0.07 for Max.
POSE_UNITS, POSE_DEGREES = 0.02, 3.0


def qmul(a, b):
    aw, ax, ay, az = a
    bw, bx, by, bz = b
    return (aw*bw - ax*bx - ay*by - az*bz, aw*bx + ax*bw + ay*bz - az*by,
            aw*by - ax*bz + ay*bw + az*bx, aw*bz + ax*by - ay*bx + az*bw)


def relative_pose(row):
    """A gearpose row (bone slot, item position and orientation w x y z, the named bone's slot, position and
    orientation) as the item's offset and turn in that bone's frame."""
    assert len(row) == 18, 'truncated gearpose row'
    v = list(map(float, row[3:10] + row[11:18]))
    assert all(math.isfinite(x) for x in v), 'nonfinite gearpose row'
    assert int(row[10]) >= 0, 'the bone the item belongs on was not found'
    gp, gq, bp, bq = v[0:3], v[3:7], v[7:10], v[10:14]
    n = math.sqrt(sum(x*x for x in bq)) or 1
    inv = (bq[0]/n, -bq[1]/n, -bq[2]/n, -bq[3]/n)
    d = (0.0, gp[0]-bp[0], gp[1]-bp[1], gp[2]-bp[2])
    off = qmul(qmul(inv, d), (inv[0], -inv[1], -inv[2], -inv[3]))[1:]
    return off, qmul(inv, gq)


def pose_gap(a, b):
    """Distance (units) and angle (degrees) between two relative poses."""
    dist = math.dist(a[0], b[0])
    na = math.sqrt(sum(x*x for x in a[1])) or 1
    nb = math.sqrt(sum(x*x for x in b[1])) or 1
    dot = abs(sum(x*y for x, y in zip(a[1], b[1]))) / (na*nb)
    return dist, math.degrees(2 * math.acos(min(1.0, dot)))


def gear(dumps, owner, name, expect=None, hidden=0, absent=False, lag=None):
    """An equipment visual of player `owner` (a gob attached to its body), as every process reads it from its own
    gob table: alive, on that body, bone slot, shown or hidden, animation, its model's sub-gobs, and its offset from
    the bone the owner's game puts it on. `absent`: it is gone everywhere. `lag` (ms): each peer's sub-gobs last changed within that long of the
    owner's (the processes share one clock), so a flame that comes on late on a peer fails even once it has caught up."""
    assert len(dumps) >= 2
    assert len({number(d, 'me') for d in dumps}) == len(dumps)
    assert sum(number(d, 'me') == owner for d in dumps) == 1
    assert len({number(d, 'level') for d in dumps}) == 1 and number(dumps[0], 'level') > 0
    bones, subs, poses, changed = set(), {}, {}, {}
    for d in dumps:
        rows = [r for r in d.get('gear', []) if int(r[0]) == owner and r[1] == name]
        if absent:
            assert not rows, f'{name} of player {owner} still exists in a process'
            continue
        assert len(rows) == 1, f'{name} of player {owner} is missing in a process'
        r = rows[0]
        assert len(r) == GEAR_FIELDS.get(name, 12), 'truncated gear row'
        assert int(r[2]) == 1, 'visual is not alive'
        assert int(r[4]) == 1, 'visual is not attached to its player body'
        assert int(r[3]) == hidden, 'visual hidden state is wrong' if hidden == 0 else 'visual is not hidden'
        assert int(r[5]) >= 0
        bones.add(int(r[5]))
        if expect is not None:
            assert int(r[6]) == expect, f'{name} animation is {r[6]}, not {expect}'
        if name == 'chute':
            continue
        me = number(d, 'me')
        subs[me] = r[10]
        changed[me] = int(r[11])
        pose = [p for p in d.get('gearpose', []) if int(p[0]) == owner and p[1] == name]
        assert len(pose) == 1, f'{name} of player {owner} has no pose row in a process'
        poses[me] = relative_pose(pose[0])
    if subs:
        # the model's sub-gobs (the jetpack's flame, the blower's puffs) shown and hidden as on the owner's side
        assert len(set(subs.values())) == 1, f'{name} sub-gobs differ from the owner\'s (count/hidden bits): {subs}'
        if lag is not None:
            late = {me: at - changed[owner] for me, at in changed.items() if abs(at - changed[owner]) > lag}
            assert not late, f'{name} sub-gobs changed on a peer this many ms after the owner: {late}'
        for me, pose in poses.items():
            dist, angle = pose_gap(pose, poses[owner])
            assert dist <= POSE_UNITS and angle <= POSE_DEGREES, \
                f'{name} on player {me}\'s game is {dist:.3f} units / {angle:.1f} degrees from where the owner\'s game puts it'
    assert absent or len(bones) == 1, 'attached to different bones in different processes'


def join(dump, phase):
    assert dump['role'] == [['client']]
    assert number(dump, 'join') == phase
    if phase in (0, 1, 5, 6):
        assert number(dump, 'level') == 0 and number(dump, 'join_screen') == 1
    if phase == 3:
        assert number(dump, 'level') > 0 and number(dump, 'join_screen') == 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('kind', choices=('ai', 'ai-host', 'canopy', 'gear', 'join'))
    parser.add_argument('files', nargs='+')
    parser.add_argument('--owner', type=int)
    parser.add_argument('--expect', type=int)
    parser.add_argument('--name')
    parser.add_argument('--hidden', type=int, default=0)
    parser.add_argument('--absent', action='store_true')
    parser.add_argument('--lag', type=int)
    args = parser.parse_args()
    try:
        rows = [load(p) for p in args.files]
        if args.kind in ('ai', 'ai-host'):
            assert len(rows) == 4
            ai(*rows, host_near=args.kind == 'ai-host')
        elif args.kind == 'canopy': canopy(rows, args.owner, args.expect)
        elif args.kind == 'gear': gear(rows, args.owner, args.name, args.expect, args.hidden, args.absent, args.lag)
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
