"""Shared geometry rule for every test of enemy targeting, aggro or damage to a player.

The player under test (the one the enemy is supposed to pick) must be far from
every other player. Standing them side by side lets area damage aimed at the
nearby player pass the test even when the enemy never targets the one being
tested. The rule:

  * the separation is read from process state (the `player` row of each game's
    own state dump), never from the positions a script asked for;
  * it is at least AOE_FACTOR times the largest splash radius or attack reach
    involved (SPLASH_RADII below);
  * what one game says about another player (a `puppet` row) must agree with
    that player's own game, so a stale or unsynced player cannot satisfy it.

Command line (used by the shell suites):
  targeting_geometry.py check [--target SLOT] [--min N] [--label TEXT] DUMP DUMP...
        one dump per game; the target (default: the first client) must be
        MIN_SEPARATION or more from every other player
  targeting_geometry.py scan [--root DIR]
        every targeting test in tools/ must call this module
  targeting_geometry.py limits
        print the radii and the minimum
"""
import argparse
import math
import sys
from pathlib import Path
import re

# Largest radius of area damage, or reach of a direct attack, in game units,
# for what can hurt a player in the targeting tests, with where it comes from.
SPLASH_RADII = {
    'script area damage, largest mdkDealDamageArea in the level scripts': (20, 'level scripts'),
    'Hans ground pound, dist < 45 in Level.r8HansAttack': (45, 'level 1 script'),
    'Grunt / Conehead strike reach, native 0x434e25': (50, 'engine, see PRESENCE-VALIDATION.md'),
    'native projectile explosions (Doganboy grenade, Hans missile, round enemies, turrets)':
        (100, 'ASSUMED upper bound: not measured, see docs/TESTING.md'),
}
AOE_FACTOR = 3
LARGEST_RADIUS = max(r for r, _ in SPLASH_RADII.values())
MIN_SEPARATION = AOE_FACTOR * LARGEST_RADIUS
# One player's view of another (a puppet) may lag by interpolation; beyond this
# the two views of the same player disagree and the separation is not trusted.
PUPPET_TOLERANCE = 15.0


def parse(text):
    """Rows of a coop_state dump as {key: [fields...]}."""
    return parse_rows(line.split() for line in text.splitlines())


def parse_rows(split_lines):
    """The same from already split lines (what the Python harnesses hold)."""
    rows = {}
    for fields in split_lines:
        if fields:
            rows.setdefault(fields[0], []).append(list(fields[1:]))
    return rows


def load(path):
    return parse(Path(path).read_text())


def _vec(fields, what):
    try:
        v = tuple(float(x) for x in fields)
    except ValueError:
        raise AssertionError(f'{what}: unreadable position {fields}')
    if len(v) != 3 or not all(math.isfinite(x) for x in v):
        raise AssertionError(f'{what}: bad position {fields}')
    return v


def own_position(rows, what='game'):
    """The position of the player that game controls, from its own dump."""
    players = rows.get('player', [])
    if len(players) != 1 or len(players[0]) != 5:
        raise AssertionError(f'{what}: missing or duplicate player row')
    return _vec(players[0][2:5], what)


def slot(rows, what='game'):
    me = rows.get('me', [])
    if len(me) != 1 or not me[0][0].isdigit():
        raise AssertionError(f'{what}: missing player number')
    return int(me[0][0])


def puppet_position(rows, number):
    found = [r for r in rows.get('puppet', []) if r and r[0] == str(number)]
    return _vec(found[0][1:4], f'puppet {number}') if len(found) == 1 else None


