#!/usr/bin/env python3
"""statecheck.py <host state> <client state> [--tol units]

Compares two coop_state() dumps (see bridge/state.c) and prints one line per problem:
  FAIL  the worlds disagree (level, enemy missing / extra / hp, item, player)
  warn  likely timing (positions a little apart, a door mid-swing, an enemy not bound yet)
Last line: "state: <n> fail, <n> warn, <n> enemies compared". Exit status 1 on any FAIL.
"""
import math
import sys


def load(path):
    s = {"level": None, "player": None, "puppet": None, "ent": {}, "gone": set(), "actor": [],
         "item": {}, "door": {}}
    for line in open(path, encoding="latin-1"):
        f = line.split()
        if not f:
            continue
        k = f[0]
        if k == "level":
            s["level"] = int(f[1])
        elif k == "player":
            s["player"] = (int(f[1]), int(f[2]), tuple(map(float, f[3:6])))
        elif k == "puppet":
            s["puppet"] = tuple(map(float, f[1:4]))
        elif k == "ent" and f[2] == "gone":
            s["gone"].add(f[1])
        elif k == "ent":
            s["ent"][f[1]] = (int(f[2]), int(f[3]), tuple(map(float, f[4:7])), " ".join(f[7:]))
        elif k == "actor":
            s["actor"].append((int(f[1]), int(f[2]), tuple(map(float, f[3:6])), " ".join(f[6:])))
        elif k == "item":
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
    h, c = load(args[0]), load(args[1])
    out = []
    fail = lambda m: out.append("FAIL  " + m)
    warn = lambda m: out.append("warn  " + m)

    if h["level"] != c["level"]:
        fail(f"level: host {h['level']}, client {c['level']}")
        for line in out:
            print(line)
        print(f"state: 1 fail, 0 warn, 0 enemies compared")
        return 1

    # players: each one's Kurt against the puppet the other game draws
    for who, me, other in (("host", h, c), ("client", c, h)):
        if not me["player"]:
            warn(f"{who} has no player")
        elif not other["puppet"]:
            fail(f"{who}'s player has no puppet in the other game")
        else:
            d = math.dist(me["player"][2], other["puppet"])
            if d > tol:
                (fail if d > 4 * tol else warn)(f"{who}'s player is {d:.1f} from its puppet")

    # enemies: the host's are the truth
    compared = 0
    for nid, (t, hp, pos, name) in sorted(h["ent"].items()):
        if nid in c["gone"]:
            if hp > 0:
                fail(f"enemy {nid} {name} (type {t}, hp {hp}) deleted on the client")
            continue
        if nid not in c["ent"]:
            warn(f"enemy {nid} {name} (type {t}) not bound on the client yet")
            continue
        compared += 1
        ct, chp, cpos, cname = c["ent"][nid]
        if ct != t:
            fail(f"enemy {nid} {name}: type {t} on host, {ct} on client")
        if chp != hp:
            fail(f"enemy {nid} {name}: hp {hp} on host, {chp} on client")
        d = math.dist(pos, cpos)
        if d > tol:
            (fail if d > 4 * tol else warn)(f"enemy {nid} {name} is {d:.1f} apart")
    for nid, (t, hp, pos, name) in sorted(c["ent"].items()):
        if nid not in h["ent"]:
            fail(f"enemy {nid} {name} (type {t}) on the client, gone on the host")
    for t, hp, pos, name in c["actor"]:
        warn(f"client actor {name} (type {t}) not announced by the host (orphan?)")
    for t, hp, pos, name in h["actor"]:
        warn(f"host actor {name} (type {t}) not announced yet")

    # items: by name, counts
    for name in sorted(set(h["item"]) | set(c["item"])):
        if h["item"].get(name, 0) != c["item"].get(name, 0):
            fail(f"item {name or '(unnamed)'}: {h['item'].get(name, 0)} on host, {c['item'].get(name, 0)} on client")

    # doors swing over a moment, so only a warning
    for name in sorted(set(h["door"]) | set(c["door"])):
        if h["door"].get(name) != c["door"].get(name):
            warn(f"door {name}: {h['door'].get(name)} on host, {c['door'].get(name)} on client")

    nfail = sum(1 for l in out if l.startswith("FAIL"))
    for line in out:
        print(line)
    print(f"state: {nfail} fail, {len(out) - nfail} warn, {compared} enemies compared")
    return 1 if nfail else 0


if __name__ == "__main__":
    sys.exit(main())
