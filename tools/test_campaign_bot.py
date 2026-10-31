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


# A walkway beside a pit 5 units deep (level 2 room 4): Max before the jetpack keeps off its rim, Kurt (chute)
# and Max with the jetpack's wider limit (6) do not mind a 5 unit drop.
walkway = {(i, 0): 0.0 for i in range(12)}
walkway.update({(i, 1): 0.0 for i in range(12)})
walkway.update({(i, 2): -5.0 for i in range(12)})
rimmed = {}
for kind, (fly, chute) in {'walker': (False, False), 'flyer': (True, False), 'kurt': (False, True)}.items():
    pn = grid(walkway)
    pn.fly, pn.chute = fly, chute
    a_, b_ = pn.nearest((0, 2, 0)), pn.nearest((2, 2, 0))
    rimmed[kind] = pn.step_cost(a_, b_) / math.dist(pn.cells[a_][:3], pn.cells[b_][:3])
check('Max without the jetpack keeps off the rim of a 5 unit pit, the others do not',
      rimmed['walker'] == 20 and rimmed['flyer'] == 1 and rimmed['kurt'] == 1)
real_rim = bot.Nav.rim
bot.Nav.rim = lambda self, k: False
pn = grid(walkway)
mutant_cost = pn.step_cost(pn.nearest((0, 2, 0)), pn.nearest((2, 2, 0)))
bot.Nav.rim = real_rim
check('mutant caught: no rim is ever avoided', mutant_cost == math.dist(pn.cells[pn.nearest((0, 2, 0))][:3],
                                                                         pn.cells[pn.nearest((2, 2, 0))][:3]))

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
    gen = {'pos': (916.0, 940.0, 246.0)}
    out.append(('a generator 20 above the ledge is aimed at, one 37 units off is not',
                b.aimable(gen, (910.0, 945.0, 226.0)) and not b.aimable(gen, (916.0, 940.0, 209.0))))
    out.append(('a jetpack hero held up 6 below the point flies up; a walker, or one just arrived, does not',
                b.climb_out(6.2, 10.0, True) and not b.climb_out(6.2, 10.0, False) and not b.climb_out(6.2, 1.0, True)
                and not b.climb_out(2.0, 10.0, True)))
    out.append(('a squad passes only with every player near the leader',
                b.squad_ok(True, {2: 3.1}) and not b.squad_ok(True, {2: 334.8}) and not b.squad_ok(False, {2: 1.0})))
    lua = open(os.path.join(os.path.dirname(__file__), '..', 'lua', 'coop_dev.lua')).read()
    src = open(os.path.join(os.path.dirname(__file__), 'campaign_bot.py')).read()
    out.append(('a followed recording caps the gunner\'s fight (a generator spawns enemies without end)',
                'coop.gunner_cap=10000' in src and 'gn.max_fight and now - gn.fight_start > gn.max_fight' in lua))
    out.append(('a hero 80 units from the point is off the route, one 5 units away is not',
                b.off_recording((0.0, 30.0, 0.0), (4.6, 109.8, 3.7)) and not b.off_recording((0.0, 0.0, 0.0), (3.0, 4.0, 0.0))
                and b.off_recording((366.2, 931.6, -6.24), (368.1, 922.3, -0.08))))
    out.append(('the gunner is given turret names as a Lua table', b.lua_names(['a', 'b']) == '{"a", "b"}'))
    # the Lua gunner must list the targets it was given (a bay's turrets are not actors)
    out.append(('the gunner shoots named scripted objects (turrets)',
                'function coop.GunnerTargets' in lua and 'coop.GunnerPick(p, gn)' in lua))
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
    out.append(('an objective is shot at from the recording\'s spot, then walked up to, then from later spots',
                [b.aim_mode(n) for n in range(5)] == ['stay', 'walk', 'stay', 'stay', 'stay']
                and [b.aim_mode(n, False) for n in range(3)] == ['stay'] * 3))
    ft = [{'pos': (float(k), 0.0, z), 'keys': [57]} for k, z in enumerate([10, 12, 14, 13, 16, 20, 20, 18])]
    got = b.flight_targets(ft, 0, 8)
    out.append(('a climb holds the height of the next points, the last point is exact',
                [t[2] for t in got] == [16, 20, 20, 20, 20, 20, 18] and got[-1][3:] == (1.5, 2.0)
                and all(t[3:] == (3.0, 3.0) for t in got[:-1])))
    sk = [{'pos': (0.0, 0.0, 0.0), 'keys': [256] if k == 3 else []} for k in range(8)]
    out.append(('shots on skipped points are owed, others are not',
                b.skipped_shots(sk, 1, 5) and not b.skipped_shots(sk, 4, 8) and not b.skipped_shots(sk, 2, 3)))
    out.append(('the follower stops at the first shot fired near a shut gate, not past it',
                b.first_shot_at_gate(sk, 0, 8, [(0.0, 20.0, 0.0)]) == 3 and b.first_shot_at_gate(sk, 4, 8, [(0.0, 20.0, 0.0)]) is None
                and b.first_shot_at_gate(sk, 0, 8, [(0.0, 90.0, 0.0)]) is None and b.first_shot_at_gate(sk, 0, 8, []) is None))
    gl = [{'pos': (0.0, 0.0, 30.0 - 3.0 * k), 'keys': [57] if 2 <= k <= 6 else [], 'air': False} for k in range(10)]
    wk = [{'pos': (0.0, 0.0, 30.0 - 3.0 * k), 'keys': [], 'air': False} for k in range(10)]
    out.append(('a glide down with jump held is flown, to the landing; a walk down is not',
                b.flown(gl) == {2, 3, 4, 5, 6, 7} and not b.flown(wk)))
    # a flight goes to the pilot in one command, up to a lift's boarding point or a ride
    fp = [{'pos': (0.0, 0.0, 10.0 + k), 'keys': [57]} for k in range(8)]
    fp[5]['ride'] = True
    out.append(('a flight is batched up to a ride, a boarding point, a walked point and the limit',
                b.flight_batch(fp, set(range(8)), 0, set()) == 5
                and b.flight_batch(fp, set(range(8)), 0, {3}) == 3
                and b.flight_batch(fp, {0, 1, 2}, 0, set()) == 3
                and b.flight_batch(fp, set(range(5)), 0, set(), limit=2) == 2
                and b.flight_batch(fp, set(range(8)), 6, set()) == 8))
    return out


