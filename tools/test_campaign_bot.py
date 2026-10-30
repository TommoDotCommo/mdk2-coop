#!/usr/bin/env python3
"""Game-free checks of the campaign bot's planner (tools/campaign_bot.py): the walking rules on
scanned cells, reachability, the room-portal route and the scan's line format."""
import math
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import campaign_bot as bot


def grid(heights):
    """Cells from {(i, j): z}, linked to their four neighbours, as coop.NavTick writes them."""
    lines = []
    for (i, j), z in heights.items():
        line = f'c {i * 2:.1f} {j * 2:.1f} {z:.2f} {i} {j}'
        for di, dj in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            n = (i + di, j + dj)
            if n in heights:
                line += f' {n[0]} {n[1]} {heights[n]:.2f}'
        lines.append(line)
    nav = bot.Nav()
    nav.add_lines('\n'.join(lines))
    return nav


def check(label, ok):
    if not ok:
        raise SystemExit(f'FAIL {label}')
    print(f'PASS {label}')


# A flat corridor 10 cells long, a 33 degree ramp beside it (1.3 per cell, Max slid down one), and a
# 2-unit ledge onto a flat landing.
flat = {(i, 0): 0.0 for i in range(10)}
ramp = {(0, j): 1.3 * j for j in range(1, 6)}
ledge = {(i, 3): 2.0 for i in range(3, 8)}
step = {(i, 1): 0.0 for i in range(3, 8)}
step.update({(i, 2): 0.0 for i in range(3, 8)})
nav = grid({**flat, **ramp, **ledge, **step})

path = nav.path([0, 0, 0], [18, 0, 0])
check('a flat corridor is walked straight, its waypoints only where the way turns', path and len(path) <= 3
      and path[-1][:2] == [18.0, 0.0])
check('a 33 degree ramp is not walkable', nav.step_cost(nav.key(0, 1, 1.3), nav.key(0, 2, 2.6)) is None)
check('a 2 unit rise onto a flat landing is a jump', nav.step_cost(nav.key(4, 2, 0), nav.key(4, 3, 2.0)) is not None)
up = nav.path([0, 0, 0], [8, 6, 2.0])
check('the ledge is reached over the jump, not up the ramp',
      up and all(p[0] != 0 or p[1] == 0 for p in up) and abs(up[-1][2] - 2.0) < 1e-6)
check('the reachable set has no ramp cell above the first',
      not any(nav.cells[k][2] > 1.3 + 1e-6 and nav.cells[k][0] == 0 for k in bot.reachable(nav, [0, 0, 0])))
check('reachable_toward stops at the nearest reachable cell', bot.reachable_toward(nav, [0, 0, 0], [0, 10, 6.5])[2] <= 2.0)

# Room portals: three boxes in a row, the goal in the last.
level = bot.Level.__new__(bot.Level)
level.m = {'portals': []}
level.rooms = {'a': {'min': [0, 0, 0], 'max': [10, 10, 5]}, 'b': {'min': [10, 0, 0], 'max': [20, 10, 5]},
               'c': {'min': [20, 0, 0], 'max': [30, 10, 5]}, 'big': {'min': [-100, -100, -100], 'max': [100, 100, 100]}}
level.edges = {'a': [('b', [10, 5, 1], 'touch')], 'b': [('a', [10, 5, 1], 'touch'), ('c', [20, 5, 1], 'touch')],
               'c': [('b', [20, 5, 1], 'touch')]}
start, steps = level.path([2, 5, 1], [28, 5, 1])
check('a point belongs to the smallest box that holds it', start == 'a')
check('the route crosses the portals in order', [s['via'] for s in steps] == ['touch', 'touch', 'goal']
      and [s['room'] for s in steps] == ['b', 'c', 'c'])
