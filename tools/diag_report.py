#!/usr/bin/env python3
"""Read diagnostics bundles (the zips the launcher's "Send diagnostics" makes, or a coop folder with a logs
folder in it) and say what happened: sessions, crashes, desyncs, marks, network quality, and one timeline.

    tools/diag_report.py host.zip client.zip              summary of each, then what the pair shows
    tools/diag_report.py host.zip client.zip --timeline merged.txt [--align]
    tools/diag_report.py some/coop --raw                  a live coop folder: the hash chains are checked here
    tools/diag_report.py bundle.zip --json                the same summary as JSON

Session log lines are "<n> <utc ms> <chain> <text>", chain = FNV-1a/64 of "<n> <utc ms> <text>" continued from the
previous line's chain (bridge/diag.c). A bundle was redacted after the chain was checked on the sender's machine, so
there the chain result comes from manifest.json; a raw folder is checked here. This shows lost, cut or edited lines,
not forgery.
"""
import argparse
import hashlib
import json
import os
import re
import statistics
import sys
import zipfile

FNV_BASIS = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3
MASK = (1 << 64) - 1
CLEAN_END = b"session end: clean exit"


def fnv(h, data):
    for b in data:
        h = ((h ^ b) * FNV_PRIME) & MASK
    return h


def chain_line(prev, n, utc, text):
    """The chain value for one line, given the previous one."""
    return fnv(prev, b"%d %s %s" % (n, utc, text))


def format_line(prev, n, utc_ms, text):
    """One session-log line as bridge/diag.c writes it; returns (line bytes, chain). For tests."""
    utc = b"%d" % utc_ms
    text = text.replace(b"\r", b" ").replace(b"\n", b" ")
    c = chain_line(prev, n, utc, text)
    return b"%d %s %016x %s\n" % (n, utc, c, text), c


def parse_log(data):
    """Lines of a session log -> (lines, problem). A line is (n, utc ms, text bytes); the chain is checked."""
    lines, prev, problem = [], FNV_BASIS, None
    pos = 0
    while pos < len(data):
        end = data.find(b"\n", pos)
        if end < 0:
            break                   # being written when copied, or cut off by a crash: not a broken chain
        raw = data[pos:end]
        pos = end + 1
        parts = raw.split(b" ", 3)
        if len(parts) < 4:
            problem = problem or "malformed line %d" % len(lines)
            break
        n, utc, chain, text = parts
        if n != b"%d" % len(lines):
            problem = problem or "line %s where %d was expected (lines missing)" % (n.decode("ascii", "replace"), len(lines))
            break
        prev = chain_line(prev, len(lines), utc, text)
        if chain != b"%016x" % prev:
            problem = problem or "chain mismatch at line %d (edited or damaged)" % len(lines)
        try:
            lines.append((len(lines), int(utc), text))
        except ValueError:
            problem = problem or "bad time at line %d" % len(lines)
            break
    return lines, problem