results = flight_rules(bot)
for label, ok in results:
    check(label, ok)

# mutants: each must make at least one check fail
import types
real = {n: getattr(bot, n) for n in ('flown', 'refuel_mode', 'needs_fuel', 'stretch_start', 'LIFT_SPEED', 'lift_ready',
                                    'boarding', 'plan_boardings', 'nearest_ahead', 'hover_points', 'tracked_points', 'turrets_to_clear', 'aimable', 'climb_out', 'squad_ok', 'off_recording', 'flight_batch', 'aim_mode', 'flight_targets', 'skipped_shots', 'first_shot_at_gate')}
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
    'aim only at objectives on the hero\'s own floor': dict(aimable=lambda o, here, reach=30.0: math.dist(o['pos'], here) < reach and abs(o['pos'][2] - here[2]) < 12),
    'a hero in a pit is never flown out': dict(climb_out=lambda rise, stuck, jetpack, wall=4.0, after=8.0: False),
    'a squad passes when only the leader got through': dict(squad_ok=lambda ok, gaps, limit=40.0: bool(ok)),
    'no hero is ever off the recording': dict(off_recording=lambda here, target, limit=15.0, below=4.0: False),
    'a recorded shot is always walked up to': dict(aim_mode=lambda tries, walk_ok=True: 'walk'),
    'a recorded shot is never walked up to': dict(aim_mode=lambda tries, walk_ok=True: 'stay'),
    'a glide is walked': dict(flown=lambda pts: {k for k, p in enumerate(pts) if p.get('air')}),
    'a climb stops and goes at every point': dict(flight_targets=lambda pts, i, k, first=None, lead=3: [(*pts[m]['pos'], 3.0, 3.0) for m in range(i + 1, k - 1)] + [(*pts[k - 1]['pos'], 1.5, 2.0)]),
    'skipped shots are forgotten': dict(skipped_shots=lambda pts, i, j: False),
    'shots at a gate are skipped over': dict(first_shot_at_gate=lambda pts, i, j, gates, reach=45.0: None),
    'a flight is sent point by point': dict(flight_batch=lambda pts, fly, i, apart, limit=14: i + 1),
    'a batch runs through boarding points and rides': dict(flight_batch=lambda pts, fly, i, apart, limit=14:
                                                           max([k for k in range(i + 1, len(pts) + 1) if all(m in fly for m in range(i, k))] or [i + 1])),
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

