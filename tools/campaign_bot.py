#!/usr/bin/env python3
"""Campaign bot: plays MDK2 levels with ordinary controls in N co-op game copies.

Each game runs the in-game pilot (coop.PilotGo in lua/coop_dev.lua): the hero faces a point and
walks there with the forward key; the game's physics and scripts do the rest. This driver issues
steps from a level plan (tools/campaign_plans/, built with tools/campaign_mine.py from the game's
own level scripts), waits for each step's result in the games' own state, and keeps evidence.

    campaign_bot.py goto --dirs D1 D2 [--player 1] --to X Y Z  walk one hero to a point
    campaign_bot.py plan --dirs D1 D2 PLAN.json                run a level plan
"""
import argparse
import json
import math
import sys
import time
from pathlib import Path


class Game:
    """One running copy, driven through its coop folder (command.lua in, files out)."""

    def __init__(self, folder, slot):
        import threading
        self.coop = Path(folder) / 'coop'
        self.slot = slot
        self.serial = 0
        self.gun_range = 60            # how far the gunner shoots (a squad's tactic may shorten it)
        # one command.lua per game: threads (a squad's leader and its followers) take turns
        self.lock = threading.RLock()

    def lua(self, code, timeout=12):
        with self.lock:
            self._lua(code, timeout)

    def _lua(self, code, timeout):
        target = self.coop / 'command.lua'
        stage = target.with_suffix('.tmp')
        stage.write_text(code + '\n')
        stage.replace(target)
        deadline = time.monotonic() + timeout
        while target.exists():
            if time.monotonic() > deadline:
                raise TimeoutError(f'game {self.slot}: command not taken: {code[:80]}')
            time.sleep(.03)

    def note(self, key, code, timeout=5):
        """Run code that ends in coop_note(key, text); return the text."""
        with self.lock:
            path = self.coop / f'note_{key}.txt'
            path.unlink(missing_ok=True)
            self._lua(code, 12)
            deadline = time.monotonic() + timeout
            while not path.exists():
                if time.monotonic() > deadline:
                    raise TimeoutError(f'game {self.slot}: no note {key} (Lua error? see bridge.log)')
                time.sleep(.03)
            return path.read_text()

    def state(self):
        self.serial += 1
        tag = f'bot_{self.serial}'
        path = self.coop / f'state_{tag}.txt'
        self.lua(f'coop_state("{tag}")')
        deadline = time.monotonic() + 5
        while not path.exists():
            if time.monotonic() > deadline:
                raise TimeoutError(f'game {self.slot}: no state dump')
            time.sleep(.03)
        rows = [line.split() for line in path.read_text().splitlines()]
        path.unlink(missing_ok=True)
        return rows

    def pilot(self):
        text = self.note('pilot', 'local pl=coop.pilot local p=coop.real_player() '
                         'if (pl and p) then coop_note("pilot", format("%s %.2f %d %d %.2f %.2f %.2f", pl.state, pl.d or -1, '
                         'pl.stuck, coop_ms()-pl.t0, p.position.x, p.position.y, p.position.z)) '
                         'else coop_note("pilot", "none") end')
        f = text.split()
        if f[0] == 'none':
            return None
        return {'state': f[0], 'd': float(f[1]), 'stuck': int(f[2]), 'ms': int(f[3]),
                'pos': tuple(float(v) for v in f[4:7])}

    def pos(self):
        t = self.note('pos', 'local p=coop.real_player() if (p) then coop_note("pos", format("%.2f %.2f %.2f", '
                      'p.position.x, p.position.y, p.position.z)) else coop_note("pos", "none") end')
        if t.startswith('none'):
            return None
        return tuple(float(v) for v in t.split())

    def telemetry(self):
        """What the bot did and how the hero is, from this game's own state."""
        t = self.note('tele', 'local p=coop.real_player() local gn=coop.gunner or {} '
                      'coop_note("tele", format("%d %d %d %d %d %d", coop.level or -1, coop.cp or -1, '
                      'p and (coop_hp(p) or -1) or -1, gn.shots or 0, gn.engaged or 0, coop_life(coop_slot())))')
        f = [int(v) for v in t.split()]
        return dict(zip(('level', 'cp', 'hp', 'shots', 'targets', 'life'), f))

    def goto(self, x, y, z, radius=2.5, timeout=60, trail=None, explore=1, jump=0, fly=0, chute=0, flat=1):
        self.lua(f'coop.PilotGo({x},{y},{z},{radius},{flat},{explore}) coop.pilot.max_stuck=40 coop.pilot.jump={jump} '
                 f'coop.pilot.fly={fly} coop.pilot.chute={chute}')
        deadline = time.monotonic() + timeout
        while True:
            p = self.pilot()
            if trail is not None and p:
                trail.append((time.monotonic(), self.slot, p['pos'], p['d']))
            if p and p['state'] != 'go':
                return p
            if time.monotonic() > deadline:
                self.lua('coop.PilotStop()')
                p = self.pilot() or {}
                p['state'] = 'timeout'
                return p
            time.sleep(.25)


class Level:
    """A mined level model (tools/campaign_mine.py) with a room graph for paths."""

    def __init__(self, path):
        self.m = json.loads(Path(path).read_text())
        self.rooms = self.m['rooms']
        self.edges = {}
        for a, b, point, how in self.m['portals']:
            self.edges.setdefault(a, []).append((b, point, how))
            self.edges.setdefault(b, []).append((a, point, how))
        # vertical shafts: three or more room boxes stacked on one footprint (level 2's bays)
        stacks = {}
        for r in self.rooms.values():
            key = tuple(round(v) for v in (r['min'][0], r['min'][1], r['max'][0], r['max'][1]))
            stacks.setdefault(key, []).append(r)
        self.shafts = [((k[0] + k[2]) / 2, (k[1] + k[3]) / 2, min(r['min'][2] for r in v), max(r['max'][2] for r in v))
                       for k, v in stacks.items() if len(v) >= 3]

    def shaft_near(self, p, reach=25):
        """The centre (x, y) of a shaft whose column p is in or beside, or None."""
        near = [(math.dist(p[:2], s[:2]), s) for s in self.shafts
                if s[2] - 25 <= p[2] <= s[3] + 5 and math.dist(p[:2], s[:2]) < reach]
        return min(near)[1][:2] if near else None

    def room_of(self, p):
        """The smallest room box holding p (rooms nest: tunnels inside sections)."""
        best = None
        for name, r in self.rooms.items():
            if all(r['min'][i] - .5 <= p[i] <= r['max'][i] + .5 for i in range(3)):
                vol = math.prod(r['max'][i] - r['min'][i] for i in range(3))
                if best is None or vol < best[0]:
                    best = (vol, name)
        return best[1] if best else None

    def path(self, start, goal):
        """Portal points from start's room to goal's room (Dijkstra on distance), then the goal."""
        a, b = self.room_of(start), self.room_of(goal)
        if a is None or b is None:
            raise ValueError(f'no room holds {"start" if a is None else "goal"}')
        import heapq
        best = {a: (0.0, start, None, None)}
        todo = [(0.0, a)]
        while todo:
            cost, room = heapq.heappop(todo)
            if room == b:
                break
            if cost > best[room][0]:
                continue
            here = best[room][1]
            for other, point, how in self.edges.get(room, []):
                c = cost + math.dist(here, point)
                if other not in best or c < best[other][0]:
                    best[other] = (c, point, room, how)
                    heapq.heappush(todo, (c, other))
        if b not in best:
            raise ValueError(f'no portal path {a} -> {b}')
        steps, room = [], b
        while best[room][2] is not None:
            steps.append({'to': best[room][1], 'room': room, 'via': best[room][3]})
            room = best[room][2]
        steps.reverse()
        steps.append({'to': goal, 'room': b, 'via': 'goal'})
        return a, steps


class Nav:
    """Where a hero can stand, as scanned in the game (coop.NavStart): cells and their links."""

    def __init__(self, cells=None):
        self.cells = cells or {}          # (i, j, zbucket) -> [x, y, z, [neighbour keys]]

    @staticmethod
    def key(i, j, z):
        return (int(i), int(j), int(math.floor(z / 4)))

    def add_lines(self, text):
        for line in text.splitlines():
            f = line.split()
            if not f or f[0] != 'c':
                continue
            x, y, z, i, j = float(f[1]), float(f[2]), float(f[3]), int(f[4]), int(f[5])
            k = self.key(i, j, z)
            links = [self.key(int(f[n]), int(f[n + 1]), float(f[n + 2])) for n in range(6, len(f), 3)]
            cell = self.cells.setdefault(k, [x, y, z, []])
            cell[3] = sorted(set(map(tuple, cell[3])) | set(links))

    def save(self, path):
        Path(path).parent.mkdir(parents=True, exist_ok=True)
        Path(path).write_text(json.dumps([[list(k), v[0], v[1], v[2], [list(n) for n in v[3]]] for k, v in self.cells.items()]))

    @classmethod
    def load(cls, path):
        nav = cls()
        for k, x, y, z, links in json.loads(Path(path).read_text()):
            nav.cells[tuple(k)] = [x, y, z, [tuple(n) for n in links]]
        return nav

    # Walking: a rise of up to WALK_RISE per 2-unit cell (about 20 degrees). Steeper: a jump, only onto
    # a cell that is flat enough to land and stand on. Max slid back down a 33 degree gutter wall.
    WALK_RISE = 0.7
    JUMP_RISE = 3.5
    # Max's jetpack, fuelled at a refueller: holding jump lifted him 38 units in 3 s
    FLY_RISE = 34.0
    fly = False
    chute = False      # Kurt: long drops with the chute

    def flat(self, k):
        cell = self.cells[k]
        if not hasattr(self, '_flat'):
            self._flat = {}
        if k not in self._flat:
            # a landing: at least two neighbours level with it (a ledge's edge has a drop beside it)
            level = [n for n in cell[3] if n in self.cells and abs(self.cells[n][2] - cell[2]) <= 0.45]
            self._flat[k] = len(level) >= 2
        return self._flat[k]

    # cells the hero failed to reach twice from the same place (a link the scan saw but the body cannot
    # take: level 2's arena pit, a 6 unit step out), for every later plan in this run
    blocked = set()
    fails = {}

    @staticmethod
    def spot(p):
        return (round(p[0] / 2), round(p[1] / 2), round(p[2] / 2))

    def step_cost(self, k, n):
        a, b = self.cells[k], self.cells[n]
        if Nav.spot(b) in Nav.blocked:
            return None
        rise = b[2] - a[2]
        base = math.dist(a[:3], b[:3])
        if -6 <= rise <= self.WALK_RISE:
            # Max without Kurt's chute: keep off the rim of a drop (the pilot cuts corners, and the way
            # back up costs fuel)
            return base * 20 if self.fly and not self.chute and self.rim(n) else base
        if rise < -6 and self.chute:                    # Kurt's chute
            return base * 2
        if rise < -6 and self.fly:
            # Max's jetpack slows the fall, but the way back up costs fuel that only a refueller gives
            # (level 2 room 6: the shaft's open top beside the path): only when there is no other way
            return base * 40
        if 0 < rise <= self.JUMP_RISE and self.flat(n):
            return base * 4
        if self.fly and rise <= self.FLY_RISE and self.flat(n):
            return base * 6
        return None          # too steep, or a long fall without Kurt's chute

    def rim(self, k):
        """A cell beside a drop of more than 6 units."""
        cache = self.__dict__.setdefault('_rim', {})
        if k not in cache:
            z = self.cells[k][2]
            cache[k] = any(n in self.cells and self.cells[n][2] < z - 6 for n in self.cells[k][3])
        return cache[k]

    def nearest(self, p):
        return min(self.cells, key=lambda k: math.dist(self.cells[k][:3], p))

    def path(self, start, goal):
        """A* over the cells; the points of the path where it turns."""
        import heapq
        a, b = self.nearest(start), self.nearest(goal)
        came, cost = {a: None}, {a: 0.0}
        todo = [(0.0, a)]
        while todo:
            _, k = heapq.heappop(todo)
            if k == b:
                break
            for n in self.cells[k][3]:
                if n not in self.cells:
                    continue
                step = self.step_cost(k, n)
                if step is None:
                    continue
                c = cost[k] + step
                if n not in cost or c < cost[n]:
                    cost[n], came[n] = c, k
                    heapq.heappush(todo, (c + math.dist(self.cells[n][:3], self.cells[b][:3]), n))
        if b not in came:
            return None
        keys, k = [], b
        while k is not None:
            keys.append(k)
            k = came[k]
        keys.reverse()
        pts = [self.cells[k][:3] for k in keys]
        out = [pts[0]]
        for i in range(1, len(pts) - 1):
            d0 = (pts[i][0] - pts[i - 1][0], pts[i][1] - pts[i - 1][1])
            d1 = (pts[i + 1][0] - pts[i][0], pts[i + 1][1] - pts[i][1])
            if (d0 != d1 or abs(pts[i + 1][2] - pts[i][2]) > .5 or math.dist(out[-1], pts[i]) > 12
                    or pts[i + 1][2] - pts[i][2] > self.WALK_RISE):
                out.append(pts[i])
        out.append(pts[-1])
        return out


