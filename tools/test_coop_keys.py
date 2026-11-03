#!/usr/bin/env python3
"""T0: the shared co-op keys, their migration and the quick guide's key lines, with the real Lua 3.1.

The key table block of lua/coop_dev.lua (between BEGIN KEYTABLE and END KEYTABLE) is run with the game's
functions stubbed (tools/fixtures/coop_keys_test.lua). Then every mutant -- a deliberately broken copy of the block --
must fail the same checks. Also static checks on the C and Lua sources: no hand-written co-op key text in the guide,
the menu page names and order, the launcher's matching section names."""
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LUA = ROOT / "local/tools/lua31/lua/bin/lua"
SRC = (ROOT / "lua/coop_dev.lua").read_text()
FIXTURE = (ROOT / "tools/fixtures/coop_keys_test.lua").read_text()

begin = SRC.index("-- BEGIN KEYTABLE")
end = SRC.index("-- END KEYTABLE")
BLOCK = SRC[begin:end]


def ensure_lua():
    if not LUA.exists():
        subprocess.run(["bash", str(ROOT / "tools/test_lua31.sh")], check=True, stdout=subprocess.DEVNULL)


def pad_layer_lua():
    """bridge/pad.c's pad_layer_lines as a Lua table: the one table the guide and the Controls page read"""
    src = (ROOT / "bridge/pad.c").read_text()
    body = src[src.index("pad_layer_lines[] = {"):]
    body = body[:body.index("};")]
    rows = re.findall(r'\{\s*"([^"]*)",\s*"([^"]*)",\s*"([^"]*)"\s*\}', body)
    return "padlayer = {\n" + "".join('  { "%s", "%s", "%s" },\n' % r for r in rows) + "}\n"


def run(block, tmp):
    prog = tmp / "keys.lua"
    # the test file defines the stubs and "coop = {}" first; the block then fills coop
    head, tail = FIXTURE.split("nfail = 0", 1)
    prog.write_text(head + "\n" + pad_layer_lua() + block + "\nnfail = 0" + tail)
    return subprocess.run([str(LUA), str(prog)], capture_output=True, text=True, timeout=30)


MUTANTS = {
    "custom-overwritten-always": ('if (v ~= format("%d,%d", a[1], a[2])) then same = nil end', 'if (nil) then same = nil end'),
    "custom-overwritten-second": ("elseif (same and k[2] == 15) then k[2] = -1 end", "elseif (k[2] == 15) then k[2] = -1 end"),
    "migration-never-runs": ('if (coop_get("controls_custom", "v", "") == "2") then return end', "do return end"),
    "migration-reruns": ('coop_set("controls_custom", "v", "2")\n        return', "return"),
    "modern-keeps-tab": ("kuse = { 33, -1 }, kclose = { 34, -1 },\n    sniper = { 1001, -1 }, zoomin = { 1008, 17 }, zoomout = { 1009, 31 },\n    minvl = { 16, 1008 }",
                         "kuse = { 33, 15 }, kclose = { 34, -1 },\n    sniper = { 1001, -1 }, zoomin = { 1008, 17 }, zoomout = { 1009, 31 },\n    minvl = { 16, 1008 }"),
    "no-side-button": ("voice = { 48, 1003 }", "voice = { 48, -1 }"),
    "tab-not-reserved": ("[15] = \"list\", ", ""),
    "reserved-ignored": ("if (o and o ~= id) then return o end", "if (nil) then return o end"),
    "f1-not-fixed": ("[59] = \"f1\", ", ""),
    "guide-ignores-bindings": ("local k = coop.Keys(id)\n            local t = coop.KeyText(k)", "local k = coop.Keys(id)\n            local t = \"T\""),
    "guide-watch-handwritten": ('format("%s / %s", coop.ShortKeyText(p), coop.ShortKeyText(q))', '"[ / ]"'),
    "guide-list-missing": ('"emote", "list", "watch"', '"emote", "watch"'),
    "notice-first-look-announces": ("coop.known_seen = nil\ncoop.notice_n = 0", "coop.known_seen = 1\ncoop.notice_n = 0"),
    "notice-left-silent": ("coop_notice(format(coop.words.left, coop.known[s]))", "coop.known[s] = coop.known[s]"),
    "notice-repeats": ("coop.known[s] = name\n                if", "if"),
    "wording-differs": ('joined = "%s joined"', 'joined = "%s has joined"'),
    "guide-pad-missing": ("coop.GuideAdd(lines, t, a.guide, pt)", "coop.GuideAdd(lines, t, a.guide)"),
}


def static_checks(fails):
    ctrl = (ROOT / "bridge/ctrlview.c").read_text()
    guide = ctrl[ctrl.index("void guide_draw"):]
    for bad in ('"V / Middle mouse"', '"Hold J', '"Hold H', '"Voice chat (B)', '"Reviving (Hold R)', "gd.chat", "gd.ping",
                '"Left mouse", "While out of the fight', '"Backspace / Esc'):
        if bad in guide or bad in ctrl[ctrl.index("static struct {\n    char preset"):ctrl.index("static int guide_forced")]:
            fails.append("ctrlview.c still has hand-written co-op key text: " + bad)
    names = re.search(r"coop\.page_names = \{(.*?)\}", SRC, re.S).group(1)
    for want in ("Session", "Co-op rules", "Players and interface", "Voice and sound", "Controls"):
        if f'"{want}"' not in names:
            fails.append("menu page name missing: " + want)
    main = SRC[SRC.index("function coop.BuildMain()"):]
    main = main[:main.index("\nend")]
    order = [m for m in re.findall(r"coop\.page_names\.(\w+)", main)]
    if order != ["session", "rules", "ui", "voice", "controls"]:
        fails.append(f"menu page order is {order}")
    launcher = (ROOT / "bridge/launcher.c").read_text()
    # standard E: the launcher's sections in this order, named as the menu's pages where both have them
    sections = re.search(r"static const char \*sections\[\] = \{(.*?)\};", launcher, re.S)
    got = re.findall(r'"([^"]+)"', sections.group(1)) if sections else []
    if got != ["Co-op rules", "Players and interface", "Voice and sound", "Controls", "Display", "Network", "Launcher"]:
        fails.append(f"launcher Settings sections do not match the menu names and the standard order: {got}")
    if '"Voice volume"' not in launcher or '"Voice chat volume"' in launcher:
        fails.append("launcher voice volume row is not named as the menu's")
    if '"Voice volume"' not in SRC:
        fails.append("menu voice volume row is not named as the launcher's")


def main():
    ensure_lua()
    fails = []
    with tempfile.TemporaryDirectory(prefix="coop-keys-") as t:
        tmp = Path(t)
        base = run(BLOCK, tmp)
        print(base.stdout, end="")
        if base.returncode or "RESULT OK" not in base.stdout:
            fails.append("baseline: " + base.stderr.strip())
        for name, (old, new) in MUTANTS.items():
            if BLOCK.count(old) < 1:
                fails.append(f"mutant {name}: expected a match, found {BLOCK.count(old)}")
                continue
            r = run(BLOCK.replace(old, new, 1), tmp)
            if "RESULT OK" in r.stdout and not r.returncode:
                fails.append(f"mutant {name} was NOT caught")
            else:
                print(f"PASS mutant caught: {name}")
    static_checks(fails)
    if fails:
        for f in fails:
            print("FAIL " + f)
        return 1
    print(f"PASS: coop keys, migration, guide, names ({len(MUTANTS)} mutants caught)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