# Gating objectives (level 2 room 2): the pods that open the exit wall come before the alarms that open nothing.
gl = bot.Level.__new__(bot.Level)
gl.m = {'objects': [{'name': 'alarm', 'type': 'OBJ_DESTRUCTABLE', 'pos': [5, 0, 0]},
                    {'name': 'pod', 'type': 'OBJ_DESTRUCTABLE', 'pos': [40, 0, 0]},
                    {'name': 'gen', 'type': 'OBJ_DOGANBOYGENERATOR', 'pos': [60, 0, 0]}],
        'events': {'alarm': ['OnDie'], 'pod': ['OnDie']}, 'pickups': [],
        'deps': {'pod': {'OnDie': {'opens': ['wall'], 'enables': [], 'damage': []}}, 'alarm': {'OnDie': {'opens': [], 'enables': [], 'damage': []}}}}
order = [o['name'] for o in bot.objectives(gl, [0, 0, 0])]
check('objectives that open something come before those that open nothing',
      order.index('pod') < order.index('alarm') and bot.gating(gl.m, gl.m['objects'][1]) and bot.gating(gl.m, gl.m['objects'][2])
      and not bot.gating(gl.m, gl.m['objects'][0]))
real_opens = bot.opens_something
bot.opens_something = lambda model, o: False
flat_order = [o['name'] for o in bot.objectives(gl, [0, 0, 0])]
bot.opens_something = real_opens
check('mutant caught: nothing is gating (the alarm comes first)', flat_order.index('alarm') < flat_order.index('pod'))

# Objectives: a pickup with an OnDie handler (level 3's BFB) is not something to shoot at.
lv = bot.Level.__new__(bot.Level)
lv.m = {'objects': [{'name': 'bfb', 'type': 'OBJ_BFB', 'pos': [5, 0, 0]},
                    {'name': 'gen', 'type': 'OBJ_DOGANBOYGENERATOR', 'pos': [30, 0, 0]},
                    {'name': 'wall', 'type': 'OBJ_DESTRUCTABLE', 'pos': [10, 0, 0]}],
        'events': {'bfb': ['OnDie'], 'wall': ['OnDie']}, 'pickups': [{'name': 'bfb', 'type': 'OBJ_BFB'}]}
check('a pickup is never an objective, the wall and the generator are',
      [o['name'] for o in bot.objectives(lv, [0, 0, 0])] == ['wall', 'gen'])
# Off the route: the level carried the hero away (level 3's start) against a walk along the route.
check('a hero carried into a room off the route is replanned',
      bot.off_route(level, [{'room': 'b'}, {'room': 'c'}], [2, 5, 1], [60, 50, 1]))
check('a hero who walked along the route is not',
      not bot.off_route(level, [{'room': 'b'}, {'room': 'c'}], [2, 5, 1], [28, 5, 1])
      and not bot.off_route(level, [{'room': 'b'}], [2, 5, 1], [8, 5, 1]))
# Kurt's fans: the one under the way up is chosen, not one off to the side or above the target.
lv.m['objects'] = [{'name': 'fan_far', 'type': 'OBJ_BLOWERCYLINDER', 'pos': [300, 0, 0]},
                   {'name': 'fan_above', 'type': 'OBJ_BLOWERCYLINDER', 'pos': [10, 0, 90]},
                   {'name': 'fan_side', 'type': 'OBJ_BLOWERCYLINDER', 'pos': [40, 30, 0]},
                   {'name': 'fan_under', 'type': 'OBJ_BLOWERCYLINDER', 'pos': [10, -25, -90]}]