def scan(game, out, timeout=900, fly=False, chute=False):
    """Scan the hero's surroundings (coop.NavStart) and save the cells."""
    for old in game.coop.glob('note_nav_*.txt'):
        old.unlink()
    # not falling (the same height a moment later): it may stand on an object the line of sight misses
    x, y, z = game.pos()
    time.sleep(.4)
    standing = abs(game.pos()[2] - z) < 0.05
    # flying opens up every floor above; a room and its shaft fit in 12000 cells
    game.lua(f'coop.NavStart({x},{y},{z},2,{12000 if fly else 40000},4,6,{36 if fly else 0},'
             f'{300 if chute else 0},{1 if standing else "nil"})')
    deadline = time.monotonic() + timeout
    state = game.coop / 'note_nav_state.txt'
    while time.monotonic() < deadline:
        text = state.read_text() if state.exists() else ''
        if text.startswith('done') or text.startswith('nofloor'):
            break
        time.sleep(1)
    nav = Nav()
    for f in sorted(game.coop.glob('note_nav_[0-9]*.txt'), key=lambda f: int(f.stem.split('_')[-1])):
        nav.add_lines(f.read_text())
    nav.fly, nav.chute = fly, chute
    nav.save(out)
    return nav, (state.read_text() if state.exists() else 'timeout')


def walk(game, nav, goal, log, timeout=90):
    """Follow the A* path over nav to goal's nearest reachable cell; (reached point, state)."""
    here = game.pos()
    pts = nav.path(list(here), list(goal))
    if not pts:
        return here, 'nopath'
    for i, pt in enumerate(pts[1:], 1):
        rise = pt[2] - pts[i - 1][2]
        climb = rise > Nav.WALK_RISE
        flying = nav.fly and rise > Nav.JUMP_RISE
        if flying:
            # straight up first, then across (moving while rising caught Max under the lips)
            if not hop(game, pt, log):
                return game.pos(), 'hop'
            continue
        r = game.goto(*pt, radius=1.5 if i < len(pts) - 1 else 2.5, timeout=timeout, explore=0,
                      jump=int(climb and not flying), fly=int(flying), chute=int(nav.chute and rise < -6))
        fell = r.get('state') != 'arrived' and game.pos()[2] < min(pt[2], pts[i - 1][2]) - 6
        if fell:
            # keep later plans off the edge he went over: the cells above where he landed
            low = game.pos()
            for c in nav.cells.values():
                if c[2] > low[2] + 6 and math.dist(c[:2], low[:2]) < 6:
                    Nav.blocked.add(Nav.spot(c))
        if fell and nav.fly:
            # off the edge into a pit (level 2 room 6: the shaft's open top, down onto its locked
            # hatch): to where the way up is open, fly back up to the waypoint and go on
            # a shaft's centre line is open (its overhangs are object geometry the line of sight
            # misses); elsewhere the nearest column the line of sight finds open
            level = getattr(game, 'level', None)
            col = (level and level.shaft_near(game.pos())) or open_column(game, game.pos(), pt[2])
            # to the waypoint, else back where he last stood (a rim can be walled from above)
            for back in (here, pts[i - 1], pt):
                at = nudge(game, col, tol=1.5) if col else game.pos()
                ok, p = jet_to(game, back[2], back[:2])
                log(f'      rise at {[round(v, 1) for v in at]} for {[round(v, 1) for v in back]}: '
                    f'{"landed" if ok else "no"}, now {[round(v, 1) for v in p]}')
                if ok:
                    break
            log(f'    fell over an edge: flew back up to '
                f'{"waypoint " + str(i) if ok and back is pt else "where he was" if ok else "nowhere"}, '
                f'now {[round(v, 1) for v in p]}')
            if ok and back is pt:
                r = game.goto(*pt, radius=1.5 if i < len(pts) - 1 else 2.5, timeout=timeout, explore=0)
            else:
                return game.pos(), 'fell'           # a new plan, around the edge just blocked
        if r.get('state') != 'arrived':
            key = (Nav.spot(pt), Nav.spot(game.pos()))
            Nav.fails[key] = Nav.fails.get(key, 0) + 1
            if Nav.fails[key] >= 2 and Nav.spot(pt) not in Nav.blocked:
                Nav.blocked.add(Nav.spot(pt))
                log(f'    waypoint {pt} failed twice from here: planning without it')
            log(f'    waypoint {i}/{len(pts) - 1} {pt}: {r.get("state")} at {r.get("pos")}')
            return r.get('pos', here), r.get('state')
    return game.pos(), 'arrived'


def reachable(nav, start, safe=True):
    """The cells the hero can walk to from start (by the walking rules); with safe, only those it can
    also come back from (level 2's arena has a one-way face onto a strip with no way back)."""
    s = nav.nearest(start)
    seen, todo = {s}, [s]
    back = {}
    while todo:
        k = todo.pop()
        for n in nav.cells[k][3]:
            if n in nav.cells and nav.step_cost(k, n) is not None:
                back.setdefault(n, []).append(k)
                if n not in seen:
                    seen.add(n)
                    todo.append(n)
    if not safe:
        return seen
    home, todo = {s}, [s]
    while todo:
        k = todo.pop()
        for prev in back.get(k, []):
            if prev not in home:
                home.add(prev)
                todo.append(prev)
    return seen & home


def rim_toward(nav, start, target, above=4.0):
    """The reachable cell nearest target among those standing at least `above` higher than it (shoot a
    generator from its pit's rim: the pit cannot be climbed out of)."""
    cells = [k for k in reachable(nav, start) if nav.cells[k][2] >= target[2] + above]
    if not cells:
        return reachable_toward(nav, start, target)
    return nav.cells[min(cells, key=lambda k: math.dist(nav.cells[k][:3], target))][:3]


def reachable_toward(nav, start, goal):
    """The reachable cell nearest the goal."""
    best = min(reachable(nav, start), key=lambda k: nav.cells[k][:3] and math.dist(nav.cells[k][:3], goal))
    return nav.cells[best][:3]


WEAPONS = ('OBJ_UZI', 'OBJ_SHOTGUN', 'OBJ_MAGNUM', 'OBJ_DOUBLEA', 'OBJ_CARBATTERY', 'OBJ_BOXINGGLOVE',
           'OBJ_LASERCANNON', 'OBJ_SUPERCHAINGUN', 'OBJ_FIREBALLCHAINGUN', 'OBJ_GUIDEDROCKET', 'OBJ_BFB',
           'OBJ_GRENADEITEM')


def collect(game, level, nav, log, radius=80, kinds=WEAPONS):
    """Pick up the weapons lying within reach (a player collects them on the way; Max then equips one in
    each of his four hands). True if anything was picked up."""
    here = game.pos()
    cells = reachable(nav, here)
    got = 0
    for o in sorted(level.m.get('pickups', []), key=lambda o: math.dist(o['pos'], here)):
        if o['type'] not in kinds or math.dist(o['pos'], here) > radius or not alive(game, o['name']):
            continue
        k = min(cells, key=lambda k: math.dist(nav.cells[k][:3], o['pos']))
        if math.dist(nav.cells[k][:3], o['pos']) > 3:
            continue
        walk(game, nav, o['pos'], log, timeout=30)
        game.goto(*o['pos'], radius=0.6, timeout=8, explore=0)
        for _ in range(2):
            if not alive(game, o['name']):
                break
            game.lua('coop_press(57, 200)')
            time.sleep(1.0)
        if not alive(game, o['name']):
            got += 1
            log(f'    picked up {o["name"]} ({o["type"]})')
    return got > 0


def max_arm(game, log, rounds=6):
    """Max has four arms: equip every weapon he carries (modern controls: F equips the selected weapon,
    E selects the next)."""
    for _ in range(rounds):
        game.lua('coop_press(33, 150)')
        time.sleep(.5)
        game.lua('coop_press(18, 150)')
        time.sleep(.5)
    log('    armed: equip pressed on each carried weapon')


def hits_since(game, offset):
    """Names this game's hero hit (bridge.log 'damage bob -> <name>') after offset; and the new offset."""
    import re
    log = game.coop / 'bridge.log'
    data = log.read_bytes()
    return re.findall(r'damage bob -> (\S+)', data[offset:].decode('latin-1')), len(data)


def fire_sweep(game, wanted, log, pitches=(0, -20, 20, -40, 40), steps=16, sniper=False):
    """Find which of the wanted objects the hero can hit from here, without knowing where their visible
    part is (a scripted wall's registered position is its pivot, often inside rock): face steps
    directions at a few elevations (degrees), fire a short burst at each, and read what was hit. Then
    hold the aim that hit and fire until it is gone. Aim is set on the hero (mdkGobPointToXYZ), never
    with mouse look: undoing a clamped mouse pitch left the camera turned until forward walked backward.
    Returns the name destroyed, or None."""
    game.lua('coop.gunner=nil')
    found = None
    offset = hits_since(game, 0)[1]

    def face(ang, elev):
        a, e = math.radians(ang), math.radians(elev)
        return (f'local p=coop.real_player() coop.Face(p, {math.cos(a):.4f}, {math.sin(a):.4f}, '
                f'{math.tan(e):.4f})')
    for elev in pitches:
        for k in range(steps):
            ang = k * 360 / steps
            game.lua(face(ang, elev) + (' coop_test_sniper()' if sniper else ' coop_press(256, 250)'))
            time.sleep(.4)
            game.lua(face(ang, 0))     # upright again (pointing up or down tips Max over)
            names, offset = hits_since(game, offset)
            hit = [n for n in names if n in wanted]
            if hit:
                found = (hit[0], ang, elev)
                break
        if found:
            break
    game.lua(f'coop.GunnerOn({game.gun_range})')
    if not found:
        log(f'    {"sniper" if sniper else "fire"} sweep hit none of {sorted(wanted)[:6]}')
        return None
    name, ang, elev = found
    log(f'    fire sweep: {name} is hit facing {ang:.0f} degrees, elevation {elev}')
    t0 = time.monotonic()
    game.lua('coop.gunner=nil')
    while time.monotonic() - t0 < 30 and alive(game, name):
        game.lua(face(ang, elev) + (' coop_test_sniper()' if sniper else ' coop_press(256, 600)'))
        time.sleep(.5)
        game.lua(face(ang, 0))
    game.lua(f'coop.GunnerOn({game.gun_range})')
    if alive(game, name) and not sniper:
        log(f'    fire sweep: {name} still alive after 30 s')
        return None
    log(f'    fire sweep: destroyed {name} in {time.monotonic() - t0:.1f}s')
    return name

def snipe(game, name, log):
    """A sniper shot at name: the game's own sniper round, launched along Kurt's aim at the target (with
    a few heights on it). Known shortcut: sniper mode's camera is not steered by the bot yet (its rounds
    follow the camera, not the hero's facing), so the round is launched by coop_test_sniper; the hit, the
    script's reaction and its replication to the other games are the game's own."""
    off = hits_since(game, 0)[1]
    for dz in (0, 1, -1, 2, -2, 3):
        game.lua(f'local p=coop.real_player() local t=getglobal("{name}") if (t) then coop.Face(p, '
                 f't.position.x-p.position.x, t.position.y-p.position.y, t.position.z+{dz}-p.position.z-1) '
                 f'coop_test_sniper() end')
        time.sleep(1)
        game.lua(f'local p=coop.real_player() local t=getglobal("{name}") if (t) then coop.Face(p, '
                 f't.position.x-p.position.x, t.position.y-p.position.y, 0) end')
        names, off = hits_since(game, off)
        if name in names:
            log(f'    sniped {name} (aim {dz:+d})')
            return True
    log(f'    sniper rounds missed {name}')
    return False


