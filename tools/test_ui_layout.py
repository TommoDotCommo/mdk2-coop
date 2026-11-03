#!/usr/bin/env python3
"""Game-free UI layout and DPI scaling verification for launcher and installer."""
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
SECTION_NAMES = ["Co-op rules", "Players and interface", "Voice and sound", "Controls", "Display", "Network", "Launcher"]


def define(src, name):
    return int(re.search(r"#define %s\s+(\d+)" % name, src).group(1))


def settings_rows(src):
    """The rows of each Settings section, read from page_settings(): [(kind, label, hint)] and the extra gaps."""
    body = src[src.index("static void page_settings(void)"):]
    body = body[:body.index("\n}\n")]
    sections = []
    for k in range(7):
        a = body.index("cfg.section == %d) {" % k) if k < 6 else body.index("} else {\n        int auto_en")
        b = body.index("cfg.section == %d) {" % (k + 1)) if k < 5 else (body.index("} else {\n        int auto_en") if k == 5 else len(body))
        part = body[a:b]
        rows = re.findall(r'\b(toggle|choice)\(ID, PX, y, PW, "([^"]+)", (NULL|"[^"]*"|[^,]*?),', part)
        sections.append((rows, part.count("y += ROW + 10")))
    return sections


def check_settings_fit(src, fails):
    """No Settings section runs into the status line above the primary button (rows are ROW + 2 apart)."""
    ROW, LH = define(src, "ROW"), define(src, "LH")
    limit = LH - 118
    for name, (rows, gaps) in zip(SECTION_NAMES, settings_rows(src)):
        if name == "Network":
            continue                    # its rows come from launcher_join.c (the port, two switches) plus two here
        bottom = 92 + ROW + 10 + len(rows) * (ROW + 2) + gaps * 8 - 2
        if bottom > limit:
            fails.append(f"Settings > {name}: {len(rows)} rows end at y={bottom}, past the status line at {limit}")


def host_decided_rows(src):
    """Launcher rows that say 'Host decides' in their hint, by section name."""
    out = {}
    for name, (rows, _) in zip(SECTION_NAMES, settings_rows(src)):
        out[name] = [(label, hint) for kind, label, hint in rows]
    return out


