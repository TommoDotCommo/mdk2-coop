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

# No-God survival: threats read from the host's state, the focus, cover places.
rows = [l.split() for l in """player 101 76 92.77 474.60 -27.07
puppet 2 91.07 493.09 -26.24
ent 10042 230 150 94.32 590.45 30.60 l2r3turret02
ent 10043 230 24 91.25 488.57 18.71 l2r3turret03
ent 30041 201 30 95 480 -20 l2r3samspawn02_s
ent 1001 511 100 60 400 30 l2r3samspawn01
ent 10050 230 0 90 500 20 deadturret
ai_target 10042 230 @p2
ai_target 10043 230 @p1
ai_target 30041 201 @p1
ai_target 1001 511 @p1
ai_target 10050 230 @p1
ai_target 10043 230 none""".splitlines()]
ht = cc.host_threats(rows)
check('threats: enemies whose AI target is a player, by player; generators and the dead are not threats',
      sorted(t['name'] for t in ht['threats'][1]) == ['l2r3samspawn02_s', 'l2r3turret03']
      and [t['name'] for t in ht['threats'][2]] == ['l2r3turret02'] and ht['players']['me'][1] == 474.6
      and ht['players'][2][1] == 493.09)
check('focus: a missile near comes first, else the weakest within reach, nothing out of reach',
      cc.pick_focus(ht['threats'][1], ht['players']['me'])['name'] == 'l2r3samspawn02_s'
      and cc.pick_focus(ht['threats'][1][:1], ht['players']['me'])['name'] == 'l2r3turret03'
      and cc.pick_focus(ht['threats'][2], ht['players'][2], reach=50) is None
      and cc.pick_focus([dict(ht['threats'][1][0], type=201, pos=(92.8, 540.0, -27.0))], ht['players']['me']) is None)
cand = cc.cover_candidates((0, 0, 0), [(30, 0, 0), (1, 0, 0)])
check('cover places: nearest first, the hero\'s own spot and trail points on top of it left out',
      cand[0] == (cand[0][0], cand[0][1], 0) and math.dist(cand[0], (0, 0, 0)) <= 5.0 + 1e-6
      and (30, 0, 0) in cand and (1, 0, 0) not in cand and len(cand) == 33
      and all(math.dist(a, (0, 0, 0)) <= math.dist(b, (0, 0, 0)) for a, b in zip(cand, cand[1:])))

# Transient world differences: tolerated only when the next comparison no longer has them.
def stp(*lines):
    return {'findings': list(lines), 'known': [], 'warn': []}
hpl = 'FAIL  enemy 10042 l2r3turret02: hp 54 on host, 48 on client'
cl = 'FAIL  enemy 1e003d n1e003d (type 201) on the client 2, gone on the host'
gone = 'FAIL  enemy 20003 sod101 (type 230, hp 150) deleted on the client'
sts = cc.settle([stp(hpl, cl, gone), stp(gone), stp(hpl)])
check('transient: health and client-only lines gone at the next step are known, the rest stay findings',
      sts[0]['findings'] == [gone] and len(sts[0]['known']) == 2 and sts[0]['settled'] == 2
      and sts[0]['known'][0][1].startswith('transient'))
check('transient: the same enemy still different at the next step is a finding; the last step stays unsettled',
      cc.settle([stp(hpl), stp(hpl)])[0]['findings'] == [hpl] and sts[2]['findings'] == [hpl])
check('transient: a deleted-on-the-client enemy (the desync mutant) is never tolerated',
      cc.settle([stp(gone), stp()])[0]['findings'] == [gone])

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
print('34 passed, 0 failed')