class Source:
    """One bundle or coop folder: its files by relative path (forward slashes) and what is known about them."""

    def __init__(self, path, raw):
        self.path = path
        self.name = os.path.basename(path.rstrip("/\\")).replace(".zip", "")
        self.files = {}
        self.manifest = None
        self.problems = []
        if os.path.isdir(path):
            raw = True
            for root, _dirs, names in os.walk(path):
                for f in names:
                    full = os.path.join(root, f)
                    self.files[os.path.relpath(full, path).replace(os.sep, "/")] = open(full, "rb").read()
        else:
            with zipfile.ZipFile(path) as z:
                for info in z.infolist():
                    if not info.is_dir():
                        self.files[info.filename.replace("\\", "/")] = z.read(info)
            if "manifest.json" in self.files:
                self.manifest = json.loads(self.files["manifest.json"].decode("utf-8-sig"))
            else:
                self.problems.append("no manifest.json: not a bundle from the launcher")
        self.raw = raw
        self.check_manifest()

    def check_manifest(self):
        if not self.manifest:
            return
        listed = {}
        for e in self.manifest.get("files", []):
            listed[e["file"]] = e
            data = self.files.get(e["file"])
            if data is None:
                self.problems.append("%s is listed in the manifest but missing from the zip" % e["file"])
            elif hashlib.sha256(data).hexdigest() != e["sha256"]:
                self.problems.append("%s does not match its SHA-256 in the manifest (changed after the bundle was made)" % e["file"])
        for f in self.files:
            if f != "manifest.json" and f != "README.txt" and f not in listed:
                self.problems.append("%s is in the zip but not in the manifest" % f)
        for p in self.manifest.get("problems", []):
            self.problems.append("collection: %s" % p)

    def chain_info(self, rel):
        """(status, problem, clean exit) for a session log: from the manifest for a bundle, else checked here."""
        if self.manifest and not self.raw:
            for e in self.manifest.get("files", []):
                if e["file"] == rel and "chain" in e:
                    c = e["chain"]
                    return c["status"], c.get("problem") or None, c.get("clean_exit")
        lines, problem = parse_log(self.files[rel])
        clean = bool(lines) and lines[-1][2] == CLEAN_END
        return ("broken" if problem else "ok"), problem, clean


def lines_of(src, rel):
    """Lines of a session log. In a redacted bundle the chain no longer matches the text, so it is not re-checked."""
    out = []
    for raw in src.files[rel].split(b"\n"):
        parts = raw.split(b" ", 3)
        if len(parts) == 4 and parts[0].isdigit() and parts[1].isdigit():
            out.append((int(parts[0]), int(parts[1]), parts[3]))
    return out


def fmt_time(ms):
    t = ms / 1000.0
    return "%s.%03d" % (__import__("time").strftime("%Y-%m-%d %H:%M:%S", __import__("time").gmtime(t)), ms % 1000)