def aimed_kill(game, name, log, seconds=60):
    """Fire straight at an object whose registered position is where it is (a generator), facing it
    with pitch, until it is gone. The hit log says whether bursts connect; while they do not (a pit's rim
    in the way) the hero closes in further."""
    t0 = time.monotonic()
    game.lua('coop.gunner=nil')
    close, misses = 12, 0
    off = hits_since(game, 0)[1]
    while time.monotonic() - t0 < seconds and alive(game, name):
        at = live_pos(game, name)
        if at and math.dist(game.pos(), at) > close:
            # along the scanned floor (the pilot's own stepping trips on the arena's one-sided face)
            nav, _ = scan(game, str(Path(__file__).resolve().parents[1] / 'local/campaign/nav/aim.json'))
            if nav.cells:
                walk(game, nav, rim_toward(nav, game.pos(), at), log, timeout=40)
        for dz in (2, 0, 4):
            game.lua(f'local p=coop.real_player() local t=getglobal("{name}") if (t) then coop.Face(p, '
                     f't.position.x-p.position.x, t.position.y-p.position.y, t.position.z+{dz}-p.position.z-3) '
                     f'coop_press(256, 300) end')
            time.sleep(.45)
            # upright again, same bearing (pointing up or down tips Max onto his side)
            game.lua(f'local p=coop.real_player() local t=getglobal("{name}") if (t) then '
                     f'coop.Face(p, t.position.x-p.position.x, t.position.y-p.position.y, 0) end')
        names, off = hits_since(game, off)
        at = live_pos(game, name)
        here = game.pos()
        log(f'      aim {name}: hero {[round(v) for v in here]} '
            f'{round(math.dist(here, at), 1) if at else "?"} away, hit {sorted(set(names))[:3]}')
        if name in names:
            misses = 0
        else:
            misses += 1
            if misses >= 2 and close > 4:
                close = max(4, close - 4)
                misses = 0
    game.lua(f'coop.GunnerOn({game.gun_range})')
    gone = not alive(game, name)
    log(f'    {"killed" if gone else "could not kill"} {name} by aimed fire in {time.monotonic() - t0:.0f}s'
        f' (from {close} units)')
    return gone


def live_pos(game, name):
    t = game.note('lp', f'local g=getglobal("{name}") if (g and coop_alive(g)==1) then coop_note("lp", '
                        f'format("%.2f %.2f %.2f", g.position.x, g.position.y, g.position.z)) else coop_note("lp", "none") end')
    return None if t.startswith('none') else tuple(float(v) for v in t.split())


def chain_step(game, level, nav, pos, target, log, chute=False):
    """The level scripts' own dependency chains (campaign_mine.py 'deps'): something near the hero or the
    target is opened by a handler on another object (a sniper lock opening a door); that opener may first
    need enabling by other objects' deaths (three enemies). Kill the live prerequisites, then trigger the
    opener with the damage it checks for. True if something was done."""
    deps = level.m.get('deps', {})
    where = {o['name']: o['pos'] for o in level.m['objects']}
    rooms = set(level.rooms)
    for opener, events in deps.items():
        if opener in rooms or opener == 'Level':
            continue
        for event, info in events.items():
            if event in ('OnCreate', 'OnUpdate', 'OnTimer', 'OnAnimLoop'):
                continue     # not something a player sets off
            if not any(t in where and min(math.dist(where[t], pos), math.dist(where[t], target)) < 80
                       for t in info['opens']):
                continue
            if not alive(game, opener):
                continue
            prereq = [o for o, evs in deps.items() if any(opener in i['enables'] for i in evs.values())
                      and alive(game, o)]
            for o in prereq:
                at = live_pos(game, o)
                if not at:
                    continue
                spot = reachable_toward(nav, game.pos(), at)
                log(f'    {opener} needs {o} dead first: going near it')
                walk(game, nav, spot, log, timeout=30)
                t0 = time.monotonic()
                while time.monotonic() - t0 < 30 and alive(game, o):
                    time.sleep(1)
                if alive(game, o):
                    gone = fire_sweep(game, {o}, log, pitches=(0, -20, 20))
                    log(f'    {o}: {"killed" if gone else "still alive"}')
            sniper = 'DAMAGE_SNIPER' in info['damage']
            log(f'    trigger {opener}.{event} ({"sniper" if sniper else "shots"}) to open {info["opens"]}')
            if sniper:
                snipe(game, opener, log)
            else:
                fire_sweep(game, {opener}, log, pitches=(0, -20, 20, -40))
            return True
    return False


def objectives(level, pos, toward=None, radius=150):
    """Scripted things whose death the level waits for (an OnDie handler: generators, locks, alarms,
    walls) within radius of the hero; those nearest the way on (toward) first, and among equals the ones
    the scripts react to before plain destructibles."""
    kinds = ('OBJ_DESTRUCTABLE', 'OBJ_DOGANBOYGENERATOR', 'OBJ_SAMSMITEGENERATOR')
    out = [o for o in level.m['objects']
           if (o['type'] in kinds or 'OnDie' in level.m['events'].get(o['name'], []))
           and math.dist(o['pos'][:2], pos[:2]) < radius]
    ref = toward or pos

    def rank(o):
        scripted = 'OnDie' in level.m['events'].get(o['name'], []) or 'GENERATOR' in o['type']
        return math.dist(o['pos'][:2], ref[:2]) - (40 if scripted else 0)
    return sorted(out, key=rank)


def kill_objective(game, nav, obj, log, seconds=25):
    """Walk to a reachable cell that sees obj (engine line of sight), then shoot it, with pitch, until
    the game says it is gone."""
    x, y, z = obj['pos']
    cells = sorted(reachable(nav, game.pos()), key=lambda k: math.dist(nav.cells[k][:3], obj['pos']))[:40]
    # the line of sight ends inside the object's own body at its centre: look for a height on it that
    # a reachable place sees (the arena generators show from +4 up) --
    spot = aim = None
    pts = ','.join(f'{v:.2f}' for k in cells for v in nav.cells[k][:3])
    view = game.note('view', f'coop.NavView({x},{y},{z},{{{pts}}},{{2,4,6,8}})').split()
    if view and view[0] != 'none':
        spot = tuple(nav.cells[cells[int(view[0]) - 1]][:3])
        aim = z + float(view[1])
    if not spot:
        # an object inside another's body (level 2's door locks sit at the door's centre, shown on its
        # face): the line of sight misses it, but bullets hit its face from in front. Try spots around
        # it and keep the first whose bursts the game reports as hits on it.
        return circle_kill(game, nav, obj, log, seconds)
    walk(game, nav, spot, log)
    t0 = time.monotonic()
    while time.monotonic() - t0 < seconds:
        if not alive(game, obj['name']):
            log(f'    killed {obj["name"]} ({obj["type"]}) in {time.monotonic() - t0:.1f}s')
            return True
        game.lua(f'local p=coop.real_player() coop.Face(p, {x}-p.position.x, {y}-p.position.y, {aim}-p.position.z-3) '
                 f'coop_press(256, 600)')
        time.sleep(.5)
    log(f'    {obj["name"]}: still alive after {seconds}s of fire')
    return False


def circle_kill(game, nav, obj, log, seconds=25, max_spots=12):
    """Shoot obj from reachable spots on rings 6, 12 and 20 units around it (eight bearings), short
    bursts at a few heights, until a burst hits it (bridge.log); then keep that aim until it is gone.
    For what the line of sight misses: a lock inside its door's body (shown on the door's face), a
    target high in an alcove (level 2 room 2's teleporters behind glass)."""
    x, y, z = obj['pos']
    if math.dist(game.pos(), obj['pos']) > 60:
        log(f'    {obj["name"]}: no reachable place sees it')
        return False
    cells = list(reachable(nav, game.pos()))
    tried = []
    for ring in (6, 12, 20):
        near = [c for c in cells if ring - 3 < math.dist(nav.cells[c][:2], (x, y)) < ring + 3
                and abs(nav.cells[c][2] - z) < 25]
        for k in range(8):
            if not near or len(tried) >= max_spots:
                break
            a = math.radians(k * 45)
            want = (x + ring * math.cos(a), y + ring * math.sin(a))
            c = min(near, key=lambda c: math.dist(nav.cells[c][:2], want))
            spot = tuple(nav.cells[c][:3])
            if math.dist(spot[:2], want) > ring or any(math.dist(spot, t) < 4 for t in tried):
                continue
            tried.append(spot)
            walk(game, nav, spot, log, timeout=30)
            offset = hits_since(game, 0)[1]
            for dz in (0, 1.5, 3, 4.5, -1.5):
                game.lua(f'local p=coop.real_player() coop.Face(p, {x}-p.position.x, {y}-p.position.y, '
                         f'{z + dz}-p.position.z-3) coop_press(256, 700)')
                time.sleep(.8)
                names, offset = hits_since(game, offset)
                if obj['name'] not in names:
                    continue
                t0 = time.monotonic()
                while alive(game, obj['name']) and time.monotonic() - t0 < seconds:
                    game.lua(f'local p=coop.real_player() coop.Face(p, {x}-p.position.x, {y}-p.position.y, '
                             f'{z + dz}-p.position.z-3) coop_press(256, 600)')
                    time.sleep(.5)
                game.lua('local p=coop.real_player() coop.Face(p, 1, 0, 0)')
                dead = not alive(game, obj['name'])
                log(f'    {obj["name"]}: {"killed" if dead else "hit but alive"} from '
                    f'{[round(v, 1) for v in spot]} (aim {dz:+})')
                return dead
    game.lua('local p=coop.real_player() coop.Face(p, 1, 0, 0)')
    log(f'    {obj["name"]}: no hit from {len(tried)} spots around it')
    return False


def alive(game, name):
    return game.note('alive', f'local g=getglobal("{name}") coop_note("alive", (g and coop_alive(g)==1) and "1" or "0")').strip() == '1'


def destroy(game, objs, toward, log, seconds=20):
    """Fire along the way the hero wants to go (a blocker is what is in the way; a scripted wall's
    registered position is its pivot, not its face) until one of the nearby destructibles is gone."""
    t0 = time.monotonic()
    while time.monotonic() - t0 < seconds:
        gone = [o for o in objs if not alive(game, o['name'])]
        if gone:
            log(f'    destroyed {gone[0]["name"]} in {time.monotonic() - t0:.1f}s')
            return gone[0]
        game.lua(f'local p=coop.real_player() coop.Face(p, {toward[0]}-p.position.x, {toward[1]}-p.position.y, 0) '
                 f'coop_press(256, 600)')
        time.sleep(.5)
    return None


def open_column(game, here, top, reach=14):
    """The nearest point around here (rings to reach units) with a clear way straight up to top, by the
    engine's line of sight both ways; None if there is none. (Level 2: fallen into the top bay of room 5's
    shaft, under the overhang of room 6's floor, the way out is the open middle.)"""
    pts = [(here[0], here[1])]
    for r in range(3, reach + 1, 3):
        for k in range(8):
            a = math.radians(k * 45)
            pts.append((here[0] + r * math.cos(a), here[1] + r * math.sin(a)))
    flat = ','.join(f'{x:.2f},{y:.2f}' for x, y in pts)
    lo, hi = here[2] + 2, top + 3
    code = (f'local t={{{flat}}} local i=1 local hit=0 '
            f'while (i < {2 * len(pts)} and hit == 0) do '
            f'if (coop.NavClear(t[i],t[i+1],{lo},t[i],t[i+1],{hi}) and coop.NavClear(t[i],t[i+1],{hi},t[i],t[i+1],{lo})) '
            f'then hit = (i + 1) / 2 end i = i + 2 end coop_note("col", tostring(hit))')
    try:
        n = int(float(game.note('col', code).strip()))
    except (ValueError, TimeoutError):
        return None
    return pts[n - 1] if n > 0 else None


def jet_to(game, stand_z, xy, seconds=12, over=6.0, land=True):
    """Max's jetpack: up until above stand_z, then over to xy holding that height (thrust off above the
    band, on below it, so the tank lasts), and down onto it. (landed there, position)"""
    game.lua('coop.pilot=nil coop_press(57, 15000)')
    thrust = True
    t0, best, best_t = time.monotonic(), -1e9, time.monotonic()
    near, near_t = 1e9, time.monotonic()
    while time.monotonic() - t0 < seconds:
        p = game.pos()
        if p[2] > best + 0.3:
            best, best_t = p[2], time.monotonic()
        if best > stand_z + over:
            d = math.dist(p[:2], xy)
            if d < 1.5:
                break
            if d < near - 0.3:
                near, near_t = d, time.monotonic()
            elif time.monotonic() - near_t > 2:
                break                       # held off (a wall above the rim): keep the fuel
            game.lua(f'local p=coop.real_player() coop.Face(p,{xy[0]}-p.position.x,{xy[1]}-p.position.y,0) '
                     f'coop_press(17,200)')
            if thrust and p[2] > stand_z + over + 3:
                game.lua('coop_press(57, 0)')
                thrust = False
            elif not thrust and p[2] < stand_z + over:
                game.lua('coop_press(57, 15000)')
                thrust = True
        elif time.monotonic() - best_t > 3:
            break                           # out of fuel, or under a ceiling
        time.sleep(.1)
    if not land:                            # on through the air to the next point: keep the thrust as it is
        p = game.pos()
        return math.dist(p[:2], xy) < 1.5 and abs(p[2] - stand_z) < 3, p
    game.lua('coop_press(57, 0)')
    last = None
    for _ in range(30):                     # down and settled
        time.sleep(.2)
        p = game.pos()
        if last and abs(p[2] - last[2]) < 0.05:
            break
        last = p
    p = game.pos()
    return abs(p[2] - stand_z) < 2.5 and math.dist(p[:2], xy) < 3, p