# A recording: walking east 20 units with W held, jumping midway, then standing and firing.
import os, tempfile
lines = ['MDKROUTE 1 3902', 'S 2 3 101 0 0 0 1 0 0 0']
for i in range(0, 121):
    keys = bytearray(33)
    keys[17 // 8] |= 1 << (17 % 8)
    if 40 <= i <= 45:
        keys[57 // 8] |= 1 << (57 % 8)
    if i > 100:
        keys[256 // 8] |= 1 << (256 % 8)
    x = min(i, 100) * 0.2
    lines.append(f'F {i * 100} {keys.hex()} 0 0 0 2 3 101 {x:.3f} 0 0 1 0 0 0')
fd, path = tempfile.mkstemp(suffix='.route')
os.write(fd, ('\n'.join(lines) + '\n').encode())
os.close(fd)
pts = bot.route_waypoints(bot.load_route(path))
os.unlink(path)
check('a recording becomes points every 3 units, movement keys left to the pilot',
      7 <= len(pts) <= 9 and all(17 not in p['keys'] for p in pts))
check('a held jump stays with the stretch it was held on', any(57 in p['keys'] for p in pts))
check('firing while standing still adds no point but goes with the last one', 256 in pts[-1]['keys']
      and sum(1 for p in pts if p['pos'][0] == 20.0) == 1)
# A one-way step: from the corridor a cell north can be entered but not left.
trap = bot.Nav()
trap.add_lines('c 0.0 0.0 0.00 0 0 1 0 0.00 0 1 0.00\nc 2.0 0.0 0.00 1 0 0 0 0.00\nc 0.0 2.0 0.00 0 1')
check('a cell with no way back is reachable but not safe',
      trap.key(0, 1, 0) in bot.reachable(trap, [0, 0, 0], safe=False)
      and trap.key(0, 1, 0) not in bot.reachable(trap, [0, 0, 0]))
# A bay whose hatch opens only once its turrets are dead (level 2's shaft scripts, trimmed).
import campaign_mine as mine
script = '''Level.SetUpDoorAndEnemies = function(room, door)
    room.lockeddoor = door
    mdkProxDoorLock(door, 1)
end
Level.AddEnemy = function(room, enemy)
    room.numenemies = room.numenemies + 1
    mdkSetLuaEvent(enemy, "OnDie", Level.EnemyOnDie)
end
bay1.OnCreate = function(gob)
    Level.SetUpDoorAndEnemies(gob, hatch1)
    Level.AddEnemy(gob, turret1)
    Level.AddEnemy(gob, turret2)
end
'''
check('a hatch locked by its bay\'s turrets is mined with them', mine.enemy_locks(script) == {'hatch1': ['turret1', 'turret2']})
# A waypoint the hero failed to reach twice is planned around.
bot.Nav.blocked.add(bot.Nav.spot(nav.cells[nav.key(4, 0, 0)][:3]))
check('a blocked cell is not reachable', nav.key(4, 0, 0) not in bot.reachable(nav, [0, 0, 0], safe=False))
bot.Nav.blocked.clear()
# A lift ride (level 2 room 9): standing still while the platform rises, no jump held.
frames = [(i * 100, set(), 2, 9, (0.0, 0.0, 0.2 * i)) for i in range(60)]
frames += [(6000 + i * 100, {17}, 2, 9, (0.3 * i, 0.0, 11.8)) for i in range(30)]
pts = bot.route_waypoints(frames)
check('a lift ride is followed by waiting, the walk after it is not',
      any(p['ride'] for p in pts if p['pos'][2] < 11) and not any(p['ride'] for p in pts if p['pos'][0] > 1))


# Following flights (level 2 rooms 6 and 9, from the owner's recordings). Each rule below has a mutant,
# a broken copy of the rule that the same checks must catch.
def flight_rules(b, quiet=False):
    """The flight-following rules' checks against module b: [(label, ok)]."""
    out = []
    # room 9: walk to a lift platform 6 units above the floor, a jetpack hop onto it (jump held, never
    # 2 units above the last ground: not "air"), stand, ride it up slowly, then a 20 unit fall back down
    fr = [(k * 100, {17}, 2, 9, (0.3 * k, 0.0, 0.0)) for k in range(20)]
    fr += [(2000 + k * 100, {17, 57}, 2, 9, (6.0 + 0.3 * k, 0.0, min(1.9, 0.4 * k) + max(0, k - 5) * 0.8))
           for k in range(10)]
    fr += [(3000 + k * 100, set(), 2, 9, (9.0, 0.0, 6.0)) for k in range(20)]
    fr += [(5000 + k * 100, set(), 2, 9, (9.0, 0.0, 6.0 + 0.4 * k)) for k in range(50)]
    fr += [(10000 + k * 100, set(), 2, 9, (9.0, 0.0, 26.0 - 2.5 * min(k, 8))) for k in range(20)]
    fr += [(12000 + k * 100, {17}, 2, 9, (9.0 + 0.3 * k, 0.0, 6.0)) for k in range(40)]
    pts = b.route_waypoints(fr)
    fly = b.flown(pts)
    hop = [k for k, p in enumerate(pts) if 57 in p['keys'] and p['pos'][2] > 1]
    land = next(k for k, p in enumerate(pts) if p['pos'][2] >= 5.9 and k not in hop)
    out.append(('a jetpack hop onto a lift is flown at its height, landing included',
                hop and all(k in fly for k in hop) and land in fly))
    out.append(('the walk to the hop is not flown', not any(k in fly for k, p in enumerate(pts) if p['pos'][0] < 5.5)))
    out.append(('the walk after a landing is not flown', not any(k in fly for k, p in enumerate(pts) if p['pos'][0] > 11)))
    out.append(('a slow lift ride is a ride, a fall down the shaft is not',
                any(p['ride'] for p in pts if 6.5 < p['pos'][2] < 25.5 and p['ms'] < 10000)
                and not any(p['ride'] for p in pts if 10000 < p['ms'] < 12000)))
    out.append(('a failed flight is tried again from the last point walked',
                b.stretch_start(pts, fly, max(hop)) == min(hop) - 1))
    # refuellers: one on a floor stood on, one hanging in a shaft hovered beside (jump tapped)
    floor = {'name': 'f', 'pos': [0.0, 0.0, 0.0]}
    pump = {'name': 'p', 'pos': [0.0, 0.0, 50.0]}
    stand = [{'pos': (0.5, 0.0, 0.0), 'keys': [], 'air': False}]
    hover = [{'pos': (1.0, 0.0, 52.0 + k % 3), 'keys': [57] if k % 2 else [], 'air': bool(k % 2)} for k in range(6)]
    out.append(('a refueller on a floor is stood on, a pump in a shaft is hovered by',
                b.refuel_mode(stand, floor) == 'stand' and b.refuel_mode(hover, pump) == 'hover'
                and b.refuel_mode(stand, pump) is None))
    # fuel: a 120 unit climb (8 s of thrust) after 5 s used runs dry; a fresh tank goes; a pump on the way
    climb = [{'pos': (0.0, 0.0, 3.0 * k), 'keys': [57], 'air': k > 0} for k in range(41)]
    cfly = set(range(1, 41))
    out.append(('a long climb on a part-used tank refuels first, a fresh tank goes',
                b.needs_fuel(5000, climb, cfly, 1) and not b.needs_fuel(0, climb, cfly, 1)))
    out.append(('a refueller on the way counts as the end of a climb',
                not b.needs_fuel(5000, climb, cfly, 1, pads=[(0.0, 0.0, 30.0)])))
    # moving lifts (room 9's platforms swing on their own): the flight that lands on one waits for it
    # to come down, holds beside it for the last points, and hovers above the landing
    lift = {'name': 'plat', 'pos': [9.0, 0.0, 6.0]}
    far = {'name': 'far', 'pos': [60.0, 0.0, 6.0]}
    out.append(('the flight landing on a lift is planned with that lift, not a far one',
                [bd['lift']['name'] for bd in b.plan_boardings(pts, fly, [far, lift])] == ['plat']
                and not b.plan_boardings(pts, fly, [far])))
    # a longer hop: the hero holds beside the lift, out from under its edge (8 units), not right at it
    lng = [{'pos': (0.0, 0.0, 0.0), 'keys': [], 'air': False, 'ride': False}] \
        + [{'pos': (2.0 * k, 0.0, 3.0 + k), 'keys': [57], 'air': True, 'ride': False} for k in range(1, 9)] \
        + [{'pos': (16.0, 0.0, 12.0), 'keys': [], 'air': False, 'ride': True}] * 3
    lfly = set(range(1, 9))
    plan = b.plan_boardings(lng, lfly, [{'name': 'L', 'pos': [17.0, 0.0, 12.0]}])
    tr = b.tracked_points(b.plan_boardings(lng, lfly, [{'name': 'L', 'pos': [17.0, 0.0, 12.0]}]))
    out.append(('only the last stretch of a flight onto a lift keeps above the platform',
                set(tr.values()) == {'L'} and min(tr) == b.plan_boardings(lng, lfly, [{'name': 'L', 'pos': [17.0, 0.0, 12.0]}])[0]['wait_at']
                and max(tr) == 8))
    out.append(('a boarding holds out from under the lift (8 units), from the ground',
                len(plan) == 1 and plan[0]['wait_at'] is not None
                and math.dist(lng[plan[0]['wait_at'] - 1]['pos'][:2], (17.0, 0.0)) >= 8
                and math.dist(lng[plan[0]['wait_at']]['pos'][:2], (17.0, 0.0)) < 8 and not plan[0]['from_ride']))
    # a hop from a ride to the next lift is not waited for (the lifts are in step; the first one would go down)
    rides = [{'pos': (0.0, 0.0, 3.0 * k), 'keys': [], 'air': False, 'ride': True} for k in range(4)]
    hop2 = [{'pos': (1.0 * k, 0.0, 12.0 + k), 'keys': [57], 'air': True, 'ride': False} for k in range(4)]
    stand2 = [{'pos': (4.0, 0.0, 16.0), 'keys': [], 'air': False, 'ride': True}] * 3
    p2 = rides + hop2 + stand2
    f2 = set(range(4, 8))
    plan2 = b.plan_boardings(p2, f2, [{'name': 'next', 'pos': [4.0, 0.0, 16.0]}])
    out.append(('a hop from one lift to the next is flagged as leaving a ride', len(plan2) == 1 and plan2[0]['from_ride']))
    out.append(('a lift coming down to the boarding height is the moment to go',
                b.lift_ready(243.0, -3.0, 239.3) and b.lift_ready(240.5, -1.0, 239.3)))
    out.append(('a lift going up, far above, or already past the height is not',
                not b.lift_ready(243.0, 3.0, 239.3) and not b.lift_ready(255.0, -3.0, 239.3)
                and not b.lift_ready(238.0, -1.0, 239.3)))
    # carrying on from the earliest point nearly as near as the nearest: a hero hovering at a pump sits as
    # near to a later point at the same height (not flown) and must not skip to it
    loop = [{'pos': (3.0 * k, 0.0, 0.0)} for k in range(10)] + [{'pos': (12.5, 0.5, 0.0)}] \
        + [{'pos': (15.0, 0.0, 30.0)}, {'pos': (0.0, 0.0, 0.0)}]
    out.append(('the route carries on from the earliest point nearly as near as the nearest',
                b.nearest_ahead((12.4, 0.3, 0.0), loop, 0) == 4 and b.nearest_ahead((0.1, 0.0, 0.0), loop, 0) == 0))
    out.append(('a point clearly nearer than the current one is skipped to',
                b.nearest_ahead((26.9, 0.0, 0.0), loop, 2) == 9))
    # the turrets a bay's hatch waits for: those near the hero on his floor, not ones on other floors or far off
    objs = [{'name': 'a', 'type': 'OBJ_TURRET', 'pos': [10.0, 0.0, 46.0]}, {'name': 'b', 'type': 'OBJ_TURRET', 'pos': [0.0, 20.0, 52.0]},
            {'name': 'c', 'type': 'OBJ_TURRET', 'pos': [0.0, 5.0, 66.0]}, {'name': 'd', 'type': 'OBJ_TURRET', 'pos': [90.0, 0.0, 46.0]},
            {'name': 'e', 'type': 'OBJ_DESTRUCTABLE', 'pos': [1.0, 1.0, 46.0]}]
    out.append(('the turrets to clear are the near ones on the hero\'s floor',
                b.turrets_to_clear(objs, (0.0, 0.0, 46.0)) == ['a', 'b']))
    # hovering with jump held after a climb (the owner's turret fight at a bay) stays flown until jump is let go
    hv = [{'ms': k * 100, 'pos': (0.0, 0.0, 3.0 * k), 'keys': [57], 'air': True, 'ride': False} for k in range(5)] \
        + [{'ms': 600 + k * 100, 'pos': (0.0, 0.0, 12.0), 'keys': [57, 256], 'air': False, 'ride': False} for k in range(4)] \
        + [{'ms': 1000 + k * 100, 'pos': (0.0, 0.0, 12.0), 'keys': [], 'air': False, 'ride': False} for k in range(3)]
    out.append(('a hover with jump held after a climb stays flown until jump is let go',
                b.flown(hv) >= set(range(0, 9)) and not (b.flown(hv) & {10, 11})))
    # a pump hanging in a shaft: the points where the person tapped jump beside it are flown, a point
    # far from it is not
    hp = [{'pos': (1.0, 0.0, 52.0 + k % 3), 'keys': [57] if k % 2 else [], 'air': bool(k % 2)} for k in range(6)] \
        + [{'pos': (30.0, 0.0, 52.0), 'keys': [57], 'air': False}]
    got = b.hover_points(hp, [pump])
    out.append(('the points hovered by a hanging pump are flown, others are not',
                got == {1, 3, 5} and not b.hover_points(hp, [floor])))
    return out


results = flight_rules(bot)
for label, ok in results:
    check(label, ok)

# mutants: each must make at least one check fail
import types
real = {n: getattr(bot, n) for n in ('flown', 'refuel_mode', 'needs_fuel', 'stretch_start', 'LIFT_SPEED', 'lift_ready',
                                    'boarding', 'plan_boardings', 'nearest_ahead', 'hover_points', 'tracked_points', 'turrets_to_clear')}
mutants = {
    'only "air" points are flown (the hop onto a lift is walked)':
        dict(flown=lambda pts: {k for k, p in enumerate(pts) if p.get('air')}),
    'every point after a landing counts as the landing':
        dict(flown=lambda pts: (lambda f: f | {k for k in range(min(f, default=len(pts)), len(pts))})(real['flown'](pts))),
    'falls count as lift rides (no speed limit)': dict(LIFT_SPEED=1e9),
    'every refueller is stood on': dict(refuel_mode=lambda pts, pad, reach=5.0: 'stand'),
    'refuellers on the way are ignored':
        dict(needs_fuel=lambda used, pts, fly, i, pads=(), **k: real['needs_fuel'](used, pts, fly, i)),
    'a failed flight is retried from where it failed': dict(stretch_start=lambda pts, fly, i: i),
    'the lift is boarded whatever way it moves': dict(lift_ready=lambda z, vz, bz, high=4.0: True),
    'the lift is boarded only when it is already at the bottom':
        dict(lift_ready=lambda z, vz, bz, high=4.0: abs(z - bz) < 0.5),
    'the nearest point wins outright (skips over flown points)':
        dict(nearest_ahead=lambda here, pts, i, window=80, slack=1.5: min(range(i, min(len(pts), i + window)),
                                                                         key=lambda k: math.dist(here, pts[k]['pos']))),
    'a hover after a climb is walked': dict(flown=lambda pts: {k for k, p in enumerate(pts) if p.get('air')}),
    'a hovered pump is not flown': dict(hover_points=lambda pts, pads, reach=6.0: set()),
    'every flown point keeps above the platform': dict(tracked_points=lambda boards: {k: bd['lift']['name'] for bd in boards
                                                                                      for k in range(bd['start'], bd['last'] + 1)}),
    'every turret counts, wherever it is': dict(turrets_to_clear=lambda objects, here, radius=45.0, dz=16.0:
                                                [o['name'] for o in objects if o['type'] == 'OBJ_TURRET']),
    'no flight lands on a lift': dict(plan_boardings=lambda pts, fly, lifts, standoff=8.0, base=None: []),
    'every lift is the nearest, however far': dict(boarding=lambda pts, fly, i, lifts, reach=9.0:
                                                    real['boarding'](pts, fly, i, lifts, reach=1e9)),
}
for name, patch in mutants.items():
    for n, v in patch.items():
        setattr(bot, n, v)
    bad = [label for label, ok in flight_rules(bot) if not ok]
    for n, v in real.items():
        setattr(bot, n, v)
    check(f'mutant caught: {name}', bool(bad))

print(f'{15 + len(results) + len(mutants)} passed, 0 failed')
