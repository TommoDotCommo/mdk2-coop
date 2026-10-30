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
