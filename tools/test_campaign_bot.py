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
print('8 passed, 0 failed')
