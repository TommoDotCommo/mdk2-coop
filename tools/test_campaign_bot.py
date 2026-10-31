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
    return out


results = flight_rules(bot)
for label, ok in results:
    check(label, ok)

# mutants: each must make at least one check fail
import types
real = {n: getattr(bot, n) for n in ('flown', 'refuel_mode', 'needs_fuel', 'stretch_start', 'LIFT_SPEED')}
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
}
for name, patch in mutants.items():
    for n, v in patch.items():
        setattr(bot, n, v)
    bad = [label for label, ok in flight_rules(bot) if not ok]
    for n, v in real.items():
        setattr(bot, n, v)
    check(f'mutant caught: {name}', bool(bad))

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
# elevators (mined `lifts`): a hero at one end whose goal is at the other end's height, near the cab, rides;
# a goal on the hero's own height, in another column or at neither end's height does not
lv.m['lifts'] = [{'top': {'name': 'doorT', 'pos': [-28, 38.5, 0.0]}, 'bottom': {'name': 'doorB', 'pos': [-28, 29.7, -90.0]},
                  'mover': 'lift0', 'cab': {'name': 'cab0', 'pos': [-28, 34.1, -90.0]}, 'locked': True}]
up = lv.lift_toward([-28, 45, 0.1], [-28, 21, -91])
check('a goal 90 down is reached by the lift from its top door',
      up and up[0]['mover'] == 'lift0' and up[1]['name'] == 'doorT' and up[2]['name'] == 'doorB')
down = lv.lift_toward([-28, 25, -90.0], [-28, 40, 0.0])
check('the same lift is taken back up from its bottom door', down and down[1]['name'] == 'doorB')
check('a goal on the hero\'s height needs no lift', lv.lift_toward([-28, 45, 0.1], [-60, 45, 0.1]) is None)
check('a goal at neither end\'s height needs no lift', lv.lift_toward([-28, 45, 0.1], [-28, 21, -45]) is None)
check('a goal far from the cab\'s column needs no lift', lv.lift_toward([-28, 45, 0.1], [300, 21, -91]) is None)
check('a hero not at an end\'s height does not use the lift', lv.lift_toward([-28, 45, -40], [-28, 21, -91]) is None)
check('a hero a floor below the lift\'s bottom door still heads for it', lv.lift_toward([-28, 20, -108], [-28, 40, 0.0]) is not None)
# sniper aim through the camera: forward from the quaternion (the values the game gave: facing +x is
# w 0.707, z -0.707), the turn needed to face a target, the stick pulse that makes it
fx = bot.cam_forward((0.7071, 0, 0, -0.7071))
check('quaternion for facing +x points along +x', abs(fx[0] - 1) < 1e-3 and abs(fx[1]) < 1e-3 and abs(fx[2]) < 1e-3)
up = bot.cam_forward((0.9659, 0.2588, 0, 0))
check('a positive x component pitches the camera up 30 degrees', abs(up[2] - 0.5) < 1e-3 and up[1] > 0.8)
yaw, pitch = bot.aim_error((0, 1, 0), (-1, 1, 0))
check('a target to the left of +y is a positive (left) yaw of 45', abs(yaw - 45) < 1e-6 and abs(pitch) < 1e-6)
yaw, pitch = bot.aim_error((1, 0, 0), (1, 0, -0.5))
check('a target below needs a negative pitch', abs(yaw) < 1e-6 and -30 < pitch < -20)
yaw, _ = bot.aim_error((-1, 0.01, 0), (-1, -0.01, 0))
check('yaw wraps around 180 the short way', abs(yaw) < 2)
check('inside the deadband no pulse', bot.look_pulse(0.5) == (0.0, 0))
st, n = bot.look_pulse(-30)
check('a big turn right is a full stick for a few frames', st == -1.0 and n == round(30 / 2.83))
st, n = bot.look_pulse(6)
check('a small turn left is a half stick for err/rate frames', st == 0.5 and n == round(6 / 0.43))
st, n = bot.look_pulse(5, pitch=True)
check('a pitch of 5 degrees is the 0.7 stick', st == 0.7 and n == round(5 / 0.70))
st, n = bot.look_pulse(-1.0, pitch=True)
check('a pitch of 1 degree is the smallest stick for a frame or more', st == -0.5 and n >= 1)
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
# Doc's hands: the hand/scroll protocol against a model of the lists seen live (level 3 checkpoint 6: left
# atomic toaster, tape, empty; right pipes, empty, loaf; the selection does not wrap)
import re