def hop(game, landing, log, rise_s=4.0, rest_s=6.0):
    """One jetpack hop onto a lip above, the way a player does it (the owner: Max kept catching under
    the lips): rise straight up where he stands (the scan found that column clear) until a little above
    the lip, let go, then step across onto it. Fuel is spent only on the height needed."""
    game.lua('coop.pilot=nil')
    # fuel: only a refueller fills the jetpack (a tank burns for about 10.5 s)
    time.sleep(rest_s)
    want = landing[2] + 2.5
    game.lua('coop_press(57, 6000)')
    t0 = time.monotonic()
    while time.monotonic() - t0 < rise_s and game.pos()[2] < want:
        time.sleep(.1)
    game.lua('coop_press(57, 0)')
    peak = game.pos()[2]
    if peak < landing[2]:
        log(f'    hop: rose only to {peak:.1f} for a lip at {landing[2]:.1f} (fuel?)')
        return False
    # across onto the lip while still above it
    for _ in range(12):
        p = game.pos()
        if math.dist(p[:2], landing[:2]) < 1.5:
            break
        game.lua(f'local p=coop.real_player() coop.Face(p,{landing[0]}-p.position.x,{landing[1]}-p.position.y,0) '
                 f'coop_press(17,200)')
        if p[2] < landing[2] + 0.5:
            game.lua('coop_press(57, 250)')       # a touch more lift if sinking below the lip
        time.sleep(.2)
    time.sleep(.8)
    p = game.pos()
    ok = abs(p[2] - landing[2]) < 1.5
    log(f'    hop to {[round(v, 1) for v in landing]}: {"landed" if ok else "missed"} at {[round(v, 1) for v in p]}')
    return ok


def find_shaft(level, pos, goal):
    """A vertical shaft between pos and goal: room boxes stacked one on another with the same footprint
    (level 2's room 5 exit tubes, 19.4 units a level). Returns (centre x, y, level floors) or None."""
    stack = [r for n, r in level.rooms.items()
             if r['min'][0] - 2 <= goal[0] <= r['max'][0] + 2 and r['min'][1] - 2 <= goal[1] <= r['max'][1] + 2
             and pos[2] - 5 <= r['min'][2] <= goal[2] + 5]
    by_box = {}
    for r in stack:
        key = (round(r['min'][0]), round(r['min'][1]), round(r['max'][0]), round(r['max'][1]))
        by_box.setdefault(key, []).append(r)
    best = max(by_box.values(), key=len, default=[])
    if len(best) < 2:
        return None
    floors = sorted(r['min'][2] for r in best)
    cx = (best[0]['min'][0] + best[0]['max'][0]) / 2
    cy = (best[0]['min'][1] + best[0]['max'][1]) / 2
    return cx, cy, floors


def nudge(game, xy, tol=1.0, tries=40):
    """Short steps straight at a spot (the pilot's planning is for longer ways)."""
    for _ in range(tries):
        p = game.pos()
        if math.dist(p[:2], xy) < tol:
            break
        game.lua(f'local p=coop.real_player() coop.Face(p,{xy[0]}-p.position.x,{xy[1]}-p.position.y,0) '
                 f'coop_press(17,150)')
        time.sleep(.2)
    return game.pos()


def mount_refueller(game, pad, log):
    """Stand on a refueller: it sits on a step, so walk at it with a jump until on its spot. The
    jetpack fills only there (it does not refill while Max stands elsewhere)."""
    x, y, z = pad['pos']
    nudge(game, (x, y), tol=4)
    for _ in range(6):
        p = game.pos()
        if math.dist(p[:2], (x, y)) < 2 and p[2] > z - 0.6:
            time.sleep(2)
            return True
        game.lua(f'local p=coop.real_player() coop.Face(p,{x}-p.position.x,{y}-p.position.y,0) '
                 f'coop_press(57,250) coop_press(17,450)')
        time.sleep(1.2)
    log(f'    could not get onto {pad["name"]}')
    return False


def shaft_doors(level, shaft, low, high):
    """The hatches in a shaft's column between two heights, lowest first (level 2: proximity doors that
    open as Max comes within reach; locked ones open when the turrets of their bay are dead)."""
    cx, cy, _ = shaft
    return sorted((o for o in level.m['objects'] if 'DOOR' in o['type']
                   and math.dist(o['pos'][:2], (cx, cy)) < 3 and low < o['pos'][2] < high),
                  key=lambda o: o['pos'][2])


HATCH_TOP = 2.9         # Max stands this far above a closed hatch's registered position


def bay_climb(game, level, shaft, goal, log, max_trips=8):
    """Climb a shaft of hatched bays (level 2's room 5 to room 6, 118 units, worked out in game): a tank
    burns for about 10.5 s and lifts Max some 15 units a second, and only the refueller at the foot fills
    it. Each bay's hatch is locked until the bay's four turrets are dead (Level.SetUpDoorAndEnemies, mined
    as 'locks'). So, trip by trip: refuel, burn up the open ring and then the centre column (hatches open
    as he passes), drift off-centre over the hatch below the locked one so it shuts under him, land on it
    and shoot that bay's turrets; then walk back to the centre, drop through the hatches and refuel. Once
    every hatch is unlocked, one burn takes him to the top."""
    cx, cy, floors = shaft
    ring = goal[:2]
    here = game.pos()
    base = floors[0]                    # the shaft's first level: its pad is in the room below that
    side = (ring[0] - cx, ring[1] - cy)
    n = math.hypot(*side) or 1
    land_xy = (cx + side[0] / n * 16, cy + side[1] / n * 16)
    pads = [o for o in level.m['objects'] if o['type'] == 'OBJ_REFUELLER' and base - 45 < o['pos'][2] < base
            and math.dist(o['pos'][:2], (cx, cy)) < 60]
    if not pads:
        return False
    pad = min(pads, key=lambda o: math.dist(o['pos'][:2], (cx, cy)))
    doors = shaft_doors(level, shaft, base - 20, goal[2])
    if not doors:
        return False
    clear_z = doors[0]['pos'][2] - 8.5      # below this the centre is roofed (room 5's light), the ring open
    locks = level.m.get('locks', {})

    def burn(stand_z, xy):
        """Up to just above stand_z, then over to xy and down onto it, the way the owner climbs (one tank
        does the whole shaft): from the floor up the open ring and then the centre column, from a hatch
        straight to the centre column; thrust off once high enough, drifting across, and on again only
        if sinking below the landing."""
        if game.pos()[2] < clear_z:
            nudge(game, ring)
        game.lua('coop.pilot=nil coop.gunner=nil coop_press(57, 12000)')
        thrust = True
        t0, best, best_t = time.monotonic(), -1e9, time.monotonic()
        while time.monotonic() - t0 < 13:
            p = game.pos()
            if p[2] > best + 0.3:
                best, best_t = p[2], time.monotonic()
            above = best > stand_z + 2.5
            to = xy if above else ((cx, cy) if p[2] > clear_z else ring)
            game.lua(f'local p=coop.real_player() coop.Face(p,{to[0]}-p.position.x,{to[1]}-p.position.y,0) '
                     f'coop_press(17,200)')
            if above:
                if math.dist(p[:2], xy) < 2:
                    break
                if thrust and p[2] > stand_z + 2.5:
                    game.lua('coop_press(57, 0)')
                    thrust = False
                elif not thrust and p[2] < stand_z + 1:
                    game.lua('coop_press(57, 12000)')
                    thrust = True
            elif time.monotonic() - best_t > 3:
                break                       # out of fuel, or held under a hatch that stays shut
            time.sleep(.1)
        game.lua('coop_press(57, 0)')
        time.sleep(2)
        p = game.pos()
        return abs(p[2] - stand_z) < 2.5, p

    def descend():
        nudge(game, (cx, cy), tol=2)        # the hatch below opens: down to the bottom of the column
        time.sleep(4)
        nudge(game, ring, tol=1.5)          # off the light's top to the floor
        time.sleep(2)

    if here[2] < pad['pos'][2] - 6:
        nudge(game, (cx, cy), tol=1.5)      # below the pad's floor (fell in a pit): up the open centre line
        ok, p = jet_to(game, pad['pos'][2], pad['pos'][:2])
        log(f'    shaft: up out of the pit to the pad: {"yes" if ok else "no"} at {[round(v, 1) for v in p]}')
    fuelled = False
    for _ in range(max_trips):
        shut = [(i, d) for i, d in enumerate(doors) if any(alive(game, e) for e in locks.get(d['name'], []))]
        if not fuelled:
            if game.pos()[2] > pad['pos'][2] + 3:
                descend()
            if not mount_refueller(game, pad, log):
                return False
            fuelled = True
        if not shut:
            ok, p = burn(goal[2], goal[:2])
            if ok:
                p = nudge(game, goal[:2], tol=3)     # the burn lets go within a couple of units
            log(f'    shaft: burn to the top: {"there" if ok else "short"} at {[round(v, 1) for v in p]}')
            if ok:
                return True
            fuelled = False
            continue
        i, door = shut[0]
        if i == 0:
            log(f'    shaft: {door["name"]} is locked with no hatch under it to stand on')
            return False
        floor = doors[i - 1]['pos'][2] + HATCH_TOP
        ok, p = burn(floor, land_xy)
        log(f'    shaft: up to the bay under {door["name"]}: {"landed" if ok else "missed"} at {[round(v, 1) for v in p]}')
        if not ok:
            fuelled = False                 # short of fuel (or knocked off): down, refuel, again
            continue
        # the turrets from the hatch, fire held (the owner killed a bay's four in about five seconds
        # with every arm firing)
        game.lua('coop_press(256, 60000)')
        for e in locks[door['name']]:
            o = next((o for o in level.m['objects'] if o['name'] == e), None)
            t0 = time.monotonic()
            while o and alive(game, e) and time.monotonic() - t0 < 40:
                x, y, z = o['pos']
                game.lua(f'local p=coop.real_player() coop.Face(p,{x}-p.position.x,{y}-p.position.y,'
                         f'{z}-p.position.z-3)')
                time.sleep(.3)
            log(f'    shaft: {e} {"dead" if not alive(game, e) else "still alive"} after {time.monotonic() - t0:.0f}s')
        game.lua('coop_press(256, 0) local p=coop.real_player() coop.Face(p,1,0,0)')
    return False


def climb(game, target, seconds=9):
    """A powered climb with Max's jetpack: one continuous hold of jump, the hero turned to the goal and
    walking at it. Returns the highest z reached."""
    game.lua('coop.pilot=nil')
    game.lua('coop_press(57, 8000)')
    top, t0 = game.pos()[2], time.monotonic()
    while time.monotonic() - t0 < seconds:
        p = game.pos()
        top = max(top, p[2])
        if p[2] >= target[2] + 1:
            break
        game.lua(f'local p=coop.real_player() coop.Face(p,{target[0]}-p.position.x,'
                 f'{target[1]}-p.position.y,0) coop_press(17,250)')
        time.sleep(.3)
    game.lua('coop_press(57, 0)')
    return top


def prepare_flight(game, level, nav, log):
    """Max: pick up a jetpack lying within reach, then stand on the nearest reachable refueller."""
    here = game.pos()
    cells = reachable(nav, here)
    near = lambda p: min(cells, key=lambda k: math.dist(nav.cells[k][:3], p))
    for o in level.m['objects']:
        if o['type'] in ('OBJ_JETPACK', 'OBJ_ATOMICJETPACK') and alive(game, o['name']):
            k = near(o['pos'])
            if math.dist(nav.cells[k][:3], o['pos']) < 3:
                walk(game, nav, o['pos'], log, timeout=40)
                game.goto(*o['pos'], radius=0.6, timeout=10, explore=0)   # a pickup needs the hero on it
                # standing on the spot is not always inside the pickup's volume (l2_r5jetpack): jump there
                for _ in range(3):
                    if not alive(game, o['name']):
                        break
                    game.lua('coop_press(57, 200)')
                    time.sleep(1.2)
                log(f'    picked up {o["name"]}: {"yes" if not alive(game, o["name"]) else "no"}')
    fuel = [o for o in level.m['objects'] if o['type'] == 'OBJ_REFUELLER']
    fuel = [o for o in fuel if math.dist(nav.cells[near(o['pos'])][:3], o['pos']) < 4]
    if not fuel:
        return False
    best = min(fuel, key=lambda o: math.dist(o['pos'], here))
    walk(game, nav, best['pos'], log, timeout=40)
    ok = mount_refueller(game, best, log)
    log(f'    refuelled at {best["name"]}: {"yes" if ok else "no"}')
    return ok


