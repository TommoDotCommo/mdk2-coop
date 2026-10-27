#!/usr/bin/env bash
# No game files, desktop input or game copies. The diagnostics chain end to end on made-up coop folders:
# tools/diag_report.py's log reader (whole, edited, missing lines, cut tail), dist/diag.ps1 run by Windows
# PowerShell on a folder whose newest log is held open for writing the way the game holds it (names, addresses,
# Steam ids and e-mail addresses planted in the logs must not reach the zip; entry names use '/'; the manifest
# hashes and hash chains check out), and the report on the result. Broken variants must fail: a changed file in
# the zip, a log with a line removed, a log that was edited.
#   tools/test_diag.sh
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
command -v powershell.exe >/dev/null || { echo "needs Windows PowerShell (WSL interop) to run dist/diag.ps1" >&2; exit 1; }
out="$here/local/tests/diag-test"; rm -rf "$out"; mkdir -p "$out"
winuser=$(powershell.exe -NoProfile -Command '$env:USERNAME' | tr -d '\r')
winhost=$(powershell.exe -NoProfile -Command '$env:COMPUTERNAME' | tr -d '\r')
python3 -B - "$here" "$out" "$winuser" "$winhost" <<'PY'
import json, shutil, subprocess, sys, threading, time, zipfile
from pathlib import Path

here, out, user, host = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3], sys.argv[4]
sys.path.insert(0, str(here / "tools"))
import diag_report as d

failures = []


def check(name, ok, detail=""):
    print(("PASS  " if ok else "FAIL  ") + name + ("" if ok else "  " + str(detail)))
    if not ok:
        failures.append(name)


def session(lines, base=1761570000000, clean=True):
    prev, data = d.FNV_BASIS, b""
    texts = list(lines) + ([b"session end: clean exit"] if clean else [])
    for n, t in enumerate(texts):
        line, prev = d.format_line(prev, n, base + n * 100, t)
        data += line
    return data


good = session([b"session s1: mod v-test", b"game build: Steam build 3902",
                b"udp listen 27900, role host:0", b"netq: player 2 rtt 30 ms, jitter 2.0 ms, loss 0.0% (290 states, 0 missing in 5 s)",
                b"MARK 1761570 (mark by player 1)"])
# --- the reader
lines, problem = d.parse_log(good)
check("a whole log reads without problem", problem is None and len(lines) == 6, problem)
rows = good.split(b"\n")
check("a missing line is found", d.parse_log(b"\n".join(rows[:2] + rows[3:]))[1] is not None)
edited = good.replace(b"Steam build 3902", b"Steam build 3903")
check("an edited line is found", d.parse_log(edited)[1] is not None)
check("a half-written last line is not a broken chain", d.parse_log(good + b"7 17615 abc half")[1] is None)
crlf = d.format_line(d.FNV_BASIS, 0, 1, b"two\nlines")[0]
check("a line break inside a message is flattened", crlf.count(b"\n") == 1)

# --- a coop folder: an old crashed run, and a live one the 'game' still holds open
coop = out / "game" / "coop"
(coop / "logs").mkdir(parents=True)
planted = (b"saved to C:\\Users\\" + user.encode() + b"\\Documents on " + host.encode() + b", peer 192.168.1.20:27900, "
           b"relay 100.101.102.103, remote 8.8.4.4, steam 76561198012345678, mail someone@example.com")
(coop / "logs" / "2026-10-26T10-00-00Z_p1.log").write_bytes(session([b"session a: mod v-test", planted, b"crash: exception 0xc0000005 at x.dll+0x1"], clean=False))
(coop / "logs" / "2026-10-26T10-00-00Z_p1.crash").write_bytes(b"")
(coop / "logs" / "2026-10-26T10-00-00Z_p1.flight-0-crash.txt").write_text("# flight recorder: kind=crash\nS 1 1 1 60.0 3 0 0 0 |\nL 1 " + planted.decode() + "\n")
live = coop / "logs" / "2026-10-27T10-00-00Z_p2.log"
live.write_bytes(session([b"session b: mod v-test", planted, b"netq: player 2 rtt 80 ms, jitter 9.0 ms, loss 3.5% (290 states, 11 missing in 5 s)"], clean=False))
broken = coop / "logs" / "2026-10-25T10-00-00Z_p0.log"
b_rows = session([b"session c: mod v-test", b"x", b"y", b"z"]).split(b"\n")
broken.write_bytes(b"\n".join(b_rows[:2] + b_rows[3:]))
(coop / "coop.ini").write_text("[net]\nrole=join\npeer_host=100.101.102.103\n")
(coop / "player.ini").write_text("[player]\nname=Kurt\nsteam=76561198012345678\n")