check('a fan below the target and near both ends is ridden, nearest the target first',
      [o['name'] for o in bot.fans_toward(lv, [0, 0, 0], [10, -30, 60])] == ['fan_under', 'fan_side'])
# Script chains: a sniper lock that raises a platform near the way on is an opener to set off; one
# already set off, or a handler the level runs by itself, is not.
lv.rooms = {}
lv.m['objects'] = [{'name': 'lock0', 'type': 'OBJ_SNIPERDOOR1', 'pos': [20, 0, 0]},
                   {'name': 'plat0', 'type': 'OBJ_SCENERY', 'pos': [30, 0, 0]},
                   {'name': 'lock1', 'type': 'OBJ_SNIPERDOOR1', 'pos': [5, 0, 0]},
                   {'name': 'plat1', 'type': 'OBJ_SCENERY', 'pos': [40, 0, 0]},
                   {'name': 'far', 'type': 'OBJ_SNIPERDOOR1', 'pos': [500, 0, 0]},
                   {'name': 'door', 'type': 'OBJ_PROXDOOR1', 'pos': [600, 0, 0]}]
sn = {'opens': [], 'enables': [], 'damage': ['DAMAGE_SNIPER']}
lv.m['deps'] = {'lock0': {'OnDamage': dict(sn, enables=['plat0'])}, 'lock1': {'OnDamage': dict(sn, enables=['plat1'])},
                'far': {'OnDamage': dict(sn, opens=['door'])}, 'plat0': {'OnCreate': dict(sn, enables=['plat1'])}}
check('sniper locks that switch on platforms near the way are set off, nearest first, once',
      [o for o, _, _ in bot.chain_openers(lv, [0, 0, 0], [50, 0, 0])] == ['lock1', 'lock0']
      and [o for o, _, _ in bot.chain_openers(lv, [0, 0, 0], [50, 0, 0], {'lock1'})] == ['lock0'])
# Kurt's blind drops: a plateau 11 by 11 cells; the landing far below is not scanned. The rims are its
# edge cells (never the middle, even when the target is straight under it), nearest the target first.
pl = grid({(i, j): 60.0 for i in range(11) for j in range(11)})
rims = bot.drop_rims(pl, [10, 10, 60], [40, 10, -73])
check('blind drops start at the plateau edge nearest the target, a few cells apart',
      rims and abs(rims[0][0] - 20.0) < 1e-6 and all(r[0] in (0.0, 20.0) or r[1] in (0.0, 20.0) for r in rims)
      and all(math.dist(a[:2], b[:2]) >= 12 for a in rims for b in rims if a is not b))
under = bot.drop_rims(pl, [10, 10, 60], [10, 10, -73])
check('a target straight under the plateau drops off its edge, not from the middle',
      under and all(r[0] in (0.0, 20.0) or r[1] in (0.0, 20.0) for r in under))
lv4file = Path(__file__).resolve().parents[1] / 'local' / 'campaign' / 'level4.json'
if lv4file.exists():
    lv4 = bot.Level(lv4file)
    below = [1137.0, -348.0, -102.0]
    check('a hero below every room box is planned from the nearest room, not a crash',
          lv4.room_of(below) is None and lv4.path(below, [1117.0, -545.0, 252.0])[1])
# sniper locks nothing in the model reacts to (the vortex room's spheres), near the hero, once each
lv.m['objects'] = [{'name': 'ball0', 'type': 'OBJ_SNIPERDOOR1', 'pos': [0, 0, -40]},
                   {'name': 'ball1', 'type': 'OBJ_SNIPERDOOR1', 'pos': [30, 0, -40]},
                   {'name': 'lockA', 'type': 'OBJ_SNIPERDOOR1', 'pos': [5, 0, -40]},
                   {'name': 'farball', 'type': 'OBJ_SNIPERDOOR1', 'pos': [500, 0, -40]}]