def advance(game, level, goal, out, log, max_rounds=25, fly=False, chute=False):
    """Get the hero to goal: portal by portal, scanning, walking, and breaking scripted walls."""
    game.lua(f'coop.GunnerOn({game.gun_range})')
    here = game.pos()
    _, portals = level.path(list(here), list(goal))
    # the room boxes have no vertical links (level 2's shaft from room 5 up to room 6): when the goal is
    # near and the room route starts by going away from it, head for the goal and let the scan find the way
    if portals and math.dist(here, goal) < 200 and \
            math.dist(portals[0]['to'], goal) > math.dist(here, goal) + 20:
        log(f'  room route leads away (via {portals[0]["room"]}): straight for the goal instead')
        portals = [{'to': list(goal), 'room': level.room_of(list(goal)), 'via': 'goal'}]
    stats = {'scans': 0, 'walks': 0, 'destroyed': [], 'rounds': 0, 'skipped': 0}
    game.level = level
    tried_from = {}
    if level.m.get('level') in (2, 5, 8, 11):
        max_arm(game, log)
    if fly:
        prepare_flight(game, level, scan(game, out, fly=True)[0], log)
    index = 0
    while index < len(portals):
        target = portals[index]['to']
        stalls = 0
        best = math.dist(game.pos()[:2], target[:2])
        # arrived: near in the plane and in height (the shaft's goal is straight above its floor)
        final = index == len(portals) - 1    # portal points sit mid-height in a doorway: only the goal's height counts
        while math.dist(game.pos()[:2], target[:2]) > 4 or (final and abs(game.pos()[2] - target[2]) > 6):
            # already further along (carried, or walked through): go on from the latest portal whose
            # room the hero is in, instead of walking back --
            room = level.room_of(list(game.pos()))
            later = [i for i in range(index + 1, len(portals)) if portals[i]['room'] == room]
            if later:
                stats['skipped'] += later[-1] - index
                log(f'  already in {room}: on from portal {later[-1]}')
                index = later[-1]
                target = portals[index]['to']
                best = math.dist(game.pos()[:2], target[:2])
                continue
            stats['rounds'] += 1
            if stats['rounds'] > max_rounds:
                return False, stats
            for attempt in range(5):
                nav, state = scan(game, out, fly=fly, chute=chute)
                stats['scans'] += 1
                if nav.cells:
                    break
                log(f'    scan found no floor under the hero ({state.strip()}): waiting')
                time.sleep(2)
            else:
                return False, stats
            # generators keep spawning enemies (level 2's arena shoved the hero 60 units at a time):
            # any live one the hero can get near is killed first
            # only the ones in the hero's room or by the way on (not back down the level)
            hero_room = level.room_of(list(game.pos()))
            for gen in [o for o in level.m['objects'] if 'GENERATOR' in o['type']
                        and o['name'] not in stats['destroyed']
                        and (o['room'] == hero_room or math.dist(o['pos'][:2], target[:2]) < 120)]:
                if not alive(game, gen['name']):
                    continue
                # a generator without an OnDie handler gates nothing (room 6's three); one that failed
                # from near here is not tried again from here
                where = tried_from.get(gen['name'])
                if where and math.dist(where, game.pos()) < 15:
                    continue
                gspot = rim_toward(nav, game.pos(), gen['pos'])
                if math.dist(gspot, gen['pos']) > 25:
                    continue
                log(f'    generator {gen["name"]} first')
                walk(game, nav, gspot, log, timeout=60)
                if aimed_kill(game, gen['name'], log):
                    stats['destroyed'].append(gen['name'])
                else:
                    tried_from[gen['name']] = tuple(game.pos())
            if collect(game, level, nav, log) and level.m.get('level') in (2, 5, 8, 11):
                max_arm(game, log)
            before = game.pos()
            spot = reachable_toward(nav, before, target)
            pos, state = walk(game, nav, spot, log)
            stats['walks'] += 1
            if state == 'fell' and fly:
                prepare_flight(game, level, scan(game, out, fly=True)[0], log)   # the way back up burnt fuel
            # progress is a new best distance to the target: walking back what a push lost is not
            gained = best - math.dist(pos[:2], target[:2])
            best = min(best, math.dist(pos[:2], target[:2]))
            log(f'  toward {[round(v) for v in target]}: {state}, {gained:+.1f} past the best, now {[round(v, 1) for v in pos]}')
            if gained > 2:
                stalls = 0
                continue
            stalls += 1
            # the goal straight above (level 2's shaft, 118 up): a powered climb, jump held while steering
            here = game.pos()
            if fly and target[2] - here[2] > 10 and math.dist(here[:2], target[:2]) < 40:
                # fuel first (anything on the way burns it), then what works by hand: one long hold of
                # jump while turning to and walking at the goal
                prepare_flight(game, level, nav, log)
                shaft = find_shaft(level, game.pos(), target)
                if shaft:
                    log(f'    shaft at {shaft[0]:.0f}, {shaft[1]:.0f}, lips {[round(f, 1) for f in shaft[2]]}')
                    if bay_climb(game, level, shaft, target, log):
                        stalls = 0
                        continue
                log(f'    climbing with the jetpack to {[round(v) for v in target]}')
                z0 = game.pos()[2]
                top = climb(game, target)
                log(f'    climb: up {top - z0:.1f}, now at {[round(v, 1) for v in game.pos()]}')
                if game.pos()[2] > z0 + 10:
                    stalls = 0
                    continue
            if fly and stalls == 1 and prepare_flight(game, level, nav, log):
                continue
            # a door opens as the hero comes near, after the scan saw it shut: push on straight toward
            # the target for a few seconds, then look again --
            if stalls == 1:
                r = game.goto(*target, radius=4, timeout=6, explore=0)
                moved = best - math.dist(game.pos()[:2], target[:2])
                best = min(best, math.dist(game.pos()[:2], target[:2]))
                log(f'    pushed toward the target: {moved:+.1f} past the best')
                if moved > 2:
                    stalls = 0
                    continue
            walls = [o for o in level.m['objects'] if o['type'] == 'OBJ_DESTRUCTABLE'
                     and math.dist(o['pos'][:2], pos[:2]) < 150 and alive(game, o['name'])]
            broken = walls and destroy(game, walls, target, log)
            if broken:
                stats['destroyed'].append(broken['name'])
                time.sleep(1.5)
                continue
            # the level scripts' own chains first: prerequisites, then the opener with its damage type
            if stalls <= 2 and chain_step(game, level, nav, pos, target, log, chute=chute):
                continue
            # what the level is waiting for: the nearest living scripted objective the hero can reach
            # a view of (the arena's exit opens when its three generators are gone) --
            done = False
            for obj in objectives(level, pos, target):
                # given up from somewhere else: worth another try from here (room 1's door lock is out of
                # view from the room, plain from in front of the door)
                where = tried_from.get(obj['name'])
                if obj['name'] in stats['destroyed'] or (where and math.dist(where, pos) < 15):
                    continue
                if not alive(game, obj['name']):
                    continue
                if kill_objective(game, nav, obj, log):
                    stats['destroyed'].append(obj['name'])
                    done = True
                    break
                tried_from[obj['name']] = tuple(pos)
                if obj['name'] not in stats.setdefault('gave_up', []):
                    stats['gave_up'].append(obj['name'])
            if not done:
                # nothing seen from known positions: shoot around and see what is hit
                near = [o for o in objectives(level, pos, target, radius=250) if alive(game, o['name'])]
                wanted = {o['name'] for o in near}
                # a generator is registered where it is (a wall at its pivot is not): go close first
                gens = [o for o in near if 'GENERATOR' in o['type']]
                gone = None
                for gen in gens:
                    spot = reachable_toward(nav, game.pos(), gen['pos'])
                    if math.dist(spot, gen['pos']) > 25:
                        continue
                    log(f'    going near {gen["name"]} to {[round(v) for v in spot]}')
                    walk(game, nav, spot, log, timeout=60)
                    if aimed_kill(game, gen['name'], log):
                        gone = gen['name']
                        break
                if not gone:
                    gone = wanted and fire_sweep(game, wanted, log, pitches=(0, -20, 20, -40, 40, 60))
                if gone:
                    stats['destroyed'].append(gone)
                    done = True
            if done:
                stalls = 0
                continue
            if stalls >= 3:
                log(f'  stalled toward {target}')
                return False, stats
        index += 1
    stats['telemetry'] = game.telemetry()
    return True, stats


def dump_state(game, folder, tag):
    """A coop_state dump kept in the run's evidence."""
    folder = Path(folder)
    folder.mkdir(parents=True, exist_ok=True)
    game.serial += 1
    name = f'{tag}_{game.slot}'
    path = game.coop / f'state_{name}.txt'
    path.unlink(missing_ok=True)
    game.lua(f'coop_state("{name}")')
    deadline = time.monotonic() + 5
    while not path.exists():
        if time.monotonic() > deadline:
            raise TimeoutError(f'game {game.slot}: no state dump')
        time.sleep(.05)
    out = folder / f'{name}.txt'
    # the bot's own invisible probes (coop_nav_a/b, coop_pilot_probe) exist only in the game that
    # made them: they are not part of the world the games share
    rows = [l for l in path.read_text(encoding='latin-1').splitlines()
            if not (l.startswith('item ') and l.split()[-1].startswith(('coop_nav_', 'coop_pilot_')))]
    out.write_text('\n'.join(rows) + '\n', encoding='latin-1')
    path.unlink(missing_ok=True)
    return out


def compare_worlds(games, folder, tag, log):
    """statecheck.py over every game's dump: the host's world against each client's."""
    import subprocess
    # a mismatch is looked at again 1.5 s later, as tools/replay.sh does: one sample can catch a fast
    # mover mid-interpolation; both results are kept
    for attempt in (1, 2):
        dumps = [str(dump_state(g, folder, f'{tag}{"" if attempt == 1 else "_again"}')) for g in games]
        r = subprocess.run([sys.executable, str(Path(__file__).with_name('statecheck.py')), *dumps],
                           capture_output=True, text=True)
        last = (r.stdout.strip().splitlines() or ['no output'])[-1]
        log(f'  worlds at {tag}{"" if attempt == 1 else " (again)"}: {last}')
        if r.returncode == 0:
            break
        (Path(folder) / f'statecheck_{tag}_{attempt}.txt').write_text(r.stdout)
        time.sleep(1.5)
    return r.returncode == 0, r.stdout


def compare_step(games, folder, tag, log):
    """compare_worlds, its last statecheck output sorted by the tolerance table (tools/campaign_coop.py)."""
    import campaign_coop
    try:
        ok, text = compare_worlds(games, folder, tag, log)
    except (TimeoutError, OSError) as e:      # a game that cannot dump (crashed, hung) is a finding too
        return {'tag': tag, 'ok': False, 'findings': [f'FAIL  no state dump: {e}'], 'known': [], 'warn': [],
                'summary': 'no dump'}
    c = campaign_coop.classify(text)
    return {'tag': tag, 'ok': ok, **c}


def down_mutant(games, log):
    """The revive's mutant: a lethal hit on the last game's hero (a client) puts it down at once, as
    tools/qa_revive.sh does. The squad must revive it with ordinary controls (a teammate holding R)."""
    client = games[-1]
    client.lua('mdkDealDamage(nil, mdkGetPlayerGob(), 999, DAMAGE_BADGUY, -1)')
    log(f'  MUTANT down: a lethal hit on player {client.slot}')
    return {'downed': client.slot}


def desync_mutant(games, log):
    """The world comparison's mutant: delete a living enemy in one client's game only (it stays alive on
    the host). The next step's comparison must report it as a finding."""
    host, client = games[0], games[1]
    rows = host.state()
    for r in rows:
        if r and r[0] == 'ent' and r[2] != 'gone' and int(r[3]) > 0:
            gid = r[1]
            name = ' '.join(r[7:])
            client.lua(f'local g=coop_gob("n{gid}") if (g) then omGobDelete(g) end')
            log(f'  MUTANT desync: deleted enemy {gid} {name} in game {client.slot} only')
            return {'deleted': gid, 'name': name, 'game': client.slot}
    log('  MUTANT desync: no living enemy to delete')
    return {'deleted': None}