def check_menu_parity(src, lua, combat, fails):
    """E2, E4: the Co-op rules rows are in the launcher's order in game and the host's rows are tagged in both."""
    rows = host_decided_rows(src)
    rules = [(l, h) for l, h in rows["Co-op rules"]]
    launcher_order = [l for l, _ in rules if l != "Who can see my lobby"]
    page = lua[lua.index("function coop.BuildGame()"):]
    page = page[:page.index("\nend\n")]
    game = re.findall(r'mdkCreateMenuItem\(m, "([^"]+)"', page)
    game_names = [re.sub(r"( \(host decides\))?(: .*)?$", "", g) for g in game]
    if game_names[:len(launcher_order)] != launcher_order:
        fails.append(f"Co-op rules rows differ: launcher {launcher_order}, game {game_names}")
    for label, hint in rules:
        if label == "Who can see my lobby":
            continue
        if '"Host decides' not in hint:
            fails.append(f"launcher Co-op rules row {label!r} is not tagged 'Host decides' ({hint})")
    for g in game[:len(launcher_order)]:
        if "(host decides)" not in g:
            fails.append(f"in-game Co-op rules row {g!r} is not tagged (host decides)")
    voice = dict((l, h) for l, h in rows["Voice and sound"])
    if '"Host decides' not in voice.get("Hearing range", ""):
        fails.append("launcher Hearing range is not tagged 'Host decides'")
    if '"Hearing range (host decides)"' not in lua:
        fails.append("in-game Hearing range is not tagged (host decides)")
    if "The host's choice counts" in src or "(host)\"" in page:
        fails.append("old host wording left")
    # a host has no host address to type: its Session page says which port it listens on
    sess = lua[lua.index('mdkCreateMenuItem(m, "Host address: "'):] if 'mdkCreateMenuItem(m, "Host address: "' in lua else ""
    before = lua[:lua.index('mdkCreateMenuItem(m, "Host address: "')] if sess else ""
    if 'elseif (coop_role() == "host") then' not in before[-900:] or "Listening on UDP port" not in before[-600:]:
        fails.append("Session page shows a host the Host address row (a client's) instead of its port")
    # the DPS meter (b): an Off / Line / Full chooser in both places, off by default, in the same section; Ctrl+M cycles the
    # same three and writes the value back, so the chooser and the screen agree
    ui = dict((l, h) for l, h in rows["Players and interface"])
    if "DPS meter" not in ui or "Ctrl+M" not in ui["DPS meter"]:
        fails.append("launcher has no DPS meter row that names Ctrl+M")
    if 'choice(ID, PX, y, PW, "DPS meter"' not in src or '"Off", "Line", "Full"' not in src:
        fails.append("launcher DPS meter is not an Off / Line / Full chooser")
    if 'put_int("hud", "dps", cfg.dps)' not in src or 'GetPrivateProfileIntA("hud", "dps", 0, ini)' not in src:
        fails.append("launcher DPS meter does not read and write [hud] dps, off by default")
    iface = lua[lua.index("function coop.DpsItem(m)"):]
    iface = iface[:iface.index("\nend\n")]
    if not all(f'mdkMenuItemAddComboString(item, "{w}")' in iface for w in ("Off", "Line", "Full")) or \
            'coop_get("hud", "dps", "0")' not in iface or 'coop_set("hud", "dps", mdkMenuItemGetWidgitValue(self))' not in iface:
        fails.append("in-game DPS meter is not an Off / Line / Full chooser on [hud] dps, off by default")
    page = lua[lua.index("function coop.BuildInterface()"):]
    page = page[:page.index("\nend\n")]
    if "coop.DpsItem(m)" not in page:
        fails.append("in-game Players and interface has no DPS meter chooser")
    if 'GetPrivateProfileIntA("hud", "dps", 0, ini)' not in combat or "meter = (meter + 1) % 3" not in combat:
        fails.append("the bridge does not read [hud] dps live (default off) or Ctrl+M does not cycle off, line, full")
    cycle = combat[combat.index("meter = (meter + 1) % 3"):]
    cycle = cycle[:cycle.index("keys[i] = down")]
    if 'WritePrivateProfileStringA("hud", "dps"' not in cycle or "dps_setting = meter" not in cycle:
        fails.append("Ctrl+M does not write the new [hud] dps back, so the chooser and the screen can disagree")
    # developer keys (c): in game only when developer mode is on, last in Players and interface; never among the host's rows
    if "Developer keys" in lua[lua.index("function coop.BuildGame()"):lua.index("menu.coopgame.Cancel")]:
        fails.append("in-game Co-op rules still has the Developer keys row")
    dev = page.find('coop.CheckItem(m, "Developer keys"')
    gate = page.find('if (coop_get("controls", "devkeys", "0") == "1") then')
    items = [m.start() for m in re.finditer(r"coop\.(CheckItem|DpsItem)\(m", page)]
    if dev < 0 or gate < 0 or gate > dev or "end" not in page[dev:page.index("coop.BackItem", dev)]:
        fails.append("the in-game Developer keys row is not shown only in developer mode")
    elif dev != max(items):
        fails.append("the in-game Developer keys row is not the last of Players and interface")
    if '"Developer keys"' not in src:
        fails.append("the launcher lost its Developer keys row")
    # the Session page (a): Copy invite for the host (the launcher's invite, through the bridge) and Leave with the Esc confirmation
    sess = lua[lua.index("function coop.BuildSession()"):]
    sess = sess[:sess.index("\nend\n")]
    start = sess.find('    if (coop_role() == "host") then\n        item = mdkCreateMenuItem(m, "Start at level...")')
    hostrows = sess[start:sess.index("\n    end\n", start)] if start >= 0 else ""
    if 'coop.invite_note or "Copy invite"' not in hostrows or "coop_invite_copy()" not in hostrows:
        fails.append("the Session page has no Copy invite row among the host's rows")
    if '"Leave"' not in sess or 'menu.SwitchDlg("quitconfirm")' not in sess or "coop.level >= 1" not in sess[sess.index('"Leave"') - 200:sess.index('"Leave"')]:
        fails.append("the Session page has no Leave row that opens the Esc confirmation while in a game")
    if "leave_host" not in lua or "leave_client" not in lua:
        fails.append("the leave confirmation lost its words for the host or a guest")


