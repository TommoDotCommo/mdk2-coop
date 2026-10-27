#!/usr/bin/env python3
"""Which script hooks does the game use, and what have we done about each?

The level scripts (the game's own Lua, in mdk2.tar) hang handlers on gobs: `gob.OnDie = fn`, or
`mdkSetLuaEvent(gob, "OnDie", fn)`. Whatever the engine runs on one machine runs on the others only if the mod
makes it (an enemy's OnDie opening a door was missed that way). This counts the handlers per event and lines
them up against tools/script_events.txt, the table of what is synced, partly synced, local on purpose or not yet
looked at. The scripts are the game's files and stay out of the repository: pass the archive.

    tools/script_census.py --archive <game dir>/mdk2.tar           the census and the table, side by side
    tools/script_census.py --archive ... --check                   exit 1 if a script uses an event the table lacks
    tools/script_census.py --archive ... --strict                  also exit 1 for every event still `unaudited`
    tools/script_census.py --dir <folder of .lua files>            the same from extracted scripts
    tools/script_census.py --archive ... --triggers 7              places to send a player on level 7, one per line
    tools/script_census.py --archive ... --proximity-lua           the nearest-player table for lua/coop_dev.lua
    tools/script_census.py --archive ... --inventory-pickups       pickup handlers that change the player's inventory

`--triggers N` lists the gobs and named points of level N's handlers that look at the local player (`mdkGetPlayerGob`
in the body of an inline `gob.OnUpdate = function ...`): a boss that starts when the player comes within 40 units of
r8_center. Such a handler runs for the player of the machine it runs on; tools/qa_triggers.sh sends each game's
player there in turn and tools/script_diff.py shows which handlers ran only on one machine.

The table (`id | event | status | handled by | note`, `-` where there is no id or file): status is `synced`
(the mod makes it run everywhere it should), `partial` (the note says what is left), `local` (each machine's own
game runs it, on purpose) or `unaudited`.
"""
import argparse
import os
import re
import sys
import tarfile
from pathlib import Path

TABLE = Path(__file__).with_name("script_events.txt")
STATUSES = ("synced", "partial", "local", "unaudited")
ASSIGN = re.compile(r"\.(On[A-Za-z0-9_]+)\s*=(?!=)")
SETEVENT = re.compile(r"SetLuaEvent\s*\(\s*[^,()]+,\s*\"(On[A-Za-z0-9_]+)\"")
OTHER = {"timers (omGobSetTimer, mdkSetTimer)": re.compile(r"SetTimer\s*\("),
         "global timers": re.compile(r"SetGlobalTimer\s*\("),
         "shatter triggers": re.compile(r"SetShatterTrigger\s*\(")}


def strip_comments(text):
    """Lua 3.1 line comments only (`--`); a block comment is rare in these scripts and counted as code."""
    return re.sub(r"--[^\n]*", "", text)


def census(scripts):
    """{event: {file: handlers}} and {mechanism: count} for {file name: source text}."""
    events, other = {}, {name: 0 for name in OTHER}
    for fname, text in scripts.items():
        code = strip_comments(text)
        for ev in ASSIGN.findall(code) + SETEVENT.findall(code):
            events.setdefault(ev, {}).setdefault(fname, 0)
            events[ev][fname] += 1
        for name, rx in OTHER.items():
            other[name] += len(rx.findall(code))
    return events, other


HEAD = re.compile(r"(?:([\w.]+)\.(On\w+)\s*=|SetLuaEvent\s*\(\s*([\w.]+)\s*,\s*\"(On\w+)\"\s*,)\s*function\s*\([^)]*\)")
BLOCK = re.compile(r"\b(function|if|while|for)\b|\b(end)\b")


def handler_bodies(code):
    """[(owner, event, body)] for inline handlers (`gob.OnX = function(...) ... end`). A handler assigned by name
    (`gob.OnX = Level.Fn`) is not followed: counts made from this are lower bounds. Block nesting is counted by
    keyword (function, if, while, for ... end), good enough for these scripts."""
    out = []
    for m in HEAD.finditer(code):
        depth, end = 1, None
        for tok in BLOCK.finditer(code, m.end()):
            depth += 1 if tok.group(1) else -1
            if depth == 0:
                end = tok.end()
                break
        if end:
            out.append((m.group(1) or m.group(3), m.group(2) or m.group(4), code[m.end():end]))
    return out