def squad(games, level, goal, folder, log, fly=False, chute=False, route=None, record=None,
          no_god=False, step_units=25.0, step_seconds=45.0, mutant=None, roles=False, tactic='fight'):
    """Every game is a bot: player 1 leads to the goal; each other player follows the leader with its
    own pilot and gunner. The worlds are compared after every step of the leader's progress (each
    step_units along the ground, or step_seconds) and at the end. Without God mode (no_god) each hero looks after
    its health, a standing teammate revives a downed one (holding R beside it, bridge/revive.c) and
    downs, revives and deaths are counted. With roles, a step with several objectives is split: each
    player after the leader takes a different one, then all regroup at the goal."""
    import threading
    import campaign_coop as cc
    leader, followers = games[0], games[1:]
    for g in games:
        g.gun_range = 6 if tactic == 'run' else 60       # run: shoot only what blocks the way, keep moving
    result = {}
    stop = threading.Event()
    t_start = time.monotonic()
    steps, care = [], {'heals': 0, 'retreats': 0, 'revive_tries': 0, 'revive_ok': 0, 'covers': 0, 'cover_s': 0,
                'holds': 0, 'hold_s': 0, 'focus_changes': 0}
    lives = cc.Lives([g.slot for g in games])
    folder = Path(folder)
    folder.mkdir(parents=True, exist_ok=True)
    assigned = {}
    if roles:
        start = leader.pos()
        top = [o for o in objectives(level, [(a + b) / 2 for a, b in zip(start, goal)], goal,
                                     radius=math.dist(start[:2], goal[:2]) / 2 + 40)
               if abs(o['pos'][2] - goal[2]) < 20 and alive(leader, o['name'])]
        assigned = cc.split(top, len(games))
        log(f'  roles: objectives {[o["name"] for o in top]}; '
            f'{ {s: o["name"] for s, o in assigned.items()} or "nothing to split"}')

    def lead():
        try:
            run_lead()
        except Exception as e:           # a crash ends the squad instead of hanging it
            log(f'[1] crashed: {e!r}')
            result['leader'] = (False, {'crash': repr(e)})
        stop.set()

    def run_lead():
        if record:
            leader.lua(f'coop_route("record", "{record}")')
        if route:
            result['leader'] = follow(leader, route, lambda m: log('[1] ' + m), level=level,
                                      scratch=str(Path(folder) / 'nav_1.json'))
        else:
            result['leader'] = advance(leader, level, goal, str(Path(folder) / 'nav_1.json'),
                                       lambda m: log('[1] ' + m), fly=fly, chute=chute)
        if record:
            leader.lua(f'coop_route("stop", "{record}")')

    def keep_up(g):
        try:
            if route:
                # every player plays the recording itself (Max's flights cannot be chased on foot);
                # a player stuck on it walks on toward the leader
                ok, stats = follow(g, route, lambda m: log(f'[{g.slot}] ' + m), level=level,
                                   scratch=str(Path(folder) / f'nav_{g.slot}.json'))
                result[f'follower_{g.slot}'] = stats
                if ok:
                    take_role(g)
                    return
            elif g.slot in assigned:
                take_role(g)
            chase(g)
        except Exception as e:
            log(f'[{g.slot}] crashed: {e!r}')

    def take_role(g):
        obj = assigned.get(g.slot)
        if obj is None:
            return
        if alive(g, obj['name']):
            log(f'[{g.slot}] role: {obj["name"]} ({obj["type"]}), not the leader\'s')
            done = aimed_kill(g, obj['name'], lambda m: log(f'[{g.slot}] ' + m), seconds=60)
        else:
            done = None
        result[f'role_{g.slot}'] = {'objective': obj['name'], 'killed_by_me': done,
                                    'alive_on_host': alive(leader, obj['name'])}
        g.goto(*goal, radius=6, timeout=40, explore=1)     # regroup at the portal

    def chase(g):
        g.lua(f'coop.GunnerOn({g.gun_range})')
        far = 0
        while not stop.is_set():
            target = leader.pos()
            here = g.pos()
            if not target or not here:
                time.sleep(1)
                continue
            if math.dist(here, target) > 10:
                r = g.goto(*target, radius=6, timeout=8, explore=1)
                far = far + 1 if r.get('state') != 'arrived' else 0
            else:
                time.sleep(.5)
        result[g.slot] = far

    def watch():
        """A world comparison after every step of the leader's progress."""
        last_pos, last_t, n = None, time.monotonic(), 0
        while not stop.wait(1.0):
            try:
                here = leader.pos()
            except (TimeoutError, OSError):
                continue
            if here is None:
                continue
            if last_pos is None:
                last_pos = here
            # progress along the ground (a shaft climb is compared by time only)
            if math.dist(here[:2], last_pos[:2]) < step_units and time.monotonic() - last_t < step_seconds:
                continue
            n += 1
            s = compare_step(games, folder, f'step{n:02d}', log)
            s.update(step=n, t=round(time.monotonic() - t_start), leader=[round(v, 1) for v in here])
            steps.append(s)
            log(f'  step {n}: {len(s["findings"])} findings, {len(s["known"])} known, {len(s["warn"])} warn')
            (folder / 'steps.json').write_text(json.dumps(steps, indent=1))
            last_pos, last_t = here, time.monotonic()
            if mutant == 'desync' and n == 1:
                result['mutant'] = desync_mutant(games, log)
            if mutant in ('down', 'down-stay') and n == 2:
                result['mutant'] = down_mutant(games, log)

    def peek(g, read=Game.telemetry):
        """This game's hp and life (or another reading), or None while another thread drives it (a revive)."""
        if not g.lock.acquire(blocking=False):
            return None
        try:
            return read(g)
        except (TimeoutError, OSError, ValueError):
            return None
        finally:
            g.lock.release()

    reviving, last_heal, trail = set(), {}, {g.slot: [] for g in games}
    known_pos, known_life = {}, {}
    taken = set()

    def revive(g, d):
        """g walks to the downed player d and holds R beside it (ordinary controls)."""
        with g.lock:
            try:
                for attempt in range(3):
                    care['revive_tries'] += 1
                    body = d.pos()
                    if body is None:
                        break
                    g.goto(*body, radius=2.5, timeout=25, explore=1)
                    near = math.dist(g.pos(), body)
                    log(f'[{g.slot}] revive {d.slot}: try {attempt + 1}, {near:.1f} from the body')
                    if near > 4.5:
                        continue
                    g.lua('coop.gun_saved = coop.gunner coop.gunner = nil coop.PilotStop() coop_press(19, 4500)')
                    t1 = time.monotonic()
                    while time.monotonic() - t1 < 7:
                        if d.telemetry()['life'] == cc.ALIVE:
                            care['revive_ok'] += 1
                            log(f'[{g.slot}] revived player {d.slot} in {time.monotonic() - t1:.1f}s of R')
                            return
                        time.sleep(.3)
            except (TimeoutError, OSError) as e:
                log(f'[{g.slot}] revive {d.slot}: {e!r}')
            finally:
                try:
                    g.lua('if (coop.gun_saved) then coop.gunner = coop.gun_saved coop.gun_saved = nil end')
                except (TimeoutError, OSError):
                    pass
                reviving.discard(d.slot)

    snap = {'t': 0.0, 'host': {'players': {}, 'threats': {}}}
    busy_with = {}                     # slot -> 'cover' | 'hold' while a defence thread drives that hero
    ignored = {}                       # enemy id -> time until which holding for it is pointless

    def spot_of(slot):
        p = snap['host']['players']
        return p.get('me' if slot == 1 else slot)

    def heal(g, hp):
        """Low on health: the nearest health pickup if one is near (cover is the defence's job)."""
        with g.lock:
            here = g.pos()
            o = cc.health_target(level.m.get('pickups', []), here, taken)
            if o and alive(g, o['name']):
                taken.add(o['name'])
                g.goto(*o['pos'], radius=1.5, timeout=20, explore=1)
                care['heals'] += 1
                log(f'[{g.slot}] hp {hp}: went for {o["name"]} ({o["type"]}), hp now {g.telemetry()["hp"]}')

    def lualist(vals):
        return '{' + ', '.join(f'{v:.2f}' for p in vals for v in p) + '}'

    def cover(g, hp):
        """Being shot at with little health: out of every shooter's sight (a place the hero came from, or one
        around it that the engine's line of sight says is hidden), and wait there for the shooting to stop."""
        try:
            with g.lock:
                g.lua('coop.PilotStop()')
                here = g.pos()
                thr = [t['pos'] for t in snap['host']['threats'].get(g.slot, []) if math.dist(t['pos'], here) < 60]
                cands = cc.cover_candidates(here, trail[g.slot])[:24]
                if not thr or not cands:
                    return
                n = g.note('cover', f'coop.NavCover({lualist(cands)}, {lualist(thr)})').strip()
                if n == 'none':
                    log(f'[{g.slot}] hp {hp}: no cover found among {len(cands)} places from {len(thr)} shooters')
                    return
                spot = cands[int(n) - 1]
                r = g.goto(*spot, radius=1.5, timeout=8, explore=0)
                care['covers'] += 1
                t0 = time.monotonic()
                log(f'[{g.slot}] hp {hp}: took cover at {[round(v) for v in spot]} ({r.get("state")}), '
                    f'{math.dist(spot, here):.0f} units away')
                while time.monotonic() - t0 < 10 and not stop.is_set():
                    time.sleep(1)
                    if not [t for t in snap['host']['threats'].get(g.slot, [])
                            if math.dist(t['pos'], spot) < 45]:
                        break
                care['cover_s'] += round(time.monotonic() - t0)
        except (TimeoutError, OSError, ValueError) as e:
            log(f'[{g.slot}] cover: {e!r}')
        finally:
            busy_with.pop(g.slot, None)

    def hold(g, target):
        """A shooter at the hero within reach: stop walking and shoot it until it is dead, the hero's health
        calls for cover, or 12 s pass; a target that loses no health in 5 s is not worth holding for."""
        try:
            with g.lock:
                g.lua('coop.PilotStop()')
                t0 = last_hp_t = time.monotonic()
                hp0 = target['hp']
                care['holds'] += 1
                while time.monotonic() - t0 < 12 and not stop.is_set():
                    time.sleep(.5)
                    mine = {t['id']: t for t in snap['host']['threats'].get(g.slot, [])}
                    cur = mine.get(target['id'])
                    if cur is None:
                        break                                   # dead, or no longer after this hero
                    if cur['hp'] < hp0:
                        hp0, last_hp_t = cur['hp'], time.monotonic()
                    elif time.monotonic() - last_hp_t > 5:
                        ignored[target['id']] = time.monotonic() + 40
                        log(f'[{g.slot}] hold: {target["name"]} loses no health, not holding for it')
                        break
                    tel = known_life.get(g.slot)
                    if tel and 0 < tel['hp'] <= cc.COVER_HP:
                        break
                care['hold_s'] += round(time.monotonic() - t0)
        except (TimeoutError, OSError) as e:
            log(f'[{g.slot}] hold: {e!r}')
        finally:
            busy_with.pop(g.slot, None)

    def defend(g, t):
        """Read what is shooting this hero from the host's state; shoot the most urgent first; hold the ground
        to finish it, or take cover when health is low."""
        mine = snap['host']['threats'].get(g.slot, [])
        here = spot_of(g.slot)
        if not here:
            return
        focus = cc.pick_focus(mine, here, reach=g.gun_range - 5 if tactic == 'run' else 55.0)
        name = focus['name'] if focus else ''
        if name != focus_set.get(g.slot):
            focus_set[g.slot] = name
            care['focus_changes'] += 1
            try:
                g.lua(f'coop.GunnerFocus({json.dumps(name) if name else "nil"})')
            except (TimeoutError, OSError):
                pass
            if focus:
                log(f'[{g.slot}] shot at by {len(mine)}: focus {name} (type {focus["type"]}, hp {focus["hp"]}, '
                    f'{math.dist(focus["pos"], here):.0f} away), own hp {t["hp"]}')
        if g.slot in busy_with or g.slot in reviving or t['life'] != cc.ALIVE or tactic == 'run':
            return
        near_fire = [x for x in mine if math.dist(x['pos'], here) < 45]
        if 0 < t['hp'] <= cc.COVER_HP and near_fire and now_s() - last_heal.get(('cover', g.slot), -99) > 12:
            last_heal[('cover', g.slot)] = now_s()
            busy_with[g.slot] = 'cover'
            threading.Thread(target=cover, args=(g, t['hp']), daemon=True).start()
        elif focus and ignored.get(focus['id'], 0) < time.monotonic() and t['hp'] > cc.COVER_HP \
                and math.dist(focus['pos'], here) < 70 and now_s() - last_heal.get(('hold', g.slot), -99) > 15:
            last_heal[('hold', g.slot)] = now_s()
            busy_with[g.slot] = 'hold'
            threading.Thread(target=hold, args=(g, focus), daemon=True).start()

    focus_set = {}

    def now_s():
        return time.monotonic() - t_start

    def look_after():
        while not stop.wait(1.0):
            now = time.monotonic() - t_start
            seen = {}
            for g in games:
                t = peek(g)
                if t is None:
                    continue
                seen[g.slot] = t
                ev = lives.update(g.slot, t['life'], now)
                if ev:
                    log(f'  player {g.slot}: {ev} (hp {t["hp"]})')
            if not no_god:
                continue
            pos = {g.slot: peek(g, Game.pos) for g in games if g.slot in seen}
            for s, p in pos.items():
                if p:
                    trail[s] = (trail[s] + [p])[-10:]
                    known_pos[s] = p
            # a game that was busy this second (its pilot holds the command file) keeps its last reading
            for g in games:
                if g.slot not in seen and g.slot in known_life:
                    seen[g.slot] = known_life[g.slot]
                if pos.get(g.slot) is None and g.slot in known_pos:
                    pos[g.slot] = known_pos[g.slot]
            known_life.update({s: v for s, v in seen.items()})
            try:
                snap['host'] = cc.host_threats(leader.state())
                snap['t'] = now
            except (TimeoutError, OSError, ValueError):
                pass
            for g in games:
                t = seen.get(g.slot)
                if not t:
                    continue
                if t['life'] == cc.DOWNED and g.slot not in reviving and mutant != 'down-stay':
                    who = cc.reviver(pos, {s: v['life'] for s, v in seen.items()}, g.slot)
                    if not who and now - last_heal.get(('why', g.slot), -99) > 5:
                        last_heal[('why', g.slot)] = now
                        log(f'  player {g.slot} is down: nobody to revive it (positions {pos}, lives '
                            f'{ {s: v["life"] for s, v in seen.items()} })')
                    if who:
                        reviving.add(g.slot)
                        helper = next(x for x in games if x.slot == who)
                        log(f'  player {g.slot} is down: player {who} goes to revive it')
                        threading.Thread(target=revive, args=(helper, g), daemon=True).start()
                elif t['life'] == cc.ALIVE:
                    defend(g, t)
                    if 0 < t['hp'] <= cc.COVER_HP and g.slot not in busy_with and now - last_heal.get(g.slot, -99) > 20:
                        last_heal[g.slot] = now
                        threading.Thread(target=heal, args=(g, t['hp']), daemon=True).start()

    threads = [threading.Thread(target=lead)] + [threading.Thread(target=keep_up, args=(g,)) for g in followers]
    helpers = [threading.Thread(target=watch, daemon=True), threading.Thread(target=look_after, daemon=True)]
    for t in threads + helpers:
        t.start()
    for t in threads:
        t.join()
    stop.set()
    for t in helpers:
        t.join(timeout=60)
    regroup = None
    if roles or len(games) > 2:
        # regroup at the portal: everyone within 15 units of the goal
        t1 = time.monotonic()
        while time.monotonic() - t1 < 60:
            if cc.regrouped({g.slot: g.pos() for g in games}, goal):
                regroup = round(time.monotonic() - t1)
                break
            for g in games:         # the leader too: its route may end a few steps short of the portal
                if math.dist(g.pos(), goal) > 15:
                    g.goto(*goal, radius=6, timeout=10, explore=1)
        log(f'  regrouped at the goal: {"in %ss" % regroup if regroup is not None else "no"}')
        if regroup is None:
            log('  not at the goal (units): ' + str({g.slot: round(math.dist(g.pos(), goal), 1) for g in games}))
    end = compare_step(games, folder, 'end', log)
    end.update(step='end', t=round(time.monotonic() - t_start))
    steps.append(end)
    (folder / 'steps.json').write_text(json.dumps(steps, indent=1))
    gaps = {g.slot: round(math.dist(g.pos(), leader.pos()), 1) for g in followers}
    log(f'  followers\' distance from the leader at the end: {gaps}')
    followed = {k: v for k, v in result.items() if str(k).startswith(('follower_', 'role_', 'mutant'))}
    findings = sorted({f for s in steps for f in s['findings']})
    known = sorted({k[0] for s in steps for k in s['known']})
    totals = lives.totals()
    if mutant in ('down', 'down-stay'):
        result['mutant'] = dict(result.get('mutant', {}), **cc.down_check(totals, (result.get('mutant') or {}).get('downed')))
        followed['mutant'] = result['mutant']
        log(f'  mutant {mutant}: {result["mutant"]}')
    stats = {'leader': result['leader'][1], 'world_ok': not end['findings'],      # known drift (flyers) is not a difference
             'gaps': gaps, 'steps': len(steps),
             'findings': findings, 'known': len(known), 'lives': totals, 'care': care,
             'regroup_s': regroup, **followed}
    (folder / 'squad.json').write_text(json.dumps({'ok': result['leader'][0], **stats, 'known_lines': known,
                                                   'life_events': lives.events}, indent=1, default=str))
    # passing is getting through; a world that differs is a co-op finding, reported beside it
    return result['leader'][0], stats