# the game holds its live log open for appending and lets others read it
holder = subprocess.Popen(["powershell.exe", "-NoProfile", "-Command",
                           "$f = [IO.File]::Open('%s', 'Append', 'Write', 'Read'); Start-Sleep 90" % subprocess.run(
                               ["wslpath", "-w", str(live)], capture_output=True, text=True).stdout.strip()])
time.sleep(3)
win = lambda p: subprocess.run(["wslpath", "-w", str(p)], capture_output=True, text=True).stdout.strip()
r = subprocess.run(["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", win(here / "dist/diag.ps1"),
                    "-CoopDir", win(coop), "-OutDir", win(out), "-Version", "v-test", "-NoOpen"], capture_output=True, text=True)
holder.kill()
result = (coop / "diag-result.txt").read_text().strip()
check("the script reports the zip", result.startswith("OK "), result + r.stderr[:300])
zips = sorted(out.glob("mdk2-coop-diag_*.zip"))
check("one zip on the output folder", len(zips) == 1)
z = zipfile.ZipFile(zips[0])
names = z.namelist()
check("entry names use '/'", all("\\" not in n for n in names), names)
check("the logs, the flight dump and the crash marker are in it",
      all(any(n.endswith(s) for n in names) for s in ("p1.log", "p1.crash", "p1.flight-0-crash.txt", "p2.log", "p0.log")), names)
check("system and network details are in it", "system.txt" in names and "network.txt" in names, names)
everything = b"\n".join(z.read(n) for n in names)
for what, needle in (("the Windows user name", user), ("the computer name", host), ("a LAN address", "192.168.1.20"),
                     ("a Tailscale address", "100.101.102.103"), ("a public address", "8.8.4.4"),
                     ("a Steam id", "76561198012345678"), ("an e-mail address", "someone@example.com")):
    check(what + " is not in the zip", needle.lower().encode() not in everything.lower(), needle)
check("addresses became numbered labels", b"ip-1(lan)" in everything and b"(tailscale-or-cgnat)" in everything and b"(public)" in everything)
check("the same address has the same label", everything.count(b"ip-1(") >= 3)
m = json.loads(z.read("manifest.json").decode("utf-8-sig"))
chains = {f["file"].split("/")[-1]: f["chain"] for f in m["files"] if "chain" in f}
check("the live log was readable while the game held it", chains.get("2026-10-27T10-00-00Z_p2.log", {}).get("status") == "ok", chains)
check("the crashed run's chain is intact and it has no clean exit",
      chains["2026-10-26T10-00-00Z_p1.log"]["status"] == "ok" and not chains["2026-10-26T10-00-00Z_p1.log"]["clean_exit"])
check("a log with a line missing is marked broken", chains["2026-10-25T10-00-00Z_p0.log"]["status"] == "broken", chains)

# --- the report
rep = subprocess.run([sys.executable, str(here / "tools/diag_report.py"), str(zips[0]), "--json"], capture_output=True, text=True)
check("the report flags the broken log (exit 1)", rep.returncode == 1, rep.stdout[-300:])
data = json.loads(rep.stdout)[0]
sessions = {s["session"]: s for s in data["sessions"]}
s2 = sessions["2026-10-27T10-00-00Z_p2"]
check("the report reads link quality", s2["rtt"]["2"]["avg"] == 80 and s2["loss"]["2"]["max"] == 3.5, s2["rtt"])
check("the report sees the crash", sessions["2026-10-26T10-00-00Z_p1"]["crashed"] and sessions["2026-10-26T10-00-00Z_p1"]["crash_lines"])
check("the report sees the flight dump", data["flight"] and data["flight"][0]["kind"] == "crash")
check("system.txt reached the report", "windows:" in data["system"].get("system.txt", ""))

# --- broken variants must fail
bad = out / "tampered.zip"
with zipfile.ZipFile(zips[0]) as src, zipfile.ZipFile(bad, "w") as dst:
    for n in src.namelist():
        body = src.read(n)
        if n.endswith("p2.log"):
            body = body.replace(b"rtt 80", b"rtt 10")
        dst.writestr(n, body)
rep = subprocess.run([sys.executable, str(here / "tools/diag_report.py"), str(bad)], capture_output=True, text=True)
check("a log changed after bundling is caught", "does not match its SHA-256" in rep.stdout and rep.returncode == 1)
raw = out / "rawcheck"
(raw / "logs").mkdir(parents=True)
(raw / "logs" / "a.log").write_bytes(edited)
rep = subprocess.run([sys.executable, str(here / "tools/diag_report.py"), "--raw", str(raw)], capture_output=True, text=True)
check("a live folder with an edited log is caught", "chain mismatch" in rep.stdout and rep.returncode == 1, rep.stdout[-300:])
check("the script is plain ASCII (Windows PowerShell 5.1 reads it as ANSI)", all(b < 128 for b in (here / "dist/diag.ps1").read_bytes()))

print()
print("%d failed" % len(failures) if failures else "all passed")
sys.exit(1 if failures else 0)
PY