def check_primary_button(src, fails):
    """A2: the primary button says what it does: Open lobby when hosting opens the lobby, Play solo, Join <name>."""
    fn = src[src.index("static const char *primary_label"):]
    fn = fn[:fn.index("\n}\n")]
    if '"Open lobby"' not in fn or 'lob_wants() ? "Open lobby"' not in fn or '"Play solo"' not in fn:
        fails.append("the primary button does not read Open lobby / Play solo")


def check_guide_text(ctrl, launcher, fails):
    """E4: the quick guides name the pages where they are now, and the lines that were cut off at 1280x720 stay short."""
    for stale in ("Launcher > Help", "Settings > Display toggles launcher", "Help tab"):
        if stale in ctrl or stale in launcher:
            fails.append(f"a quick guide still says {stale!r}")
    for cut in ("opens paged browser", "enemy HP & scaling, damage, down/revive time", "Toggle HUD, outlines, timer, ghosts, wheels",
                "proximity audio (16 kHz); Short / Med / Long / All"):
        if cut in ctrl:
            fails.append(f"the in-game guide line is cut off at 1280x720: {cut!r}")


def check_controls_lines(lua, fails):
    """The Controls page's line list is cut off at 1280x720 beyond about 70 characters of text: the push-to-talk line was."""
    for m in re.finditer(r'long="([^"]+)"', lua):
        if "Voice and sound" in m.group(1) and len(m.group(1)) > 62:
            fails.append(f"a Controls line is too long for 1280x720 and ends in 'sou.': {m.group(1)!r}")


def check_text_fit(src, fails):
    """Long values never run over the chooser's arrows; the About version never runs over its heading."""
    ch = src[src.index("static int choice(int id"):]
    ch = ch[:ch.index("\n}\n")]
    if not re.search(r"if \(gdi_width\(vf, names\[\*v\]\) > 140\)\s*vf = f_small", ch) or "vf = f_tiny" not in ch:
        fails.append("choice() does not shrink a value that would reach its arrows (Players and interface)")
    ab = src[src.index("static void page_about(void)"):]
    ab = ab[:ab.index("y += 36;")]
    if "title_width(\"MDK2 HD Co-op\"" not in ab or ab.count("gdi_width(f_small, vline) > room") < 2:
        fails.append("About does not keep the version clear of the heading")


def check_game_look(src, fails):
    """The game's look (launcher_look.h): every button label is fitted in the menu font or drawn in the GDI face, the
    short orange buttons fit theirs, and a lobby name never runs into the player's state on its right."""
    bl = src[src.index("static void button_label("):]
    bl = bl[:bl.index("\n}\n")]
    if "ll_label_height(glyph_adv, s, w, h)" not in bl or "ll_primary_height(glyph_adv, s, w, h)" not in bl or \
            "gdi_text(f, " not in bl:
        fails.append("button labels are not fitted to their button (ll_label_height / ll_primary_height, GDI fallback)")
    bt = src[src.index("static int button(int id"):]
    bt = bt[:bt.index("\n}\n")]
    if "button_label(x + 2, y, w - 2, h, s," not in bt or "if (gh >= 34)" not in bt:
        fails.append("button() draws a label without fitting it")
    lp = src[src.index("static void lobby_page(void)"):]
    lp = lp[:lp.index("\n}\n")]
    if "room = bx - 4 - gdi_width(f_tiny, pill) - 8 - nx;" not in lp or "clip_text(line, room < 170 ? room : 170);" not in lp:
        fails.append("a long lobby name can run into the player's state (ready, ping, version)")
    card = src[src.index("static int session_card("):]
    card = card[:card.index("\n}\n")]
    if "clip_text(line, w - 40 - off);" not in card or "who_text(s, w - 40 - off," not in card or "x + 20 + off" not in card or "heading(x + 20 + off, y + 9, hh," not in card:
        fails.append("a Saves card's text can run under the hero's face")
    if "hh > 14 && (font.px" not in card:
        fails.append("a Saves card's heading can run into the CO-OP pill beside the face")
    if "if (!plate.px) {" not in src or "if (!font.px) {" not in src:
        fails.append("the steel or the menu font has no fallback without mdk2.tar")


