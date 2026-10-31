#!/usr/bin/env python3
"""Co-op play for the campaign bot (tools/campaign_bot.py squad): the parts that are pure logic, so
tools/test_campaign_bot.py checks them without a game.

- World comparison per step: statecheck.py's lines sorted by a tolerance table into known
  differences (listed with their reason), findings (everything else that fails) and timing warnings.
- Lives: downs, revives, deaths and returns counted from each game's own coop_life() readings.
- Revives: who goes to a downed player (the nearest standing teammate).
- Roles: when a step has several objectives, each player takes a different one.
"""
import math
import re

# statecheck.py FAIL lines that are known and not counted as findings, with the reason. Everything
# else that fails is a co-op finding. Keep each entry narrow: a pattern and the evidence for it.
KNOWN = [
    (re.compile(r'^FAIL  enemy \w+ \S*(manta|flyer|shiptur)\S* is [0-9.]+ apart on the client'),
     'flying enemies drift on clients (docs/CAMPAIGN-BOT.md, whole-campaign survey: levels 1, 2, 4, 8)'),
]


def classify(text, known=KNOWN):
    """statecheck.py output -> {'findings': [...], 'known': [[line, reason]...], 'warn': [...], 'summary': str}."""
    out = {'findings': [], 'known': [], 'warn': [], 'summary': ''}
    for line in text.splitlines():
        line = line.rstrip()
        if line.startswith('state:'):
            out['summary'] = line
        elif line.startswith('FAIL'):
            reason = next((why for pat, why in known if pat.search(line)), None)
            if reason:
                out['known'].append([line, reason])
            else:
                out['findings'].append(line)
        elif line.startswith('warn'):
            out['warn'].append(line)
    return out


# coop_life() values (bridge/revive.c)
ALIVE, DOWNED, DEAD, SPECTATING = 0, 1, 2, 3


class Lives:
    """Counts what happens to each player from successive coop_life() readings of its own game."""

    def __init__(self, slots):
        self.last = {s: ALIVE for s in slots}
        self.downs = {s: 0 for s in slots}
        self.revives = {s: 0 for s in slots}     # downed -> alive (a teammate held R, or a checkpoint)
        self.deaths = {s: 0 for s in slots}      # downed or alive -> dead (bled out, lava)
        self.returns = {s: 0 for s in slots}     # dead -> alive (a checkpoint, the zone cleared)
        self.events = []

    def update(self, slot, life, t=0.0):
        was = self.last.get(slot, ALIVE)
        if life == was or life is None or life < 0:
            return None
        if life == DOWNED:
            self.downs[slot] += 1
            ev = 'down'
        elif life == ALIVE and was == DOWNED:
            self.revives[slot] += 1
            ev = 'revived'
        elif life == ALIVE:
            self.returns[slot] += 1
            ev = 'back'
        elif was in (ALIVE, DOWNED):
            self.deaths[slot] += 1
            ev = 'died'
        else:
            ev = 'spectating'
        self.last[slot] = life
        self.events.append([round(t, 1), slot, ev])
        return ev

    def totals(self):
        return {'downs': sum(self.downs.values()), 'revives': sum(self.revives.values()),
                'deaths': sum(self.deaths.values()), 'returns': sum(self.returns.values()),
                'per_player': {s: {'downs': self.downs[s], 'revives': self.revives[s], 'deaths': self.deaths[s],
                                   'returns': self.returns[s]} for s in self.last}}


def bridge_lives(logs):
    """What the mod itself logged ('life: ...' lines of each game's bridge.log, a list of texts) as counts:
    downs, revives by a teammate, and new levels (a wipe: the squad reloaded at its checkpoint, which
    Lives cannot tell from a revive)."""
    out = {'downs': 0, 'revived_by_teammate': 0, 'new_level': 0, 'bled_out': 0}
    for text in logs:
        for line in text.splitlines():
            if 'life: alive -> downed' in line:
                out['downs'] += 1
            elif 'life: downed -> alive (revived by' in line:
                out['revived_by_teammate'] += 1
            elif 'life: alive (new level)' in line:
                out['new_level'] += 1
            elif 'bled out' in line and 'life:' in line:
                out['bled_out'] += 1
    return out


def reviver(positions, lives, downed):
    """The standing teammate nearest the downed player, or None. positions/lives: {slot: ...}."""
    here = positions.get(downed)
    if here is None:
        return None
    able = [s for s, l in lives.items() if s != downed and l == ALIVE and positions.get(s) is not None]
    return min(able, key=lambda s: math.dist(positions[s], here), default=None)


LOW_HP = 35         # below this the hero looks after itself (of 100)
HEALTH = ('OBJ_DOUBLEA', 'OBJ_CARBATTERY')


def health_target(pickups, here, taken, radius=60.0):
    """The nearest health pickup not yet tried within radius of here, or None."""
    near = [o for o in pickups if o['type'] in HEALTH and o['name'] not in taken
            and math.dist(o['pos'], here) <= radius]
    return min(near, key=lambda o: math.dist(o['pos'], here), default=None)


def split(objectives, players):
    """Roles for a step with several objectives: {player slot: objective} for every player after the
    leader (slot 1 keeps its route, which deals with the first). Each takes a different one, the
    one after the leader's first, in order; players beyond the objectives get none."""
    roles = {}
    for slot, obj in zip(range(2, players + 1), objectives[1:]):
        roles[slot] = obj
    return roles


