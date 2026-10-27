#!/usr/bin/env python3
"""Compare which script handlers ran on the host and on a client (bridge/scripttrace.c writes
`script event L<level>: <event> <gob>` the first time a gob's handler runs in a level).

A handler that ran on one machine and never on the other is how the door after the first enemy stayed shut
for clients: an enemy's OnDie ran on the host only. Events whose status in tools/script_events.txt is `synced`
or `partial` are promises, so a difference there fails; the rest is listed for a look.

    tools/script_diff.py host/bridge.log client/bridge.log [client2/bridge.log ...]
    tools/script_diff.py host.log client.log --all         fail on every difference, not only promised events
    tools/script_diff.py host.log client.log --client-only also fail on any handler that ran on a client only
    tools/script_diff.py host.log client.log --levels 1    only that level

tools/script_known.txt records differences someone has looked at (`level | event | gob (wildcards) | which side |
status | note`): `reviewed` is expected and harmless, `bug` is a real one that is still open, `unreviewed` was seen
and nobody has looked yet. All are listed apart and do not fail the run; a difference that matches neither is new and fails if its event is promised (or with --all).

Logs are bridge.log or a session log (lines prefixed `<n> <utc> <chain>`). The host's own puppets and test spawns
(coop_*), unnamed gobs and the network's n<id> names are left out: they are different on every machine by design.
"""
import argparse
import fnmatch
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import script_census

LINE = re.compile(r"script event L(-?\d+): (On\w+) (\S+)")
KNOWN = Path(__file__).with_name("script_known.txt")
SKIP_NAME = re.compile(r"^(\(unnamed\)|coop_\w*|n[0-9a-f]{3,})$")


def parse(text):
    """{level: {(event, gob)}} from a log."""
    seen = {}
    for line in text.splitlines():
        m = LINE.search(line)
        if m and not SKIP_NAME.match(m.group(3)):
            seen.setdefault(int(m.group(1)), set()).add((m.group(2), m.group(3)))
    return seen


def diff(host, client, levels=None):
    """[(level, event, gob, 'host only' | 'client only')] for the levels both logs have events for."""
    out = []
    for level in sorted(set(host) & set(client)):
        if levels and level not in levels:
            continue
        for ev, gob in sorted(host[level] - client[level]):
            out.append((level, ev, gob, "host only"))
        for ev, gob in sorted(client[level] - host[level]):
            out.append((level, ev, gob, "client only"))
    return out


def load_known(path=KNOWN):
    """[(level or '*', event, gob glob, side or '*', status, note)]; a bad line is an error."""
    rows = []
    for n, line in enumerate(Path(path).read_text().splitlines(), 1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        f = [x.strip() for x in line.split("|")]
        if len(f) != 6 or f[4] not in ("reviewed", "bug", "unreviewed") or f[3] not in ("host only", "client only", "*"):
            raise ValueError("%s:%d: expected 'level | event | gob | host only/client only/* | reviewed/bug/unreviewed | note'" % (path, n))
        rows.append(tuple(f))
    return rows


def known_status(row, known):
    """(status, note) of the first known entry matching a diff row, or None."""
    level, ev, gob, side = row
    for k_level, k_ev, k_gob, k_side, status, note in known:
        if (k_level in ("*", str(level)) and k_ev == ev and fnmatch.fnmatch(gob, k_gob) and k_side in ("*", side)):
            return status, note
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("host")
    ap.add_argument("clients", nargs="+")
    ap.add_argument("--all", action="store_true", help="fail on every difference")
    ap.add_argument("--levels", type=int, nargs="*", help="only these levels")
    ap.add_argument("--table", default=str(script_census.TABLE))
    ap.add_argument("--known", default=str(KNOWN))
    ap.add_argument("--client-only", action="store_true",
                    help="a handler that ran on a client and never on the host fails too: it changed state the host does not know")
    a = ap.parse_args()
    table = script_census.load_table(a.table)
    known = load_known(a.known)
    host = parse(Path(a.host).read_text(errors="replace"))
    failing = 0
    print("host: %d levels, %d (event, gob) pairs" % (len(host), sum(len(v) for v in host.values())))
    for path in a.clients:
        client = parse(Path(path).read_text(errors="replace"))
        rows = diff(host, client, a.levels)
        print("\n%s: %d levels, %d pairs; levels in both: %s" % (path, len(client), sum(len(v) for v in client.values()),
                                                              sorted(set(host) & set(client))))
        by_event = {}
        recorded = []
        for level, ev, gob, where in rows:
            k = known_status((level, ev, gob, where), known)
            if k:
                recorded.append((level, ev, gob, where, k))
            else:
                by_event.setdefault(ev, []).append((level, gob, where))
        groups = {}
        for level, ev, gob, where, (status, note) in recorded:
            groups.setdefault((status, level, ev, where, note), []).append(gob)
        for (status, level, ev, where, note), gobs in groups.items():
            print("  known %-8s L%d %s %s (%s)%s: %s" % (status, level, ev, gobs[0], where,
                                                      " and %d more" % (len(gobs) - 1) if len(gobs) > 1 else "", note))
        for ev in sorted(by_event, key=lambda e: -len(by_event[e])):
            status = table.get(ev, (None, "unlisted"))[1]
            promised = a.all or status in ("synced", "partial")
            n_fail = sum(1 for _l, _g, where in by_event[ev] if promised or (a.client_only and where == "client only"))
            failing += n_fail
            print("  %-15s %-9s %3d differ%s" % (ev, status, len(by_event[ev]), "   <- %d fail" % n_fail if n_fail else ""))
            for level, gob, where in by_event[ev][:6]:
                print("      L%d %s  (%s)" % (level, gob, where))
            if len(by_event[ev]) > 6:
                print("      ... %d more" % (len(by_event[ev]) - 6))
    return 1 if failing else 0


if __name__ == "__main__":
    sys.exit(main())