def triggers(code):
    """Names to send a player to: the owner and the quoted names of inline handlers that read the local player."""
    names = []
    for owner, event, body in handler_bodies(code):
        if "GetPlayerGob" not in body:
            continue
        for n in [owner] + re.findall(r'"([A-Za-z_][\w]*)"', body):
            if n not in names and not n.startswith("On"):
                names.append(n)
    return names


READ_ONLY = ("mdkGobDistance", "mdkGobDistancePoint", "mdkGobDistanceGob", "mdkGobDistancePointXY", "mdkGetGobType",
             "mdkGobGetHealth", "mdkGetGobName", "mdkGobGetPosition")
INVENTORY = re.compile(r"mdk\w*(?:Add|Remove|Give|Take)\w*Item\w*")
NAMED = re.compile(r"(?:^|\n)\s*(?:function\s+)?([\w.]+)\s*=?\s*function\s*\([^)]*\)|\nfunction\s+([\w.]+)\s*\(")
ASSIGN_NAMED = re.compile(r"([\w.]+)\.(On\w+)\s*=\s*([A-Za-z_][\w.]*)\s*(?:\n|$)")
SETEVENT_NAMED = re.compile(r"SetLuaEvent\s*\(\s*([\w.]+)\s*,\s*\"(On\w+)\"\s*,\s*([A-Za-z_][\w.]*)\s*\)")


def named_functions(code):
    out = {}
    for m in NAMED.finditer(code):
        end = None
        depth = 1
        for tok in BLOCK.finditer(code, m.end()):
            depth += 1 if tok.group(1) else -1
            if depth == 0:
                end = tok.end()
                break
        if end:
            out[m.group(1) or m.group(2)] = code[m.end():end]
    return out


def all_handlers(code):
    """[(owner, event, body)]: inline handlers and handlers assigned by the name of a function defined in this file."""
    named = named_functions(code)
    out = list(handler_bodies(code))
    for rx in (ASSIGN_NAMED, SETEVENT_NAMED):
        for m in rx.finditer(code):
            if m.group(3) in named:
                out.append((m.group(1), m.group(2), named[m.group(3)]))
    return out


def reads_player_only_to_look(body):
    """True if the body uses mdkGetPlayerGob() (or a local alias of it) only as an argument of a function that
    just looks (distances, type, health, position): a handler like that can be given the nearest player."""
    if "GetPlayerGob" not in body:
        return False
    tok = "__PLAYER__"
    body = re.sub(r"mdkGetPlayerGob\(\)", tok, body)
    for a in re.findall(r"local\s+(\w+)\s*=\s*%s" % tok, body):
        body = re.sub(r"local\s+%s\s*=\s*%s" % (a, tok), "", body)
        body = re.sub(r"\b%s\b" % a, tok, body)
    body = re.sub(r"%s\s*\.\s*position\s*\.\s*[xyz]\b" % tok, "0", body)
    for fn in READ_ONLY:
        body = re.sub(r"(\b%s\s*\()([^()]*)\)" % fn, lambda m: m.group(1) + m.group(2).replace(tok, "X") + ")", body)
    return tok not in body


def proximity(scripts):
    """{level number: [(owner, event)]}: OnUpdate and OnTimer handlers of level<N>.lua that only look at the player,
    on objects with a plain global name (a local variable called `door` is no use)."""
    out = {}
    for fname, text in scripts.items():
        m = re.fullmatch(r"level(\d+)\.lua", fname)
        if not m:
            continue
        for owner, event, body in all_handlers(strip_comments(text)):
            if (event in ("OnUpdate", "OnTimer") and re.fullmatch(r"[A-Za-z_]\w*", owner) and len(owner) >= 2 and
                    re.search(r"[0-9_]", owner) and reads_player_only_to_look(body)):
                if (owner, event) not in out.setdefault(int(m.group(1)), []):
                    out[int(m.group(1))].append((owner, event))
    return out


def proximity_lua(prox):
    """The table lua/coop_dev.lua carries: coop.nearest[level] = { "owner", "OnEvent", ... }"""
    lines = ["coop.nearest = {"]
    for level in sorted(prox):
        flat = ", ".join('"%s", "%s"' % pair for pair in prox[level])
        lines.append("    [%d] = { %s }," % (level, flat))
    lines.append("}")
    return "\n".join(lines)