def regrouped(positions, goal, radius=15.0):
    """True when every player stands within radius of the goal (the portal they meet at)."""
    return all(p is not None and math.dist(p, goal) <= radius for p in positions.values())


def down_check(totals, slot):
    """The revive mutant's verdict from Lives.totals(): the player that was hit must have gone down, and
    been revived without dying. {'down_seen', 'revived', 'died', 'ok'}."""
    me = totals.get('per_player', {}).get(slot, {})
    seen, rev, died = me.get('downs', 0) > 0, me.get('revives', 0) > 0, me.get('deaths', 0) > 0
    return {'down_seen': seen, 'revived': rev, 'died': died, 'ok': seen and rev and not died}


# ---- reading threats from the host's state dump (no-God survival) ----
COVER_HP = 60       # at or below this a hero that is being shot at takes cover
MISSILE = 201       # homing missiles of a SAM generator (hp 30), shot down first when near
GENERATOR = 511     # spawners do not shoot; they are objectives, not threats


def host_threats(rows):
    """The host's state dump (Game.state() rows) -> {'players': {slot: (x, y, z)}, 'threats': {slot: [enemy]}}.
    An enemy is a threat to player N when the engine's own AI target of it (`ai_target <id> <type> @pN`)
    is that player. Each enemy: {id, type, hp, pos, name, target}."""
    ents, targets, players = {}, {}, {}
    for r in rows:
        try:
            if r[0] == 'ent' and len(r) >= 8:
                ents[r[1]] = {'id': r[1], 'type': int(r[2]), 'hp': int(r[3]),
                              'pos': (float(r[4]), float(r[5]), float(r[6])), 'name': r[7]}
            elif r[0] == 'ai_target' and len(r) >= 4 and r[3].startswith('@p'):
                targets[r[1]] = int(r[3][2:])
            elif r[0] == 'player' and len(r) >= 6:
                players['me'] = (float(r[3]), float(r[4]), float(r[5]))
            elif r[0] == 'puppet' and len(r) >= 5:
                players[int(r[1])] = (float(r[2]), float(r[3]), float(r[4]))
        except (ValueError, IndexError):
            continue
    out = {}
    for eid, slot in targets.items():
        e = ents.get(eid)
        if e and e['hp'] > 0 and e['type'] != GENERATOR:
            out.setdefault(slot, []).append(dict(e, target=slot))
    return {'players': players, 'threats': out}


def pick_focus(threats, here, reach=55.0):
    """What a hero shoots first among the enemies targeting it: a missile within 25 units (it is about to
    hit), otherwise the one that dies soonest (lowest health), the nearer on a tie. Only what is within the
    weapon's reach counts (the AI targets a hero from across the room), and a missile further off is not
    worth a shot. None if nothing is in reach."""
    near = [(math.dist(t['pos'], here), t) for t in threats]
    near = [(d, t) for d, t in near if d <= (25 if t['type'] == MISSILE else reach)]
    if not near:
        return None
    missiles = [(d, t) for d, t in near if t['type'] == MISSILE]
    pool = missiles or near
    return min(pool, key=lambda x: (x[1]['hp'], x[0]))[1]


def cover_candidates(here, trail, radii=(5.0, 9.0, 14.0, 20.0), spokes=8):
    """Places to try for cover, nearest first: where the hero came from, then rings around it."""
    pts = [tuple(p) for p in trail if math.dist(p, here) > 3]
    for r in radii:
        for k in range(spokes):
            a = 2 * math.pi * k / spokes
            pts.append((here[0] + r * math.cos(a), here[1] + r * math.sin(a), here[2]))
    return sorted(pts, key=lambda p: math.dist(p, here))


# Differences that are real in a single comparison and gone at the next one. They are tolerated only
# once the next comparison of the same run shows the host's world and the client's agreeing about that
# enemy (settle()); one that is still there stays a finding. Evidence: docs/CAMPAIGN-BOT.md, findings.
TRANSIENT = [
    (re.compile(r'^FAIL  enemy (\w+) \S+: hp \d+ on host, \d+ on client'),
     'enemy health on a client lags the host while players fire; equal at the next comparison'),
    (re.compile(r'^FAIL  enemy (\w+) \S+ \(type \d+\) on the client(?: \d+)?, gone on the host'),
     'an enemy the client still showed after the host removed it; gone at the next comparison'),
]


def settle(steps):
    """Move the transient findings of every step that the next step no longer has into its `known` list
    (with the reason) and count them in `settled`. The last step cannot be settled. steps: the list of
    per-step dicts of campaign_bot.squad (changed in place and returned)."""
    for i, s in enumerate(steps[:-1]):
        later = ' '.join(steps[i + 1]['findings'])
        keep, moved = [], 0
        for line in s['findings']:
            hit = next(((m.group(1), why) for pat, why in TRANSIENT for m in [pat.search(line)] if m), None)
            if hit and f' {hit[0]} ' not in f' {later} ':
                s['known'].append([line, 'transient: ' + hit[1]])
                moved += 1
            else:
                keep.append(line)
        s['findings'] = keep
        if moved:
            s['settled'] = s.get('settled', 0) + moved
    return steps