MUTANTS = {
    "label-not-fitted": ("float gh = !font.px ? 0 : primary ? ll_primary_height(glyph_adv, s, w, h) : ll_label_height(glyph_adv, s, w, h);",
                         "float gh = !font.px ? 0 : h * 0.6f;"),
    "saves-text-under-face": ("clip_text(line, w - 40 - off);", "clip_text(line, w - 40);"),
    "saves-heading-unfitted": ("while (hh > 14 && (font.px", "while (0 && (font.px"),
    "lobby-name-over-state": ("clip_text(line, room < 170 ? room : 170);", "clip_text(line, 170);"),
    "tenth-interface-row": ('toggle(ID, PX, y, PW, "Pings", "Mark a spot for everyone", &cfg.pings);',
                            'toggle(ID, PX, y, PW, "Pings", "Mark a spot for everyone", &cfg.pings);\n        y += ROW + 2;\n'
                            '        toggle(ID, PX, y, PW, "Extra", NULL, &cfg.pings);\n        y += ROW + 2;\n'
                            '        toggle(ID, PX, y, PW, "Extra two", NULL, &cfg.pings);'),
    "chooser-clips": ("if (gdi_width(vf, names[*v]) > 140)\n            vf = f_small", "if (0)\n            vf = f_small"),
    "about-overlap": ("if (gdi_width(f_small, vline) > room)       /*", "if (0)       /*"),
    "untagged-friendly-fire": ('"Friendly fire", "Host decides"', '"Friendly fire", "The host decides"'),
    "launcher-order": ('toggle(ID, PX, y, PW, "Friendly fire", "Host decides", &cfg.friendly_fire);',
                       'toggle(ID, PX, y, PW, "Back to level\'s hero", "Host decides", &cfg.char_follow);'
                       '\n        y += ROW + 2;\n        toggle(ID, PX, y, PW, "Friendly fire", "Host decides", &cfg.friendly_fire);'),
    "primary-says-host": ('lob_wants() ? "Open lobby" :', 'lob_wants() ? "Host" :'),
    "dps-row-gone": ('"DPS meter", "Ctrl+M in game changes it too"', '"Meter", "Ctrl+M in game changes it too"'),
    "dps-back-to-switch": ('choice(ID, PX, y, PW, "DPS meter", "Ctrl+M in game changes it too", dps_names, 3, &cfg.dps);', 'toggle(ID, PX, y, PW, "DPS meter", "Ctrl+M in game changes it too", &cfg.dps);'),
}
GUIDE_MUTANTS = {
    "guide-help-tab": ("Launcher > About > Send diagnostics", "Launcher > Help > Send diagnostics"),
    "guide-cut-line": ('"Friendly fire, enemy HP, damage, revive times"', '"Friendly fire, enemy HP & scaling, damage, down/revive time"'),
}
LUA_MUTANTS = {
    "game-untagged": ('"Co-op save every (host decides)"', '"Co-op save every"'),
    "game-order": ('item = mdkCreateMenuItem(m, "Difficulty (host decides): "', 'item = mdkCreateMenuItem(m, "Zzz: "'),
    "host-address-row": ('elseif (coop_role() == "host") then\n        -- the host has no host', 'elseif (coop_role() == "nobody") then\n        -- the host has no host'),
    "controls-line-cut": ('long="Hold to talk: players near you hear you (see Voice and sound)"',
                          'long="Hold to talk: players near you hear you (Options > Co-op > Voice and sound)"'),
    "dps-default-on": ('local v = tonumber(coop_get("hud", "dps", "0")) or 0', 'local v = tonumber(coop_get("hud", "dps", "1")) or 0'),
    "dps-switch-again": ("    coop.DpsItem(m)\n", '    coop.CheckItem(m, "DPS meter", "hud", "dps", "0", nil)\n'),
    "no-copy-invite-row": ('coop.invite_note or "Copy invite"', 'coop.invite_note or "Copy link"'),
    "invite-for-guests": ('        -- the launcher\'s Copy invite: the same text and code', '    end\n    if (coop_role() ~= "nobody") then\n        -- the launcher\'s Copy invite: the same text and code'),
    "no-leave-row": ('item = mdkCreateMenuItem(m, "Leave")', 'item = mdkCreateMenuItem(m, "Quit")'),
    "leave-without-confirm": ('item.Select = function(self) menu.SwitchDlg("quitconfirm") end', 'item.Select = function(self) coop.LeaveYes() end'),
    "dev-row-always": ('    if (coop_get("controls", "devkeys", "0") == "1") then\n        coop.CheckItem(m, "Developer keys"', '    if (1) then\n        coop.CheckItem(m, "Developer keys"'),
    "dev-row-in-rules": ('    item = mdkCreateMenuItem(m, "Back to level\'s hero (host decides)")', '    item = mdkCreateMenuItem(m, "Developer keys")\n    item = mdkCreateMenuItem(m, "Back to level\'s hero (host decides)")'),
}
COMBAT_MUTANTS = {
    "ctrl-m-not-written-back": ('WritePrivateProfileStringA("hud", "dps", val, ini);', 'val[0] = val[0];'),
}


