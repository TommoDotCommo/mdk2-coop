#!/usr/bin/env python3
"""statecheck.py <host state> <client state> [<client state> ...] [--tol units]

Compares coop_state() dumps (see bridge/state.c), each client's world against the host's and
every player against its puppet in every other game, and prints one line per problem:
  FAIL  the worlds disagree (level, enemy missing / extra / hp, item, player)
  warn  likely timing (positions a little apart, a door mid-swing, an enemy not bound yet)
Last line: "state: <n> fail, <n> warn, <n> enemies compared". Exit status 1 on any FAIL.
"""
import math
import sys


def load(path):
    s = {"level": None, "me": None, "player": None, "puppet": {}, "ent": {}, "gone": set(), "actor": [],
         "item": {}, "unplaced": 0, "door": {}}
    for line in open(path, encoding="latin-1"):
        f = line.split()
        if not f:
            continue
        k = f[0]
        if k == "level":
            s["level"] = int(f[1])
        elif k == "player":
            s["player"] = (int(f[1]), int(f[2]), tuple(map(float, f[3:6])))
        elif k == "me":
            s["me"] = int(f[1])
        elif k == "puppet":
            s["puppet"][int(f[1])] = tuple(map(float, f[2:5]))
        elif k == "ent" and f[2] == "gone":
            s["gone"].add(f[1])
        elif k == "ent":
            s["ent"][f[1]] = (int(f[2]), int(f[3]), tuple(map(float, f[4:7])), " ".join(f[7:]))
        elif k == "actor":
            s["actor"].append((int(f[1]), int(f[2]), tuple(map(float, f[3:6])), " ".join(f[6:])))
        elif k == "item":
            # an item at exactly (0, 0, 0) is not placed yet: a gob caught while being made,
            # still carrying its slot's old name ("chute" with item types 314, 323, 331 in
            # three sweeps, on either side); counted apart and only warned about
            if all(float(v) == 0.0 for v in f[2:5]):
                s["unplaced"] += 1
                continue
            name = " ".join(f[5:])
            s["item"][name] = s["item"].get(name, 0) + 1
        elif k == "door":
            s["door"][" ".join(f[2:])] = int(f[1])
    return s


def main():
    args = sys.argv[1:]
    tol = 4.0
    if "--tol" in args:
        i = args.index("--tol")
        tol = float(args[i + 1])
        del args[i:i + 2]
    dumps = [load(a) for a in args]
    h, clients = dumps[0], dumps[1:]
    out = []
    fail = lambda m: out.append("FAIL  " + m)
    warn = lambda m: out.append("warn  " + m)
    who = lambda d: "host" if d is h else ("client" if len(clients) == 1 else f"client {d['me']}")

    bad_level = [c for c in clients if c["level"] != h["level"]]
    for c in bad_level:
        fail(f"level: host {h['level']}, {who(c)} {c['level']}")

    # players: each one's character against its puppet in every other game
    for me in dumps:
        if not me["player"]:
            warn(f"{who(me)} has no player")
            continue
        for other in dumps:
            if other is me or other in bad_level or (me in bad_level and other is h):
                continue
            pos = other["puppet"].get(me["me"])
            if pos is None:
                fail(f"{who(me)}'s player has no puppet in the {who(other)}'s game")
                continue
            d = math.dist(me["player"][2], pos)
            if d > tol:
                (fail if d > 4 * tol else warn)(f"{who(me)}'s player is {d:.1f} from its puppet in the {who(other)}'s game")

    compared = 0
    for c in clients:
        if c not in bad_level:
            compared += compare_world(h, c, who(c), tol, fail, warn)

    nfail = sum(1 for l in out if l.startswith("FAIL"))
    for line in out:
        print(line)
    print(f"state: {nfail} fail, {len(out) - nfail} warn, {compared} enemies compared")
    return 1 if nfail else 0


def compare_world(h, c, cn, tol, fail, warn):
    """enemies, items and doors of one client against the host's; returns enemies compared"""
    # enemies: the host's are the truth
    compared = 0
    for nid, (t, hp, pos, name) in sorted(h["ent"].items()):
        if nid in c["gone"]:
            if hp > 0:
                fail(f"enemy {nid} {name} (type {t}, hp {hp}) deleted on the {cn}")
            continue
        if nid not in c["ent"]:
            warn(f"enemy {nid} {name} (type {t}) not bound on the {cn} yet")
            continue
        compared += 1
        ct, chp, cpos, cname = c["ent"][nid]
        if ct != t:
            fail(f"enemy {nid} {name}: type {t} on host, {ct} on {cn}")
        if chp != hp:
            fail(f"enemy {nid} {name}: hp {hp} on host, {chp} on {cn}")
        d = math.dist(pos, cpos)
        if d > tol:
            (fail if d > 4 * tol else warn)(f"enemy {nid} {name} is {d:.1f} apart on the {cn}")
    for nid, (t, hp, pos, name) in sorted(c["ent"].items()):
        if nid not in h["ent"]:
            fail(f"enemy {nid} {name} (type {t}) on the {cn}, gone on the host")
    for t, hp, pos, name in c["actor"]:
        warn(f"{cn} actor {name} (type {t}) not announced by the host (orphan?)")
    if cn in ("client", "client 2"):
        for t, hp, pos, name in h["actor"]:
            warn(f"host actor {name} (type {t}) not announced yet")

    if h["unplaced"] or c["unplaced"]:
        warn(f"items not placed yet (at 0,0,0): {h['unplaced']} on host, {c['unplaced']} on {cn}")
    # items: by name, counts
    for name in sorted(set(h["item"]) | set(c["item"])):
        if h["item"].get(name, 0) != c["item"].get(name, 0):
            fail(f"item {name or '(unnamed)'}: {h['item'].get(name, 0)} on host, {c['item'].get(name, 0)} on {cn}")

    # doors swing over a moment, so only a warning
    for name in sorted(set(h["door"]) | set(c["door"])):
        if h["door"].get(name) != c["door"].get(name):
            warn(f"door {name}: {h['door'].get(name)} on host, {c['door'].get(name)} on {cn}")
    return compared


if __name__ == "__main__":
    sys.exit(main())
