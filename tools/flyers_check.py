#!/usr/bin/env python3
"""flyers_check.py [--limit units] [--min-flyers n] [--min-travel units] HOST CLIENT [HOST CLIENT ...]

Flying enemies (OBJ_TURRET, type 230: the mantas and level 1's flyers) on a client must stay where the
host has them. Each HOST CLIENT pair is one coop_state request (bridge/state.c; the host asks the client
at the same moment). For every type-230 enemy in both dumps (fixed turrets share the type) the distance
between the host's position and the client's is measured; the check fails when any is more than --limit
apart in any pair, or when fewer than --min-flyers of them travelled --min-travel on the host over the
run (a test of enemies that never moved proves nothing). Prints the moving and the failing ones, and
PASS/FAIL.
"""
import argparse
import math
import sys

FLYER_TYPE = 230


def flyers(path):
    """{netid: (name, (x, y, z))} for the synced type-230 enemies in a dump."""
    out = {}
    with open(path, encoding="latin-1") as f:
        for line in f:
            p = line.split()
            if len(p) >= 8 and p[0] == "ent" and p[2] == str(FLYER_TYPE):
                out[p[1]] = (p[7], tuple(float(v) for v in p[4:7]))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--limit", type=float, default=30.0)
    ap.add_argument("--min-flyers", type=int, default=1)
    ap.add_argument("--min-travel", type=float, default=50.0)
    ap.add_argument("dumps", nargs="+")
    a = ap.parse_args()
    if len(a.dumps) % 2:
        ap.error("dumps come in HOST CLIENT pairs")
    worst, names, first, travel = {}, {}, {}, {}
    for i in range(0, len(a.dumps), 2):
        host, client = flyers(a.dumps[i]), flyers(a.dumps[i + 1])
        for nid, (name, hp) in host.items():
            names[nid] = name
            first.setdefault(nid, hp)
            travel[nid] = max(travel.get(nid, 0.0), math.dist(first[nid], hp))
            if nid in client:
                worst[nid] = max(worst.get(nid, 0.0), math.dist(hp, client[nid][1]))
    ok = True
    moving = [nid for nid in worst if travel[nid] >= a.min_travel]
    for nid in sorted(worst):
        bad = worst[nid] > a.limit
        ok &= not bad
        if bad or nid in moving:
            print(f"{'FAIL' if bad else 'ok  '}  {names[nid]} ({nid}): at most {worst[nid]:.1f} apart, "
                  f"travelled {travel[nid]:.1f} on the host")
    if len(moving) < a.min_flyers:
        print(f"FAIL  {len(moving)} flyers travelled {a.min_travel} units on the host, want {a.min_flyers}")
        ok = False
    print(f"max drift {max(worst.values(), default=0.0):.1f} over {len(a.dumps) // 2} samples: "
          f"{'PASS' if ok else 'FAIL'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