lv.m['deps'] = {'lockA': {'OnDamage': {'opens': ['door'], 'enables': [], 'damage': ['DAMAGE_SNIPER']}}}
check('script-watched sniper locks near the hero are listed nearest first, not the ones with a chain or far off',
      [o['name'] for o in bot.sniper_locks(lv, [0, 0, 0])] == ['ball0', 'ball1']
      and [o['name'] for o in bot.sniper_locks(lv, [0, 0, 0], done={'ball0'})] == ['ball1'])
# a tunnel's mouth: a 1.7 step onto a slope that leads on to level ground (level 4 room 1's tunnel was
# the way to room 2 and the planner called it unreachable); a plain 1.7 step onto a slope with no level
# ground beyond it, and a 3 unit rise onto a slope, are still not walked
mouth = grid({(0, 0): 58.9, (1, 0): 58.9, (2, 0): 60.6, (3, 0): 61.7, (4, 0): 61.7, (5, 0): 61.7,
              (0, 1): 58.9, (1, 1): 58.9, (2, 1): 60.8, (3, 1): 61.9, (4, 1): 61.7, (5, 1): 61.7})
check('a curb onto a slope that leads to level ground is a jump', mouth.path([0, 0, 58.9], [10, 0, 61.7]) is not None)
ramp = grid({(i, 0): 58.9 + 1.7 * max(0, i - 1) for i in range(6)} | {(i, 1): 58.9 + 1.7 * max(0, i - 1) for i in range(6)})
check('a steady 40 degree slope is not walked', ramp.path([0, 0, 58.9], [10, 0, 66.0]) is None)
# the game moves note_<key>.tmp over note_<key>.txt; the move fails while the bot has the .txt open, and
# the newer text then stays in the .tmp (a scan 'done' that never showed: a 15 minute wait)
import os
import tempfile
with tempfile.TemporaryDirectory() as td:
    txt, tmp = Path(td) / 'note_nav_state.txt', Path(td) / 'note_nav_state.tmp'
    check('a note not written yet reads as None', bot.read_note(txt) is None)
    txt.write_text('running')
    check('a note is its .txt', bot.read_note(txt) == 'running')
    tmp.write_text('done 22849 115')
    os.utime(txt, ns=(1, 1))
    check('a newer .tmp left by a failed move is the note', bot.read_note(txt) == 'done 22849 115')
    os.utime(tmp, ns=(1, 1))
    os.utime(txt, ns=(2, 2))
    check('an older .tmp is not', bot.read_note(txt) == 'running')
# Co-op (tools/campaign_coop.py): the world comparison's tolerance table, lives, revives and roles.
import campaign_coop as cc
report = """warn  enemy 1f l2_r6gen00 (type 511) not bound on the client yet
FAIL  enemy 2a l2_r4manta01 is 131.2 apart on the client
FAIL  enemy 2b l2_r5turret03 (type 220, hp 40) deleted on the client
state: 2 fail, 1 warn, 30 enemies compared"""
c = cc.classify(report)
check('a flying enemy apart is known, an enemy deleted on the client only is a finding',
      [k[0][:30] for k in c['known']] == ['FAIL  enemy 2a l2_r4manta01 is'] and len(c['findings']) == 1
      and 'deleted on the client' in c['findings'][0] and len(c['warn']) == 1 and c['summary'].startswith('state:'))
check('a ground enemy apart is a finding, not known drift',
      len(cc.classify('FAIL  enemy 3 l2_r5grunt is 40.0 apart on the client')['findings']) == 1)
lives = cc.Lives([1, 2])
for slot, life in ((2, 1), (2, 0), (2, 1), (2, 2), (2, 3), (2, 0), (1, 1), (1, 1)):
    lives.update(slot, life)
t = lives.totals()
check('downs, revives, deaths and returns are counted from life readings',
      (t['downs'], t['revives'], t['deaths'], t['returns']) == (3, 1, 1, 1) and t['per_player'][1]['downs'] == 1)
