#!/usr/bin/env python3
"""hpsync_check.py pick HOST_DUMP                      -> prints "<netid> <hp>" of an enemy to change (hp >= 20)
hpsync_check.py check NETID HP HOST_DUMP CLIENT_DUMP  -> PASS/FAIL: both games hold that enemy at HP

An enemy's hp changes without a hit (the game tops a turret up, a grunt at 0 stands up at 60) and the client must
follow (bridge/entities.c, ent_hp_told). tools/qa_hpsync.sh changes one enemy's hp on the host only, with
coop_test_sethp, and reads each game's own state dump (bridge/state.c, "ent <netid> <type> <hp> ...").
"""
import sys

FLYER_TYPE = 230


def ents(path):
    out = {}
    with open(path, encoding="latin-1") as f:
        for line in f:
            p = line.split()
            if len(p) >= 8 and p[0] == "ent" and p[2] != "gone":
                out[p[1]] = (int(p[2]), int(p[3]))
    return out


def main(argv):
    if len(argv) == 2 and argv[0] == "pick":
        # a walking or standing enemy (not a flyer, whose type is shared with fixed turrets, nor a generator)
        for nid, (typ, hp) in sorted(ents(argv[1]).items()):
            if 200 <= typ <= 259 and typ != FLYER_TYPE and hp >= 20:
                print(nid, hp)
                return 0
        print("FAIL  no ground enemy with hp >= 20 in the host's dump")
        return 1
    if len(argv) == 5 and argv[0] == "check":
        nid, want = argv[1], int(argv[2])
        host, client = ents(argv[3]), ents(argv[4])
        hp = lambda d: d[nid][1] if nid in d else None
        ok = hp(host) == want and hp(client) == want
        print(f"{'PASS' if ok else 'FAIL'}  enemy {nid} wanted hp {want}: host {hp(host)}, client {hp(client)}")
        return 0 if ok else 1
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
