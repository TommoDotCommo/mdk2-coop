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
print('29 passed, 0 failed')