def test_launcher_layout():
    src = (ROOT / "bridge/launcher.c").read_text()
    lua = (ROOT / "lua/coop_dev.lua").read_text()
    combat = (ROOT / "bridge/combat.c").read_text()
    LW, LH = define(src, "LW"), define(src, "LH")
    PX, PW = define(src, "PX"), define(src, "PW")

    # Common resolutions: (work_w, work_h, desc)
    resolutions = [
        (1280, 720, "1280x720 100%"),
        (1280, 800, "1280x800 100% (Steam Deck)"),
        (1600, 900, "1600x900 125%"),
        (1920, 1080, "1920x1080 150%"),
        (2560, 1440, "2560x1440 200%"),
        (3840, 2160, "3840x2160 300%"),
    ]

    # A1: four tabs, in this order
    tabs = re.search(r"static const char \*tabs\[\] = \{(.*?)\};", src, re.S).group(1)
    assert re.findall(r'"([^"]+)"', tabs) == ["Play", "Saves", "Settings", "About"], "launcher tabs are not A1's four"

    for work_w, work_h, desc in resolutions:
        # Launcher scale computation from bridge/launcher.c:
        sc = work_w * 0.8 / LW
        if (work_h * 0.85 / LH) < sc:
            sc = work_h * 0.85 / LH
        sc = min(2.0, max(0.5, sc))
        fw = int(LW * sc + 0.5)
        fh = int(LH * sc + 0.5)

        assert fw <= work_w, f"{desc}: window width {fw} exceeds work area width {work_w}"
        assert fh <= work_h, f"{desc}: window height {fh} exceeds work area height {work_h}"

        def px(v):
            return int(v * sc + 0.5)

        # the panel's content column, the close button and the primary button stay on the canvas
        assert px(PX + PW) <= fw, f"{desc}: panel exceeds page width"
        assert px(LW - 52 + 34) <= fw and px(18 + 34) <= fh, f"{desc}: close button off canvas"
        assert px(LH - 92 + 64) <= fh, f"{desc}: primary button overflows canvas"
        # lobby: 16 member rows end above the Leave / chat row
        lob_btn = LH - 100
        rows = [float(v) for v in re.findall(r"row_h = lob\.n > 12 \? ([\d.]+)f : ([\d.]+)f", src)[0]]
        assert px(126 + 16 * rows[0]) <= px(lob_btn), f"{desc}: 16 lobby members overflow into the buttons"
        # the hero's portrait sits inside its row, and the name after it leaves room before the Mute / Kick buttons
        for rh in rows:
            assert rh - 7 >= 12 and px(2 + rh - 7) <= px(rh - 3), f"{desc}: the portrait does not fit a {rh} row"
            name_x = 26 + rh - 2
            buttons_x = 392 - 8 - 44 - 4 - 50 - 4
            assert px(name_x + 60) < px(buttons_x), f"{desc}: no room for a name beside the portrait in a {rh} row"

    # a Saves card (98 tall, PW wide): the face fits inside it above the progress bar, and the text keeps a readable width
    face = define(src, "SAVE_FACE")
    for work_w, work_h, desc in resolutions:
        sc = min(2.0, max(0.5, min(work_w * 0.8 / LW, work_h * 0.85 / LH)))
        px = lambda v: int(v * sc + 0.5)
        assert px(12 + face) <= px(98 - 5), f"{desc}: the face does not fit a Saves card above its bar"
        assert PW - 40 - (face + 12) >= 300, f"{desc}: too little room for a Saves card's text beside the face"

    fails = []
    check_settings_fit(src, fails)
    check_menu_parity(src, lua, combat, fails)
    check_text_fit(src, fails)
    check_primary_button(src, fails)
    check_game_look(src, fails)
    ctrl = (ROOT / "bridge/ctrlview.c").read_text()
    check_guide_text(ctrl, src, fails)
    check_controls_lines(lua, fails)
    assert not fails, "\n".join(fails)
    for name, (old, new) in MUTANTS.items():
        assert src.count(old) == 1, f"mutant {name}: expected one match, found {src.count(old)}"
        bad = []
        m = src.replace(old, new)
        check_settings_fit(m, bad)
        check_menu_parity(m, lua, combat, bad)
        check_text_fit(m, bad)
        check_primary_button(m, bad)
        check_game_look(m, bad)
        assert bad, f"mutant {name} was NOT caught"
    for name, (old, new) in GUIDE_MUTANTS.items():
        assert ctrl.count(old) == 1, f"mutant {name}: expected one match, found {ctrl.count(old)}"
        bad = []
        check_guide_text(ctrl.replace(old, new), src, bad)
        assert bad, f"mutant {name} was NOT caught"
    for name, (old, new) in COMBAT_MUTANTS.items():
        assert combat.count(old) == 1, f"mutant {name}: expected one match, found {combat.count(old)}"
        bad = []
        check_menu_parity(src, lua, combat.replace(old, new), bad)
        assert bad, f"mutant {name} was NOT caught"
    for name, (old, new) in LUA_MUTANTS.items():
        assert lua.count(old) == 1, f"mutant {name}: expected one match, found {lua.count(old)}"
        bad = []
        check_menu_parity(src, lua.replace(old, new), combat, bad)
        check_controls_lines(lua.replace(old, new), bad)
        assert bad, f"mutant {name} was NOT caught"