def spark(values, width=60):
    bars = " .:-=+*#%@"
    if not values:
        return ""
    step = max(1, len(values) // width)
    cols = [max(values[i:i + step]) for i in range(0, len(values), step)]
    hi = max(cols) or 1
    return "".join(bars[min(len(bars) - 1, int(v * (len(bars) - 1) / hi))] for v in cols)


def stats(values):
    if not values:
        return None
    v = sorted(values)
    return {"min": v[0], "avg": statistics.mean(v), "p95": v[min(len(v) - 1, int(len(v) * 0.95))], "max": v[-1], "n": len(v)}


# F2 marks: "lua: MARK n at (..)" where it was pressed, "lua: MARK n (from player k)" where it arrived
MARK = re.compile(r"(?:lua: )?MARK (\d+)(?: \(from player (\d+)\))?")
NETQ = re.compile(rb"netq: player (\d+) rtt (\d+) ms, jitter ([\d.]+) ms, loss ([\d.]+)%")


def summarise(src):
    """Everything worth saying about one source, as a dict."""
    out = {"source": src.name, "problems": list(src.problems), "sessions": [], "flight": [], "system": {}}
    for rel in sorted(src.files):
        if re.fullmatch(r"logs/[^/]+\.log", rel):
            key = rel.split("/")[-1][:-4]
            status, problem, clean = src.chain_info(rel)
            lines = lines_of(src, rel)
            s = {"session": key, "lines": len(lines), "chain": status, "chain_problem": problem, "clean_exit": bool(clean),
                 "crashed": ("logs/%s.crash" % key) in src.files, "version": "", "build": "", "role": "",
                 "start": lines[0][1] if lines else 0, "end": lines[-1][1] if lines else 0,
                 "crash_lines": [], "desync": [], "marks": [], "rtt": {}, "jitter": {}, "loss": {}}
            netq = {}
            for _n, utc, text in lines:
                t = text.decode("latin-1")
                m = re.match(r"session \S+: mod (\S+)", t)
                if m:
                    s["version"] = m.group(1)
                m = re.match(r"game build: (.*)", t)
                if m:
                    s["build"] = m.group(1)
                m = re.search(r"\brole (host|client)\b", t) if not s["role"] else None
                if m:
                    s["role"] = m.group(1)
                if t.startswith("crash:"):
                    s["crash_lines"].append((utc, t))
                if t.startswith("DESYNC"):
                    s["desync"].append((utc, t))
                if MARK.match(t):
                    s["marks"].append((utc, t))
                m = NETQ.match(text)
                if m:
                    p = int(m.group(1))
                    d = netq.setdefault(p, {"rtt": [], "jitter": [], "loss": []})
                    d["rtt"].append(int(m.group(2)))
                    d["jitter"].append(float(m.group(3)))
                    d["loss"].append(float(m.group(4)))
            for p, d in netq.items():
                s["rtt"][p], s["jitter"][p], s["loss"][p] = stats(d["rtt"]), stats(d["jitter"]), stats(d["loss"])
                s.setdefault("rtt_series", {})[p] = d["rtt"]
            out["sessions"].append(s)
        m = re.fullmatch(r"logs/([^/]+)\.flight-(\d+)-(\w+)\.txt", rel)
        if m:
            head, samples = src.files[rel].decode("latin-1").split("\n", 1)[0], 0
            body = src.files[rel].decode("latin-1").split("\n")
            rtts = {}
            reason = ""
            for line in body:
                if line.startswith("# reason:"):
                    reason = line[9:].strip()
                if line.startswith("S "):
                    samples += 1
                    for rec in line.split("|", 1)[1].split(";"):
                        f = rec.split()
                        if len(f) >= 4:
                            rtts.setdefault(int(f[0]), []).append(float(f[1]))
            out["flight"].append({"file": rel, "session": m.group(1), "marker": int(m.group(2)), "kind": m.group(3),
                                  "reason": reason, "samples": samples, "rtt": rtts})
    for rel in ("system.txt", "network.txt"):
        if rel in src.files:
            out["system"][rel] = src.files[rel].decode("utf-8", "replace")
    return out


def print_summary(r):
    p = print
    p("=" * 78)
    p("%s" % r["source"])
    for pr in r["problems"]:
        p("  !! %s" % pr)
    sysx = r["system"].get("system.txt", "")
    for key in ("windows:", "cpu:", "memory:", "graphics:", "game exe:", "dinput8.dll:"):
        for line in sysx.splitlines():
            if line.startswith(key):
                p("  " + line)
    crashes = sysx.split("== game crashes in the Windows event log (14 days)")[-1].split("==")[0].strip() if sysx else ""
    if crashes and crashes != "none":
        p("  windows event log, game crashes:")
        for line in crashes.splitlines():
            p("    " + line.strip())
    for s in r["sessions"]:
        dur = (s["end"] - s["start"]) / 1000.0
        end = "crashed" if s["crashed"] else "ended normally" if s["clean_exit"] else "no clean exit (killed, or still running)"
        p("")
        p("  session %s  mod %s  %s  %.0f s  %d lines  %s" % (s["session"], s["version"] or "?", s["role"] or "?", dur,
                                                         s["lines"], end))
        if s["build"]:
            p("    game build: %s" % s["build"])
        if s["chain"] != "ok":
            p("    !! log chain %s: %s" % (s["chain"], s["chain_problem"]))
        for utc, t in s["crash_lines"]:
            p("    %s  %s" % (fmt_time(utc), t))
        for utc, t in s["desync"]:
            p("    %s  %s" % (fmt_time(utc), t))
        for utc, t in s["marks"]:
            p("    %s  %s" % (fmt_time(utc), t))
        for pl in sorted(s["rtt"]):
            rt, ji, lo = s["rtt"][pl], s["jitter"][pl], s["loss"][pl]
            p("    link to player %d: rtt %d / %.0f / %d / %d ms (min / avg / p95 / max), jitter avg %.1f ms, loss avg %.1f%% max %.1f%%"
              % (pl, rt["min"], rt["avg"], rt["p95"], rt["max"], ji["avg"], lo["avg"], lo["max"]))
            p("      rtt over time  [%s]" % spark(s["rtt_series"][pl]))
    for f in r["flight"]:
        p("")
        p("  flight recorder %s marker %d (%s): %s" % (f["session"], f["marker"], f["kind"], f["reason"]))
        for pl in sorted(f["rtt"]):
            vals = f["rtt"][pl]
            if any(vals):
                p("    player %d rtt over the last %d samples  [%s]  max %d ms" % (pl, len(vals), spark(vals), max(vals)))


def mark_pairs(reports):
    """(mark number, sender source, sender utc, receiver source, receiver utc) for every F2 mark pressed in one
    bundle and received in another. Mark numbers count per player, so a number pressed in two bundles is ambiguous
    and left out."""
    local, recv = {}, {}
    for r in reports:
        for s in r["sessions"]:
            for utc, t in s["marks"]:
                m = MARK.match(t)
                if m:
                    (recv if m.group(2) else local).setdefault(int(m.group(1)), []).append((r["source"], utc))
    pairs = []
    for n, sent in sorted(local.items()):
        if len({src for src, _ in sent}) != 1:
            continue
        for src, utc in recv.get(n, []):
            if src != sent[0][0]:
                pairs.append((n, sent[0][0], sent[0][1], src, utc))
    return pairs


def pair_findings(reports):
    """What several sources show together: F2 marks seen on both machines and the clock offset between them."""
    return ["mark %d pressed on %s, arrived on %s: its clock reads %+d ms against %s's (includes the one-way delay, "
            "about half the round trip)" % (n, a, b, ub - ua, a) for n, a, ua, b, ub in mark_pairs(reports)]


def timeline(sources, reports, path, align):
    offsets = {r["source"]: 0 for r in reports}
    if align:
        for _n, a, ua, b, ub in mark_pairs(reports):
            if not offsets[b] and offsets[a] == 0:
                offsets[b] = ua - ub        # shift b so the mark it received lines up with the press
    rows = []
    for src in sources:
        for rel in src.files:
            if re.fullmatch(r"logs/[^/]+\.log", rel):
                for _n, utc, text in lines_of(src, rel):
                    rows.append((utc + offsets[src.name], src.name, rel.split("/")[-1][:19], text.decode("latin-1")))
    rows.sort(key=lambda x: (x[0], x[1]))
    with open(path, "w") as f:
        for utc, name, sess, text in rows:
            f.write("%s  %-12s %s\n" % (fmt_time(utc), name[:12], text))
    return len(rows)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("inputs", nargs="+", help="bundle zips, or coop folders")
    ap.add_argument("--raw", action="store_true", help="the inputs are live (unredacted): check the hash chains here")
    ap.add_argument("--timeline", metavar="FILE", help="write every log line of every input, in time order")
    ap.add_argument("--align", action="store_true", help="in the timeline, shift each source so a shared mark lines up")
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args()
    sources = [Source(p, a.raw) for p in a.inputs]
    reports = [summarise(s) for s in sources]
    if a.json:
        json.dump(reports, sys.stdout, indent=1, default=lambda o: None)
        print()
    else:
        for r in reports:
            print_summary(r)
        pf = pair_findings(reports)
        if pf:
            print("")
            print("=" * 78)
            for line in pf:
                print(line)
    if a.timeline:
        n = timeline(sources, reports, a.timeline, a.align)
        print("\ntimeline: %d lines in %s" % (n, a.timeline))
    bad = any(r["problems"] or any(s["chain"] != "ok" for s in r["sessions"]) for r in reports)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