check('the nearest standing teammate revives; nobody when all are down',
      cc.reviver({1: (0, 0, 0), 2: (5, 0, 0), 3: (50, 0, 0)}, {1: 1, 2: 0, 3: 0}, 1) == 2
      and cc.reviver({1: (0, 0, 0), 2: (5, 0, 0)}, {1: 1, 2: 1}, 1) is None
      and cc.reviver({1: (0, 0, 0), 2: (5, 0, 0), 3: (1, 0, 0)}, {1: 1, 2: 0, 3: 3}, 1) == 2)
picks = [{'name': 'a', 'type': 'OBJ_DOUBLEA', 'pos': [10, 0, 0]}, {'name': 'u', 'type': 'OBJ_UZI', 'pos': [1, 0, 0]},
         {'name': 'b', 'type': 'OBJ_CARBATTERY', 'pos': [30, 0, 0]}]
check('low health goes for the nearest health pickup not tried yet, within reach',
      cc.health_target(picks, (0, 0, 0), set())['name'] == 'a' and cc.health_target(picks, (0, 0, 0), {'a'})['name'] == 'b'
      and cc.health_target(picks, (0, 0, 0), {'a'}, radius=20) is None)
gens = [{'name': 'g2'}, {'name': 'g1'}]
check('roles: the second player takes the objective after the leader\'s, the rest none',
      cc.split(gens, 2) == {2: gens[1]} and cc.split(gens, 4) == {2: gens[1]} and cc.split(gens[:1], 3) == {})
check('regrouped only when everyone is at the portal',
      cc.regrouped({1: (0, 0, 0), 2: (10, 0, 0)}, (0, 0, 0)) and not cc.regrouped({1: (0, 0, 0), 2: (20, 0, 0)}, (0, 0, 0)))

t = cc.Lives([1, 2])
t.update(2, cc.DOWNED)
check('revive mutant: down and not revived is not ok', not cc.down_check(t.totals(), 2)['ok'])
t.update(2, cc.ALIVE)
check('revive mutant: down then revived is ok', cc.down_check(t.totals(), 2)['ok'])
t = cc.Lives([1, 2])
t.update(2, cc.DOWNED)
t.update(2, cc.DEAD)
check('revive mutant: down then dead is reported', cc.down_check(t.totals(), 2)['died']
      and not cc.down_check(t.totals(), 2)['ok'] and not cc.down_check(cc.Lives([1, 2]).totals(), 2)['down_seen'])

bl = cc.bridge_lives(['life: alive -> downed (lethal hit)\nlife: downed -> alive (revived by player 2)\n',
                      'life: alive -> downed (lethal hit)\nlife: alive (new level)\nlife: alive (new level)\n'])
check('the mod\'s life log tells a revive by a teammate from a wipe reload',
      bl == {'downs': 2, 'revived_by_teammate': 1, 'new_level': 2, 'bled_out': 0})

# Crash detection in campaign_run.py: a new dump in a watched copy's crash folder fails the segment.
import os, subprocess, tempfile
import campaign_run as cr
with tempfile.TemporaryDirectory() as tmp:
    game = Path(tmp) / 'game'
    crash = game / 'profile' / 'MDK2HD' / 'crash'
    crash.mkdir(parents=True)
    (crash / 'dump0000.dmp').write_bytes(b'old')
    state = str(Path(tmp) / 'state.json')
    subprocess.run([sys.executable, str(Path(__file__).with_name('crashwatch.py')), 'begin', '--state', state,
                    '--crash-dir', str(crash), '--dir', str(game)], check=True)
    env = dict(os.environ, MDK2_CRASH_STATE=state, MDK2_CRASH_DIALOGS='0')
    check('no crash, no CRASH line', not [l for l in cr.crash_lines(env) if l.startswith('CRASH')])
    (crash / 'dump0001.dmp').write_bytes(b'MDMP not really')
    lines = [l for l in cr.crash_lines(env) if l.startswith('CRASH')]
    check('a new dump%04i.dmp of a copy is a CRASH with its path', len(lines) == 1 and 'dump0001.dmp' in lines[0])