def test_installer_layout():
    # Installer window: ClientSize (760, 540), MinSize (640, 520)
    for scale in (1.0, 1.25, 1.5, 2.0):
        w = int(760 * scale)
        h = int(540 * scale)
        # Header: h = 91 * scale
        header_h = int(91 * scale)
        # Path textbox: x=26, y=154, w=470, h=28
        # Browse button: x=505, y=151, w=108, h=32
        bx = int(505 * scale)
        bw = int(108 * scale)
        assert bx + bw <= w, f"scale {scale}: Browse button exceeds width"
        # Inventory listview: x=26, y=255, w=702, h=205
        # Bottom buttons: y=480, h=40
        # Refresh: x=26, w=110
        # Install: x=430, w=143
        # Uninstall: x=585, w=143
        inv_y = int(255 * scale)
        inv_h = int(205 * scale)
        btn_y = int(480 * scale)
        btn_h = int(40 * scale)
        assert inv_y + inv_h <= btn_y, f"scale {scale}: inventory overlaps bottom buttons"
        assert btn_y + btn_h <= h, f"scale {scale}: bottom buttons exceed window height"

        ref_x = int(26 * scale)
        ref_w = int(110 * scale)
        ins_x = int(430 * scale)
        ins_w = int(143 * scale)
        uni_x = int(585 * scale)
        uni_w = int(143 * scale)
        assert ref_x + ref_w <= ins_x, f"scale {scale}: Refresh overlaps Install button"
        assert ins_x + ins_w <= uni_x, f"scale {scale}: Install overlaps Uninstall button"
        assert uni_x + uni_w <= w, f"scale {scale}: Uninstall button exceeds window width"


if __name__ == "__main__":
    test_launcher_layout()
    test_installer_layout()
    print("PASS: launcher and setup UI layout, menu parity and text-fit checks at 1280x720, 1280x800 and 100%, 125%, 150%, 200%, 300% "
          f"({len(MUTANTS) + len(LUA_MUTANTS) + len(GUIDE_MUTANTS) + len(COMBAT_MUTANTS)} mutants caught)")