class DocHands:
    OBJ = {'OBJ_ATOMICTOASTER': 344, 'OBJ_DUCTTAPE': 332, 'OBJ_PIPES': 351, 'OBJ_LOAF': 328, 'OBJ_NONE': 0}

    def __init__(self, lose=0):
        self.lists = {0: [344, 332, 0], 1: [351, 0, 328]}
        self.cur = {0: 0, 1: 0}
        self.held = {0: 0, 1: 0}
        self.pressed = []
        self.lose = lose          # every lose-th press is lost (a press by time)

    def lua(self, code):
        n, frames = (int(v) for v in re.search(r'coop_press_frames\((\d+), (\d+)\)', code).groups())
        self.pressed.append((n, frames))
        if self.lose and len(self.pressed) % self.lose == 0:
            return
        if n in bot.DOC_SCROLL:
            for h in (0, 1):
                self.cur[h] = min(2, max(0, self.cur[h] + (1 if n == bot.DOC_SCROLL[0] else -1)))
        for h, k in bot.DOC_HAND_KEY.items():
            if n == k:
                self.held[h] = self.lists[h][self.cur[h]]

    def note(self, key, code):
        hand = int(re.search(r'GetHeldItem\(mdkGetPlayerGob\(\), (\d)\)', code).group(1))
        if key == 'held':
            return str(self.held[hand])
        return '1' if self.held[hand] == self.OBJ[re.search(r'== (\w+)\)', code).group(1)] else 'nil'


orig_sleep = bot.time.sleep
bot.time.sleep = lambda s: None
try:
    d = DocHands()
    check('Doc: the tape goes to the left hand', bot.doc_hold(d, 0, 'OBJ_DUCTTAPE') and d.held[0] == 332)
    check('Doc: the loaf goes to the right hand, the selection turning round at the list end',
          bot.doc_hold(d, 1, 'OBJ_LOAF') and d.held[1] == 328 and bot.doc_hold(d, 1, 'OBJ_PIPES') and d.held[1] == 351)
    check('Doc: a hand is emptied', bot.doc_hold(d, 1, 'OBJ_NONE') and d.held[1] == 0)
    check('Doc: a held item needs no press', not (lambda n: (bot.doc_hold(d, 0, 'OBJ_DUCTTAPE'), len(d.pressed) - n)[1])(len(d.pressed)))
    check('Doc: every press is held for whole frames', all(f >= 2 for _, f in d.pressed))
    d = DocHands()
    check('Doc: an item in each hand, then fire (a combine)',
          bot.doc_use(d, 'OBJ_ATOMICTOASTER', 'OBJ_LOAF') and d.held == {0: 344, 1: 328} and d.pressed[-1][0] == bot.FIRE_BUTTON)
    d = DocHands()
    check('Doc: one item alone is used by its hand key pressed again',
          bot.doc_use(d, 'OBJ_DUCTTAPE') and d.held == {0: 332, 1: 0} and d.pressed[-1][0] == bot.DOC_HAND_KEY[0])
    d = DocHands(lose=3)
    check('Doc: with every third press lost the item is still found (the mutant that loses presses)',
          bot.doc_hold(d, 1, 'OBJ_LOAF', tries=14))
    d = DocHands(lose=1)
    check('Doc: with every press lost the hand fails, and says so', not bot.doc_hold(d, 0, 'OBJ_DUCTTAPE', tries=6))
finally:
    bot.time.sleep = orig_sleep

print('all checks passed')