# World comparison with a second host dump (tools/statecheck.py --after): dumps are taken one game after another, so a
# fight or a spawn moves the host's world between the host's and a client's. Evidence: the campaign survey's level 2
# runs showed the client lower on hp (the client dumped after more hits), an enemy "extra" on the client (the host made
# it after its dump) and one the host had removed (its DESPAWN still on the way). Real mismatches must still fail.
HEADER = 'role {role}\nlevel 2\nme {me}\nlife 0\n'


def dump(role, ents, me=None):
    text = HEADER.format(role=role, me=me or (1 if role == 'host' else 2))
    # one hero each, far apart, and the other's puppet where that hero stands
    text += 'player 1 100 0 0 0\npuppet 2 500 0 0\n' if role == 'host' else 'player 1 100 500 0 0\npuppet 1 0 0 0\n'
    for nid, spec in ents.items():
        text += f'ent {nid} {spec}\n' if spec == 'gone' else 'ent {} {} {} {} {} {} {}\n'.format(nid, *spec)
    return text


def statecheck(host, client, post=None):
    with tempfile.TemporaryDirectory() as t:
        names = []
        for i, text in enumerate((host, client) + ((post,) if post else ())):
            f = Path(t) / f'd{i}.txt'
            f.write_text(text)
            names.append(str(f))
        args = names[:2] + (['--after', names[2]] if post else [])
        r = subprocess.run([sys.executable, str(Path(__file__).with_name('statecheck.py')), *args], capture_output=True, text=True)
        return r.returncode, r.stdout


sam = lambda hp, x=0.0: (201, hp, x, 0.0, 0.0, 'sam')
rc, out = statecheck(dump('host', {'a': sam(14)}), dump('client', {'a': sam(0)}))
check('statecheck without --after: client hp 0 against 14 fails', rc == 1 and 'hp 14 on host, 0 on client' in out)
rc, out = statecheck(dump('host', {'a': sam(14)}), dump('client', {'a': sam(0)}), dump('host', {'a': sam(0)}))
check('statecheck --after: client hp the host reached in between passes', rc == 0 and 'went on to 0' in out)
rc, out = statecheck(dump('host', {'a': sam(14)}), dump('client', {'a': sam(30)}), dump('host', {'a': sam(0)}))
check('statecheck --after: client hp above what the host ever had fails', rc == 1)
rc, out = statecheck(dump('host', {'a': sam(60)}), dump('client', {'a': sam(0)}), dump('host', {'a': sam(60)}))
check('statecheck --after: a client stuck at 0 while the host stays at 60 fails (the silent refill)', rc == 1)
rc, out = statecheck(dump('host', {}), dump('client', {'b': sam(50)}), dump('host', {'b': sam(50)}))
check('statecheck --after: an enemy the host made after its first dump passes', rc == 0 and 'made it after' in out)
rc, out = statecheck(dump('host', {}), dump('client', {'b': sam(50)}), dump('host', {}))
check('statecheck --after: an enemy the host never had still fails', rc == 1)
rc, out = statecheck(dump('host', {'c': 'gone'}), dump('client', {'c': sam(0)}), dump('host', {}))
check('statecheck --after: a removed enemy whose DESPAWN is on the way passes', rc == 0 and 'DESPAWN is on the way' in out)
rc, out = statecheck(dump('host', {'c': 'gone'}), dump('client', {'c': sam(0)}), dump('host', {'c': 'gone'}))
check('statecheck --after: an enemy the host keeps "gone" without removing it fails', rc == 1)
rc, out = statecheck(dump('host', {'m': sam(40, 0.0)}), dump('client', {'m': sam(40, 9.0)}), dump('host', {'m': sam(40, 10.0)}))
check('statecheck --after: a mover on the host\'s path between the dumps passes', rc == 0 and 'apart' not in out)
rc, out = statecheck(dump('host', {'m': sam(40, 0.0)}), dump('client', {'m': sam(40, 60.0)}), dump('host', {'m': sam(40, 10.0)}))
check('statecheck --after: a mover off the host\'s path still fails', rc == 1)
print('all checks passed')