def assert_separation(dumps, target=None, minimum=None, label='targeting test'):
    """Raise AssertionError unless the target player keeps `minimum` units
    (default MIN_SEPARATION) from every other player, measured from the process
    state of every game. dumps: parsed state dumps, one per game, any order
    (exactly one host). target: player number under test (default: the first
    client). Returns the smallest separation found."""
    minimum = MIN_SEPARATION if minimum is None else minimum
    if minimum < MIN_SEPARATION:
        raise AssertionError(f'{label}: minimum {minimum} is below the rule ({MIN_SEPARATION})')
    if len(dumps) < 2:
        raise AssertionError(f'{label}: need the dumps of at least two games')
    at = {}
    for i, rows in enumerate(dumps):
        what = f'game {i + 1}'
        n = slot(rows, what)
        if n in at:
            raise AssertionError(f'{label}: two dumps for player {n}')
        at[n] = (own_position(rows, what), rows)
    hosts = [n for n, (_, rows) in at.items() if rows.get('role') == [['host']]]
    if hosts != [1]:
        raise AssertionError(f'{label}: need exactly one host dump, player 1')
    if len({tuple(map(tuple, rows.get('level', []))) for _, rows in at.values()}) != 1:
        raise AssertionError(f'{label}: the games are not on one level')
    if target is None:
        clients = sorted(n for n in at if n != 1)
        if not clients:
            raise AssertionError(f'{label}: no client dump')
        target = clients[0]
    if target not in at:
        raise AssertionError(f'{label}: no dump for target player {target}')
    # Every view of a player must agree with that player's own game.
    for viewer, (_, rows) in at.items():
        for n, (own, _) in at.items():
            seen = puppet_position(rows, n) if n != viewer else None
            if seen is not None and math.dist(seen, own) > PUPPET_TOLERANCE:
                raise AssertionError(f'{label}: game {viewer} sees player {n} at {seen} but that game is at {own}')
    smallest = math.inf
    for n, (pos, _) in at.items():
        if n == target:
            continue
        d = math.dist(at[target][0], pos)
        if d < minimum:
            raise AssertionError(f'{label}: player {n} is {d:.1f} units from target player {target}, '
                                 f'the rule is at least {minimum:g} (3x the largest splash radius)')
        smallest = min(smallest, d)
    return smallest


# ---- the scan: a new targeting test cannot forget the rule -------------------

# What marks a script as a test of enemy targeting, aggro or damage to a player.
TARGETING_MARKERS = re.compile(
    r'OBJ_GRUNT|OBJ_DOGANBOY\b|OBJ_HANS\b|qa_melee|mdkWalkerAlert|direct_source|ai_fire|'
    r'r8HansAttack|hanspilot|coop_presence_test\(0,[01],[01],1|dog bites|r2_dog01|'
    r'\bai_hook\b|mdkSetAITargetGob')
HELPER_CALL = re.compile(r'targeting_geometry|assert_separation|mdk_assert_far')
# Files that mention enemies but test something else, with the reason.
NOT_TARGETING = {
    'qa_shots_native.sh': 'projectile replay: a Doganboy fires, players are raised by 20000 hp, nothing asserts a target or a hit',
    'qa_playtest.sh': 'shared pickups and the dome puzzle: reads an enemy bar and hurts players with scripted nil-attacker damage',
}
SCAN_SUFFIXES = ('.sh', '.py')
SELF = 'targeting_geometry.py'


def scan(root):
    """List (file, problem) for targeting tests in root/tools that do not call the helper."""
    problems = []
    tools = Path(root) / 'tools'
    for path in sorted(tools.iterdir()):
        name = path.name
        if path.suffix not in SCAN_SUFFIXES or name == SELF or name.startswith('test_'):
            continue
        if not (name.startswith('qa_') or name.startswith('check_') or name.endswith('_check.py')
                or name.endswith('showcase.sh')):
            continue
        text = path.read_text(errors='replace')
        if not TARGETING_MARKERS.search(text) or name in NOT_TARGETING:
            continue
        if not HELPER_CALL.search(text):
            problems.append((name, 'tests enemy targeting or damage but never asserts player separation'))
    for name in NOT_TARGETING:
        if not (tools / name).exists():
            problems.append((name, 'listed as not a targeting test but the file does not exist'))
    return problems


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    c = sub.add_parser('check')
    c.add_argument('dumps', nargs='+')
    c.add_argument('--target', type=int)
    c.add_argument('--min', type=float)
    c.add_argument('--label', default='targeting test')
    s = sub.add_parser('scan')
    s.add_argument('--root', default=str(Path(__file__).resolve().parents[1]))
    sub.add_parser('limits')
    a = ap.parse_args(argv)
    if a.cmd == 'limits':
        for name, (r, basis) in SPLASH_RADII.items():
            print(f'{r:6g}  {name} [{basis}]')
        print(f'minimum separation {MIN_SEPARATION:g} ({AOE_FACTOR} x {LARGEST_RADIUS:g})')
        return 0
    if a.cmd == 'scan':
        bad = scan(a.root)
        for name, why in bad:
            print(f'FAIL geometry scan {name}: {why}')
        if not bad:
            print('PASS geometry scan: every targeting test asserts separation')
        return 1 if bad else 0
    try:
        rows = [load(p) for p in a.dumps]
        d = assert_separation(rows, a.target, a.min, a.label)
    except (AssertionError, ValueError, OSError) as exc:
        print(f'FAIL geometry {exc}')
        return 1
    print(f'PASS geometry {a.label}: players are {d:.1f} units apart (rule {MIN_SEPARATION:g})')
    return 0


if __name__ == '__main__':
    sys.exit(main())
