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

    def goto(self, x, y, z, radius=2.5, timeout=60, trail=None, explore=1, jump=0, fly=0, chute=0):
        self.lua(f'coop.PilotGo({x},{y},{z},{radius},1,{explore}) coop.pilot.max_stuck=40 coop.pilot.jump={jump} '
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

    def step_cost(self, k, n):
        a, b = self.cells[k], self.cells[n]
        rise = b[2] - a[2]
        base = math.dist(a[:3], b[:3])
        if -6 <= rise <= self.WALK_RISE:
            return base
        if rise < -6 and self.chute:
            return base * 2
        if 0 < rise <= self.JUMP_RISE and self.flat(n):
            return base * 4
        if self.fly and rise <= self.FLY_RISE and self.flat(n):
            return base * 6
        return None          # too steep, or a long fall without Kurt's chute

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
    x, y, z = game.pos()
    # flying opens up every floor above; a room and its shaft fit in 12000 cells
    game.lua(f'coop.NavStart({x},{y},{z},2,{12000 if fly else 40000},4,6,{36 if fly else 0},{300 if chute else 0})')
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
        r = game.goto(*pt, radius=1.5 if i < len(pts) - 1 else 2.5, timeout=timeout, explore=0,
                      jump=int(climb and not flying), fly=int(flying), chute=int(nav.chute and rise < -6))
        if r.get('state') != 'arrived':
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
        return (f'local p=coop.real_player() mdkGobPointToXYZ(p, {math.cos(a):.4f}, {math.sin(a):.4f}, '
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
    game.lua('coop.GunnerOn(60)')
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
    game.lua('coop.GunnerOn(60)')
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
        game.lua(f'local p=coop.real_player() local t=getglobal("{name}") if (t) then mdkGobPointToXYZ(p, '
                 f't.position.x-p.position.x, t.position.y-p.position.y, t.position.z+{dz}-p.position.z-1) '
                 f'coop_test_sniper() end')
        time.sleep(1)
        game.lua(f'local p=coop.real_player() local t=getglobal("{name}") if (t) then mdkGobPointToXYZ(p, '
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
            game.lua(f'local p=coop.real_player() local t=getglobal("{name}") if (t) then mdkGobPointToXYZ(p, '
                     f't.position.x-p.position.x, t.position.y-p.position.y, t.position.z+{dz}-p.position.z-3) '
                     f'coop_press(256, 300) end')
            time.sleep(.45)
            # upright again, same bearing (pointing up or down tips Max onto his side)
            game.lua(f'local p=coop.real_player() local t=getglobal("{name}") if (t) then '
                     f'mdkGobPointToXYZ(p, t.position.x-p.position.x, t.position.y-p.position.y, 0) end')
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
    game.lua('coop.GunnerOn(60)')
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
        log(f'    {obj["name"]}: no reachable place sees it')
        return False
    walk(game, nav, spot, log)
    t0 = time.monotonic()
    while time.monotonic() - t0 < seconds:
        if not alive(game, obj['name']):
            log(f'    killed {obj["name"]} ({obj["type"]}) in {time.monotonic() - t0:.1f}s')
            return True
        game.lua(f'local p=coop.real_player() mdkGobPointToXYZ(p, {x}-p.position.x, {y}-p.position.y, {aim}-p.position.z-3) '
                 f'coop_press(256, 600)')
        time.sleep(.5)
    log(f'    {obj["name"]}: still alive after {seconds}s of fire')
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
        game.lua(f'local p=coop.real_player() mdkGobPointToXYZ(p, {toward[0]}-p.position.x, {toward[1]}-p.position.y, 0) '
                 f'coop_press(256, 600)')
        time.sleep(.5)
    return None


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
    game.goto(*best['pos'], radius=1.0, timeout=10, explore=0)
    time.sleep(4)
    log(f'    refuelled at {best["name"]}')
    return True


def advance(game, level, goal, out, log, max_rounds=25, fly=False, chute=False):
    """Get the hero to goal: portal by portal, scanning, walking, and breaking scripted walls."""
    game.lua('coop.GunnerOn(60)')
    here = game.pos()
    _, portals = level.path(list(here), list(goal))
    stats = {'scans': 0, 'walks': 0, 'destroyed': [], 'rounds': 0, 'skipped': 0}
    if level.m.get('level') in (2, 5, 8, 11):
        max_arm(game, log)
    if fly:
        prepare_flight(game, level, scan(game, out, fly=True)[0], log)
    index = 0
    while index < len(portals):
        target = portals[index]['to']
        stalls = 0
        best = math.dist(game.pos()[:2], target[:2])
        while math.dist(game.pos()[:2], target[:2]) > 4:
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
                gspot = rim_toward(nav, game.pos(), gen['pos'])
                if math.dist(gspot, gen['pos']) > 25:
                    continue
                log(f'    generator {gen["name"]} first')
                walk(game, nav, gspot, log, timeout=60)
                if aimed_kill(game, gen['name'], log):
                    stats['destroyed'].append(gen['name'])
            if collect(game, level, nav, log) and level.m.get('level') in (2, 5, 8, 11):
                max_arm(game, log)
            before = game.pos()
            spot = reachable_toward(nav, before, target)
            pos, state = walk(game, nav, spot, log)
            stats['walks'] += 1
            # progress is a new best distance to the target: walking back what a push lost is not
            gained = best - math.dist(pos[:2], target[:2])
            best = min(best, math.dist(pos[:2], target[:2]))
            log(f'  toward {[round(v) for v in target]}: {state}, {gained:+.1f} past the best, now {[round(v, 1) for v in pos]}')
            if gained > 2:
                stalls = 0
                continue
            stalls += 1
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
                if obj['name'] in stats['destroyed'] or obj['name'] in stats.setdefault('gave_up', []):
                    continue
                if not alive(game, obj['name']):
                    continue
                if kill_objective(game, nav, obj, log):
                    stats['destroyed'].append(obj['name'])
                    done = True
                    break
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


def squad(games, level, goal, folder, log, fly=False, chute=False, route=None, record=None):
    """Every game is a bot: player 1 leads to the goal; each other player follows the leader with its
    own pilot and gunner. Worlds are compared at the end."""
    import threading
    leader, followers = games[0], games[1:]
    result = {}
    stop = threading.Event()

    def lead():
        if record:
            leader.lua(f'coop_route("record", "{record}")')
        if route:
            result['leader'] = follow(leader, route, lambda m: log('[1] ' + m))
        else:
            result['leader'] = advance(leader, level, goal, str(Path(folder) / 'nav_1.json'),
                                       lambda m: log('[1] ' + m), fly=fly, chute=chute)
        if record:
            leader.lua(f'coop_route("stop", "{record}")')
        stop.set()

    def follow(g):
        g.lua('coop.GunnerOn(60)')
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

    threads = [threading.Thread(target=lead)] + [threading.Thread(target=follow, args=(g,)) for g in followers]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    ok_world, _ = compare_worlds(games, folder, 'end', log)
    gaps = {g.slot: round(math.dist(g.pos(), leader.pos()), 1) for g in followers}
    log(f'  followers\' distance from the leader at the end: {gaps}')
    return result['leader'][0] and ok_world, {'leader': result['leader'][1], 'world_ok': ok_world, 'gaps': gaps}


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


def route_waypoints(frames, step_ms=400, step_units=3.0):
    """Points every step_ms or step_units along the recording, each with the action keys (not movement)
    held on the way to it, and the checkpoint the recording was at."""
    out, held, last = [], set(), None
    for ms, keys, level, cp, pos in frames:
        held |= keys - MOVE_KEYS
        # a point per step_units moved (or a little movement over a while); standing still adds none,
        # its held keys (firing at a wall) go with the next point
        moved = math.dist(pos, last[1]) if last else step_units
        if last is None or moved >= step_units or (moved >= 0.5 and ms - last[0] >= 5 * step_ms):
            out.append({'ms': ms, 'pos': pos, 'keys': sorted(held), 'level': level, 'cp': cp})
            held, last = set(), (ms, pos)
    if frames and (not out or out[-1]['pos'] != frames[-1][4]):
        out.append({'ms': frames[-1][0], 'pos': frames[-1][4], 'keys': [], 'level': frames[-1][2], 'cp': frames[-1][3]})
    return out


def follow(game, path, log, radius=2.5, lost_after=120):
    """Play a recording closed-loop: walk its positions with the pilot (skipping on when nearer a later
    point), pressing the action keys the recording held on each stretch. Robust to timing and drift,
    unlike replaying the inputs by the clock."""
    pts = route_waypoints(load_route(path))
    log(f'  following {len(pts)} points of {path}')
    game.lua('coop.GunnerOn(60)')
    i, t0, last_progress, best = 0, time.monotonic(), time.monotonic(), None
    while i < len(pts):
        here = game.pos()
        # skip on: the nearest of the next few points
        ahead = range(i, min(len(pts), i + 80))
        j = min(ahead, key=lambda k: math.dist(here, pts[k]['pos']))
        if j > i:
            i = j
        p = pts[i]
        for code in p['keys']:
            game.lua(f'coop_press({code if code < 256 else code}, 450)')
        rise = p['pos'][2] - here[2]
        r = game.goto(*p['pos'], radius=radius, timeout=6, explore=0, jump=int(0.7 < rise <= 3.5),
                      fly=int(57 in p['keys'] and rise > 3.5))
        if r.get('state') == 'arrived':
            i += 1
            last_progress, best = time.monotonic(), None
            continue
        # nearer the point than ever before counts as progress (a fight or a wall can take a while)
        d = math.dist(game.pos(), p['pos'])
        if best is None or d < best - 1:
            best, last_progress = d, time.monotonic()
        # held up where the recording was shooting (a scripted wall in the way): fire along the way
        if any(256 in q['keys'] for q in pts[max(0, i - 3):i + 3]):
            for _ in range(6):
                game.lua(f'local p=coop.real_player() mdkGobPointToXYZ(p, {p["pos"][0]}-p.position.x, '
                         f'{p["pos"][1]}-p.position.y, 0) coop_press(256, 600)')
                time.sleep(.5)
        if time.monotonic() - last_progress > lost_after:
            log(f'  lost at point {i}/{len(pts)} {p["pos"]}, hero at {here}')
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
    fo = sub.add_parser('follow', help='play a recorded route closed-loop')
    fo.add_argument('--dirs', nargs='+', required=True)
    fo.add_argument('--player', type=int, default=1)
    fo.add_argument('--route', required=True)
    a = ap.parse_args(argv)
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
        ok, stats = follow(games[a.player - 1], a.route, lambda m: print(m, flush=True))
        stats['telemetry'] = games[a.player - 1].telemetry()
        print(f'{"ARRIVED" if ok else "FAILED"}: {stats}')
        return 0 if ok else 1
    if a.cmd == 'squad':
        t0 = time.monotonic()
        folder = Path(a.evidence) / time.strftime('%Y%m%d-%H%M%S')
        ok, stats = squad(games, Level(a.level), a.to, folder, lambda m: print(m, flush=True), fly=a.fly,
                          chute=a.chute, route=a.route, record=a.record)
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