# Keys that move or turn the hero: the pilot does that itself when following a recording.
MOVE_KEYS = {17, 30, 31, 32, 200, 203, 205, 208}


def load_route(path):
    """Frames of a recorded route (tools/replay.sh record): (ms, held key codes, level, cp, position)."""
    frames = []
    for line in Path(path).read_text().splitlines():
        f = line.split()
        if not f or f[0] != 'F':
            continue
        bits = bytes.fromhex(f[2])
        keys = {i * 8 + b for i, byte in enumerate(bits) for b in range(8) if byte >> b & 1}
        frames.append((int(f[1]), keys, int(f[6]), int(f[7]), tuple(float(v) for v in f[9:12])))
    return frames


def journal_segments(folder, out, log=print, jump=40.0, reach=12.0):
    """A person's play journals (coop/journal/L<level>_cp<cp>_<time>.route, [test] journal=1) as segment
    routes for campaign_run.py, seg_<level>_<from>_<to>.route for consecutive launcher checkpoints
    (tools/checkpoints.txt). The game does not report every checkpoint passed (level 2 played from 6 to
    its end came as one file), so a file is cut where the hero first comes within reach of each next
    checkpoint's position (local/campaign/level<N>.json). Deaths and reloads jump the hero back: a
    segment keeps only its last attempt. An existing segment route is replaced only by a shorter one."""
    root = Path(__file__).resolve().parents[1]
    order = [tuple(int(v) for v in t.split('-')) for t in (root / 'tools/checkpoints.txt').read_text().split()]
    made = []
    for f in sorted(Path(folder).glob('L*_cp*_*.route'), key=lambda f: f.name.split('_')[-1]):
        level = int(f.name[1:].split('_')[0])
        cp = int(f.name.split('_cp')[1].split('_')[0])
        if (level, cp) not in order:
            continue
        model = root / f'local/campaign/level{level}.json'
        cps = json.loads(model.read_text())['checkpoints'] if model.exists() else {}
        lines = f.read_text().splitlines()
        head = [l for l in lines if l.startswith(('MDKROUTE', 'S '))]
        frames = [l for l in lines if l.startswith('F ')]
        pos = [[float(v) for v in l.split()[9:12]] for l in frames]
        later = [c for (l, c) in order[order.index((level, cp)) + 1:] if l == level]
        start, here = 0, cp
        for nxt in later:
            goal = cps.get(str(nxt), {}).get('pos')
            if not goal:
                break
            hit = next((k for k in range(start, len(pos)) if math.dist(pos[k], goal) < reach), None)
            if hit is None:
                break
            seg = list(range(start, hit + 1))
            first = seg[0]
            for k in seg[1:]:
                if math.dist(pos[k - 1], pos[k]) > jump:
                    first = k               # died or reloaded: the last attempt is the one that got through
            part = frames[first:hit + 1]
            start, prev, here = hit, here, nxt
            if len(part) < 10:
                continue
            target = Path(out) / f'seg_{level}_{prev}_{nxt}.route'
            secs = (int(part[-1].split()[1]) - int(part[0].split()[1])) / 1000
            if target.exists():
                old = [l for l in target.read_text().splitlines() if l.startswith('F ')]
                if old and (int(old[-1].split()[1]) - int(old[0].split()[1])) / 1000 <= secs:
                    continue
            Path(out).mkdir(parents=True, exist_ok=True)
            target.write_text('\n'.join(head + part) + '\n')
            log(f'  {target.name}: {secs:.0f} s of play from {f.name}')
            made.append(target)
    return made


def route_waypoints(frames, step_ms=400, step_units=3.0):
    """Points every step_ms or step_units along the recording, each with the action keys (not movement)
    held on the way to it, and the checkpoint the recording was at."""
    out, held, last = [], set(), None
    ground, steady, prev_z, jumped = None, 0, None, False
    for ms, keys, level, cp, pos in frames:
        held |= keys - MOVE_KEYS
        # airborne: above the last ground stood on (a few steady frames without jump) after a jump --
        # flights and glides the follower must fly, not walk
        if prev_z is not None and abs(pos[2] - prev_z) < 0.02 and 57 not in keys:
            steady += 1
            if steady >= 8:
                ground, jumped = pos[2], False
        else:
            steady = 0
        if 57 in keys:
            jumped = True
        prev_z = pos[2]
        air = jumped and ground is not None and pos[2] > ground + 2
        # a point per step_units moved (or a little movement over a while); standing still adds none,
        # its held keys (firing at a wall) go with the next point
        moved = math.dist(pos, last[1]) if last else step_units
        if last is None or moved >= step_units or (moved >= 0.5 and ms - last[0] >= 5 * step_ms):
            # a lift ride (level 2 room 9's rising platforms): height changed, the hero stood still and
            # held no jump; the follower waits there for the lift instead of walking on
            ride = bool(last) and math.dist(pos[:2], last[1][:2]) < 1.0 and abs(pos[2] - last[1][2]) >= 2 \
                and 57 not in held
            out.append({'ms': ms, 'pos': pos, 'keys': sorted(held), 'level': level, 'cp': cp, 'ride': ride,
                        'air': air and not ride})
            held, last = set(), (ms, pos)
    if frames and (not out or out[-1]['pos'] != frames[-1][4]):
        out.append({'ms': frames[-1][0], 'pos': frames[-1][4], 'keys': [], 'level': frames[-1][2], 'cp': frames[-1][3],
                    'ride': False, 'air': False})
    return out