def inventory_pickups(scripts):
    """[(file, owner, inventory calls)] for OnPickedUp handlers that change an inventory."""
    out = []
    for fname, text in scripts.items():
        for owner, event, body in all_handlers(strip_comments(text)):
            calls = sorted(set(INVENTORY.findall(body)) | set(re.findall(r"mdk(?:Kurt|Max|Doc)\w+Item\w*", body)))
            if event == "OnPickedUp" and calls:
                out.append((fname, owner, calls))
    return out


def load_table(path=TABLE):
    """{event: (id or None, status, handled by, note)}; a bad line is an error, not skipped."""
    rows = {}
    for n, line in enumerate(Path(path).read_text().splitlines(), 1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        f = [x.strip() for x in line.split("|")]
        if len(f) != 5 or f[2] not in STATUSES or not f[1].startswith("On"):
            raise ValueError("%s:%d: expected 'id | OnEvent | %s | handled by | note'" % (path, n, " or ".join(STATUSES)))
        if f[1] in rows:
            raise ValueError("%s:%d: %s listed twice" % (path, n, f[1]))
        rows[f[1]] = (None if f[0] == "-" else int(f[0]), f[2], f[3], f[4])
    return rows


def read_archive(path):
    out = {}
    with tarfile.open(path) as t:
        for m in t.getmembers():
            if m.isfile() and m.name.lower().endswith(".lua"):
                out[os.path.basename(m.name)] = t.extractfile(m).read().decode("latin-1")
    return out


def read_dir(path):
    return {p.name: p.read_text(encoding="latin-1") for p in Path(path).glob("*.lua")}


def report(events, other, table, out=sys.stdout):
    total = sum(sum(f.values()) for f in events.values())
    out.write("%d handlers in %d scripts, %d event names\n\n" % (
        total, len({n for f in events.values() for n in f}), len(events)))
    out.write("%-16s %8s %6s  %-10s %s\n" % ("event", "handlers", "files", "status", "handled by / note"))
    by_status = {s: 0 for s in STATUSES}
    unlisted = []
    for ev in sorted(events, key=lambda e: -sum(events[e].values())):
        n = sum(events[ev].values())
        row = table.get(ev)
        if row is None:
            unlisted.append(ev)
            out.write("%-16s %8d %6d  %-10s %s\n" % (ev, n, len(events[ev]), "NOT LISTED", "add it to tools/script_events.txt"))
            continue
        by_status[row[1]] += n
        out.write("%-16s %8d %6d  %-10s %s%s\n" % (ev, n, len(events[ev]), row[1], row[2] + ": " if row[2] != "-" else "", row[3]))
    out.write("\nhandlers by status: " + ", ".join("%s %d" % (s, by_status[s]) for s in STATUSES) + "\n")
    out.write("other mechanisms: " + ", ".join("%s %d" % kv for kv in other.items()) + "\n")
    return unlisted, by_status


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--archive", help="the game's mdk2.tar")
    ap.add_argument("--dir", help="a folder of extracted .lua files")
    ap.add_argument("--table", default=str(TABLE))
    ap.add_argument("--triggers", type=int, metavar="LEVEL", help="list places to send a player on that level")
    ap.add_argument("--proximity-lua", action="store_true", help="print the nearest-player table for lua/coop_dev.lua")
    ap.add_argument("--inventory-pickups", action="store_true", help="list pickup handlers that change an inventory")
    ap.add_argument("--check", action="store_true", help="exit 1 if a script uses an event the table does not list")
    ap.add_argument("--strict", action="store_true", help="--check, and exit 1 while any used event is still unaudited")
    a = ap.parse_args()
    if not a.archive and not a.dir:
        ap.error("give --archive or --dir")
    scripts = read_archive(a.archive) if a.archive else read_dir(a.dir)
    if a.triggers is not None:
        for name in triggers(strip_comments(scripts.get("level%d.lua" % a.triggers, ""))):
            print(name)
        return 0
    if a.proximity_lua:
        print(proximity_lua(proximity(scripts)))
        return 0
    if a.inventory_pickups:
        for fname, owner, calls in inventory_pickups(scripts):
            print("%s %s: %s" % (fname, owner, ", ".join(calls)))
        return 0
    events, other = census(scripts)
    table = load_table(a.table)
    unlisted, by_status = report(events, other, table)
    if (a.check or a.strict) and unlisted:
        print("\nNOT LISTED: " + ", ".join(unlisted))
        return 1
    if a.strict and by_status["unaudited"]:
        print("\n%d handlers are still unaudited" % by_status["unaudited"])
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