def follow(game, path, log, radius=2.5, lost_after=120, level=None, scratch=None):
    """Play a recording closed-loop: walk its positions with the pilot (skipping on when nearer a later
    point), pressing the action keys the recording held on each stretch. Robust to timing and drift,
    unlike replaying the inputs by the clock."""
    pts = route_waypoints(load_route(path))
    log(f'  following {len(pts)} points of {path}')
    game.lua(f'coop.GunnerOn({game.gun_range})')
    i, t0, last_progress, best = 0, time.monotonic(), time.monotonic(), None
    helped, aimed, flybacks, fuelled_at = set(), set(), {}, {}
    while i < len(pts):
        here = game.pos()
        if math.dist(here, pts[-1]['pos']) < 6:
            break                           # at the recording's end (the next checkpoint) already
        # a wipe puts the squad back at its checkpoint, far behind the point it was walking to: rewind to
        # the nearest earlier point (otherwise the pilot pushes at walls 100 units short of it)
        if math.dist(here, pts[i]['pos']) > 30 and i > 0:
            k = min(range(max(0, i - 300), i), key=lambda q: math.dist(here, pts[q]['pos']))
            if math.dist(here, pts[k]['pos']) < math.dist(here, pts[i]['pos']) - 20:
                log(f'  {math.dist(here, pts[i]["pos"]):.0f} units behind point {i} (a checkpoint reload?): '
                    f'back to point {k}')
                i, last_progress, best = k, time.monotonic(), None
        # skip on: the nearest of the next few points
        ahead = range(i, min(len(pts), i + 80))
        j = min(ahead, key=lambda k: math.dist(here, pts[k]['pos']))
        if j > i:
            i = j
        p = pts[i]
        if p.get('ride'):
            # on a lift: stand still until it brings the hero to the height the ride ended at; give up
            # waiting when the hero's height stops changing (not on the lift, or it is the wrong way)
            j = i
            while j + 1 < len(pts) and pts[j + 1].get('ride'):
                j += 1
            want = pts[j]['pos'][2]
            t1 = moved_t = time.monotonic()
            z0 = game.pos()[2]
            while abs(game.pos()[2] - want) > 1.5 and time.monotonic() - t1 < 45:
                z = game.pos()[2]
                if abs(z - z0) > 0.3:
                    z0, moved_t = z, time.monotonic()
                elif time.monotonic() - moved_t > 6:
                    break
                time.sleep(.3)
            z = game.pos()[2]
            log(f'  lift ride at points {i}-{j}: {"rode to" if abs(z - want) <= 1.5 else "stopped at"} '
                f'{z:.1f} (recorded {want:.1f})')
            i = j + 1
            last_progress, best = time.monotonic(), None
            continue
        if here[2] < p['pos'][2] - 6 and flybacks.get(i, 0) < 3 and \
                (57 in p['keys'] or any(57 in q['keys'] for q in pts[max(0, i - 8):i])):
            flybacks[i] = flybacks.get(i, 0) + 1
            # below the recording where it flew or glided (room 6: across the shaft's open top on
            # momentum): fly back to the point from the open centre line of a shaft, or a column found open
            col = (level and level.shaft_near(here)) or open_column(game, here, p['pos'][2])
            if col:
                nudge(game, col, tol=1.5)
            ok, at = jet_to(game, p['pos'][2], p['pos'][:2])
            log(f'  below the recording at point {i}: flew back up {"to it" if ok else "short"}, now '
                f'{[round(v, 1) for v in at]}')
            if ok:
                last_progress, best = time.monotonic(), None
                continue
        if level:
            # the recording stood on a refueller (it fills only there, while standing): do the same
            pad = next((o for o in level.m['objects'] if o['type'] == 'OBJ_REFUELLER'
                        and math.dist(o['pos'], p['pos']) < 2.5 and i - fuelled_at.get(o['name'], -99) > 30), None)
            if pad:
                fuelled_at[pad['name']] = i
                ok = mount_refueller(game, pad, log)
                log(f'  refuelled at {pad["name"]} as the recording did: {"yes" if ok else "no"}')
        if p.get('air'):
            # the recording was in the air here (a flight, or a glide over a gap): fly to the point at
            # its height instead of walking off an edge
            more = i + 1 < len(pts) and pts[i + 1].get('air')
            ok, at = jet_to(game, p['pos'][2], p['pos'][:2], seconds=6, over=0.5, land=not more)
            if ok or math.dist(at, p['pos']) < radius + 1:
                i += 1
                last_progress, best = time.monotonic(), None
                continue
        if 256 in p['keys'] and level:
            # the recording was shooting here: what it shot is most likely the nearest living scripted
            # objective (the arena's generators: one left at 16 hp kept its exit shut), so aim at it
            near = [o for o in objectives(level, here, None, radius=50)
                    if o['name'] not in aimed and math.dist(o['pos'], here) < 50]
            near = [o for o in near if alive(game, o['name'])][:1]
            for o in near:
                aimed.add(o['name'])
                aimed_kill(game, o['name'], log, seconds=30)
                game.lua(f'coop.GunnerOn({game.gun_range})')
        for code in p['keys']:
            game.lua(f'coop_press({code if code < 256 else code}, 450)')
        rise = p['pos'][2] - here[2]
        r = game.goto(*p['pos'], radius=radius, timeout=6, explore=0, jump=int(0.7 < rise <= 3.5),
                      fly=int(57 in p['keys'] and rise > 3.5))
        if r.get('state') == 'arrived' or math.dist(game.pos(), p['pos']) < radius + 1:
            i += 1
            last_progress, best = time.monotonic(), None
            continue
        # the pilot would not move (within its own idea of arrival, or turned away): a few plain steps
        nudge(game, p['pos'][:2], tol=radius, tries=6)
        # nearer the point than ever before counts as progress (a fight or a wall can take a while)
        d = math.dist(game.pos(), p['pos'])
        if best is None or d < best - 1:
            best, last_progress = d, time.monotonic()
        # held up a while with the level model at hand: what the bot does on its own, once per spot --
        # the scripted objectives near (room 1's door lock, shot from further back in the recording)
        if level and scratch and time.monotonic() - last_progress > 25 and Nav.spot(here) not in helped:
            helped.add(Nav.spot(here))
            nav, _ = scan(game, scratch)
            if nav.cells:
                for obj in objectives(level, here, p['pos'], radius=40)[:4]:
                    if alive(game, obj['name']) and kill_objective(game, nav, obj, log):
                        break
            continue
        # held up where the recording was shooting (a scripted wall in the way): fire along the way
        if any(256 in q['keys'] for q in pts[max(0, i - 10):i + 3]):
            for _ in range(6):
                game.lua(f'local p=coop.real_player() coop.Face(p, {p["pos"][0]}-p.position.x, '
                         f'{p["pos"][1]}-p.position.y, 0) coop_press(256, 600)')
                time.sleep(.5)
        if time.monotonic() - last_progress > lost_after:
            log(f'  lost at point {i}/{len(pts)} {p["pos"]}, hero at {here}')
            try:
                rows = game.state()
                tg = {r[1]: r[3] for r in rows if r[0] == 'ai_target' and len(r) >= 4}
                near = sorted((math.dist((float(r[4]), float(r[5]), float(r[6])), here), r[7], r[2], r[3], tg.get(r[1], '-'))
                              for r in rows if r[0] == 'ent' and len(r) >= 8)[:6]
                log(f'  lost: hp {game.telemetry()["hp"]}, nearest enemies (dist, name, type, hp, target): '
                    f'{[(round(d), n, t, h, a) for d, n, t, h, a in near]}')
            except (TimeoutError, OSError, ValueError):
                pass
            return False, {'points': len(pts), 'reached': i, 'seconds': round(time.monotonic() - t0)}
    log(f'  followed all {len(pts)} points in {time.monotonic() - t0:.0f}s')
    return True, {'points': len(pts), 'reached': len(pts), 'seconds': round(time.monotonic() - t0)}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    g = sub.add_parser('goto')
    g.add_argument('--dirs', nargs='+', required=True)
    g.add_argument('--player', type=int, default=1)
    g.add_argument('--to', nargs=3, type=float, required=True, metavar=('X', 'Y', 'Z'))
    g.add_argument('--radius', type=float, default=2.5)
    t = sub.add_parser('travel')
    t.add_argument('--dirs', nargs='+', required=True)
    t.add_argument('--player', type=int, default=1)
    t.add_argument('--level', required=True, help='mined model, e.g. local/campaign/level2.json')
    t.add_argument('--to', nargs=3, type=float, required=True, metavar=('X', 'Y', 'Z'))
    sc = sub.add_parser('scan')
    sc.add_argument('--dirs', nargs='+', required=True)
    sc.add_argument('--player', type=int, default=1)
    sc.add_argument('--out', required=True)
    w = sub.add_parser('walk')
    w.add_argument('--dirs', nargs='+', required=True)
    w.add_argument('--player', type=int, default=1)
    w.add_argument('--nav', required=True)
    w.add_argument('--to', nargs=3, type=float, required=True, metavar=('X', 'Y', 'Z'))
    ad = sub.add_parser('advance')
    ad.add_argument('--dirs', nargs='+', required=True)
    ad.add_argument('--player', type=int, default=1)
    ad.add_argument('--level', required=True)
    ad.add_argument('--to', nargs=3, type=float, required=True, metavar=('X', 'Y', 'Z'))
    ad.add_argument('--fly', action='store_true', help="Max's jetpack: refuel and fly")
    ad.add_argument('--chute', action='store_true', help="Kurt's chute: long drops")
    ad.add_argument('--scratch', default=str(Path(__file__).resolve().parents[1] / 'local/campaign/nav/scratch.json'))
    sq = sub.add_parser('squad')
    sq.add_argument('--dirs', nargs='+', required=True)
    sq.add_argument('--level', required=True)
    sq.add_argument('--to', nargs=3, type=float, required=True, metavar=('X', 'Y', 'Z'))
    sq.add_argument('--fly', action='store_true', help="Max's jetpack: refuel and fly")
    sq.add_argument('--chute', action='store_true', help="Kurt's chute: long drops")
    sq.add_argument('--route', help='follow this recording instead of exploring')
    sq.add_argument('--record', help="record the leader's play under this name (kept by the game in coop/)")
    sq.add_argument('--evidence', default=str(Path(__file__).resolve().parents[1] / 'local/campaign/runs'))
    sq.add_argument('--no-god', action='store_true', help='heroes mind their health and revive each other')
    sq.add_argument('--tactic', choices=('fight', 'run'), default='fight',
                    help='no-God play: fight (hold the ground, cover) or run (shoot only what is close)')
    sq.add_argument('--roles', action='store_true', help='split a step\'s objectives between the players')
    sq.add_argument('--step-units', type=float, default=25.0, help='compare the worlds after this much progress')
    sq.add_argument('--mutant', choices=['desync', 'down', 'down-stay'],
                    help='desync: delete an enemy in one client only (must be reported); down: a lethal hit on a client '
                         '(must be revived); down-stay: the same without the revive (the player must be reported dead)')
    fo = sub.add_parser('follow', help='play a recorded route closed-loop')
    fo.add_argument('--dirs', nargs='+', required=True)
    fo.add_argument('--player', type=int, default=1)
    fo.add_argument('--route', required=True)
    fo.add_argument('--level', help='mined model: objective killing and refuellers while following')
    jo = sub.add_parser('journal', help="turn a person's play journals into segment routes")
    jo.add_argument('folder', help='a game copy\'s coop/journal folder (or a copy of it)')
    jo.add_argument('--out', default=str(Path(__file__).resolve().parents[1] / 'local/routes'))
    a = ap.parse_args(argv)
    if a.cmd == 'journal':
        made = journal_segments(a.folder, a.out)
        print(f'{len(made)} segment routes written to {a.out}')
        return 0
    games = [Game(d, i + 1) for i, d in enumerate(a.dirs)]
    if a.cmd == 'goto':
        trail = []
        r = games[a.player - 1].goto(*a.to, radius=a.radius, trail=trail)
        print(json.dumps(r))
        return 0 if r.get('state') == 'arrived' else 1
    if a.cmd == 'scan':
        t0 = time.monotonic()
        nav, state = scan(games[a.player - 1], a.out)
        links = sum(len(v[3]) for v in nav.cells.values())
        print(f'{state.strip()}: {len(nav.cells)} cells, {links} links in {time.monotonic() - t0:.0f}s -> {a.out}')
        return 0
    if a.cmd == 'walk':
        game = games[a.player - 1]
        nav = Nav.load(a.nav)
        here = game.pilot()
        if not here:
            game.lua('coop.PilotGo(0,0,0,1000000)')
            here = game.pilot()
        pts = nav.path(list(here['pos']), list(a.to))
        if not pts:
            print('no path in the scanned cells')
            return 1
        print(f'{len(pts)} waypoints', flush=True)
        t0 = time.monotonic()
        for i, pt in enumerate(pts[1:], 1):
            climb = pt[2] - pts[i - 1][2] > Nav.WALK_RISE
            r = game.goto(*pt, radius=1.5 if i < len(pts) - 1 else 2.5, timeout=30, explore=0, jump=int(climb))
            if r.get('state') != 'arrived':
                print(f'  waypoint {i} {pt}: {r.get("state")} at {r.get("pos")}')
                return 1
        print(f'arrived in {time.monotonic() - t0:.1f}s at {game.pos()}')
        return 0
    if a.cmd == 'follow':
        ok, stats = follow(games[a.player - 1], a.route, lambda m: print(m, flush=True),
                           level=Level(a.level) if a.level else None,
                           scratch=str(Path(a.route).with_suffix('.nav.json')) if a.level else None)
        stats['telemetry'] = games[a.player - 1].telemetry()
        print(f'{"ARRIVED" if ok else "FAILED"}: {stats}')
        return 0 if ok else 1
    if a.cmd == 'squad':
        t0 = time.monotonic()
        folder = Path(a.evidence) / time.strftime('%Y%m%d-%H%M%S')
        ok, stats = squad(games, Level(a.level), a.to, folder, lambda m: print(m, flush=True), fly=a.fly,
                          chute=a.chute, route=a.route, record=a.record, no_god=a.no_god, roles=a.roles,
                          step_units=a.step_units, mutant=a.mutant, tactic=a.tactic)
        print(f'{"RESULT: PASS" if ok else "RESULT: FAIL"} in {time.monotonic() - t0:.0f}s: {stats}; evidence {folder}')
        return 0 if ok else 1
    if a.cmd == 'advance':
        t0 = time.monotonic()
        ok, stats = advance(games[a.player - 1], Level(a.level), a.to, a.scratch, lambda m: print(m, flush=True), fly=a.fly,
                            chute=a.chute)
        stats.setdefault('telemetry', games[a.player - 1].telemetry())
        print(f'{"ARRIVED" if ok else "FAILED"} in {time.monotonic() - t0:.0f}s: {stats}')
        return 0 if ok else 1
    if a.cmd == 'travel':
        game = games[a.player - 1]
        here = game.pilot()
        if not here:
            game.lua('coop.PilotGo(0,0,0,1000000)')
            here = game.pilot()
        level = Level(a.level)
        start, steps = level.path(list(here['pos']), list(a.to))
        print(f'from {start}: ' + ' -> '.join(f"{st['room']}({st['via']})" for st in steps), flush=True)
        for st in steps:
            r = game.goto(*st['to'], radius=2.5 if st['via'] == 'goal' else 4, timeout=240)
            print(f"  {st['room']:<24} {r.get('state'):<8} {r.get('ms', 0) / 1000:5.1f}s stuck {r.get('stuck')} at {r.get('pos')}", flush=True)
            if r.get('state') != 'arrived':
                return 1
        return 0


if __name__ == '__main__':
    sys.exit(main())
