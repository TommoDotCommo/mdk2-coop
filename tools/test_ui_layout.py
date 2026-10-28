#!/usr/bin/env python3
"""Game-free UI layout and DPI scaling verification for launcher and installer."""
import sys


def test_launcher_layout():
    LW, LH = 1280, 720
    PX, PW = 58, 540

    # Common resolutions: (work_w, work_h, desc)
    resolutions = [
        (1280, 720, "1280x720 100%"),
        (1600, 900, "1600x900 125%"),
        (1920, 1080, "1920x1080 150%"),
        (2560, 1440, "2560x1440 200%"),
        (3840, 2160, "3840x2160 300%"),
    ]

    for work_w, work_h, desc in resolutions:
        # Launcher scale computation from bridge/launcher.c:
        sc = work_w * 0.8 / LW
        if (work_h * 0.85 / LH) < sc:
            sc = work_h * 0.85 / LH
        fw = int(LW * sc + 0.5)
        fh = int(LH * sc + 0.5)

        assert fw <= work_w, f"{desc}: window width {fw} exceeds work area width {work_w}"
        assert fh <= work_h, f"{desc}: window height {fh} exceeds work area height {work_h}"

        def px(v):
            return int(v * sc + 0.5)

        # 1. Header & Tabs
        tabs = [("Play", 58, 48, 80, 32), ("Saved", 146, 48, 80, 32),
                ("Settings", 234, 48, 90, 32), ("About", 332, 48, 80, 32),
                ("Profiles", 420, 48, 90, 32), ("Help", 518, 48, 80, 32)]
        for name, tx, ty, tw, th in tabs:
            x0, y0, x1, y1 = px(tx), px(ty), px(tx + tw), px(ty + th)
            assert 0 <= x0 < x1 <= fw, f"{desc}: tab {name} out of bounds"
            assert 0 <= y0 < y1 <= fh, f"{desc}: tab {name} out of bounds"

        # 2. Lobby page widgets
        # Voice toggle button
        vbtn = (PX + PW - 168, 88 - 2, 168, 28)
        # 16 Member rows
        for i in range(16):
            row_h = 11.0 if 16 > 8 else 21.0
            ry = 120 + i * row_h
            assert px(ry + row_h) <= px(302), f"{desc}: member row {i} overflows into chat"
        # Chat box: y=302, h=114
        assert px(302 + 114) <= px(420), f"{desc}: chat box overlaps input"
        # Chat field: y=420, w=PW-88, h=40; Send button: x=PX+PW-80, y=444, w=80, h=40
        assert px(PX + PW) <= fw, f"{desc}: chat controls exceed page width"
        # Primary start / ready button: y=LH-160, h=64
        assert px(LH - 160 + 64) <= fh, f"{desc}: start button overflows canvas"

        # 3. Persistent lobby bar (drawn on non-play pages when lobby active)
        # Status text: LH-118
        # Action button 1: PX, LH-92, PW/2 - 5, 52
        # Action button 2: PX + PW/2 + 5, LH-92, PW/2 - 5, 52
        assert px(LH - 92 + 52) <= fh, f"{desc}: persistent lobby bar buttons overflow canvas"

        # 4. Profiles page widgets
        # 4 profile cards: y=150, step=72, h=66
        for i in range(4):
            cy = 150 + i * 72
            assert px(cy + 66) <= px(446), f"{desc}: profile card {i} overlaps nav links"
        # Bottom profile action buttons: y=476, h=38
        assert px(476 + 38) <= px(LH - 118), f"{desc}: profile buttons overlap persistent bar"

        # 5. About / Updates page widgets
        # Update check button: y ~ 280, w=144, h=36
        # Install update button: x=PX+154, w=160, h=36
        # Roll back button: x=PX+324, w=152, h=36
        assert px(PX + 324 + 152) <= px(PX + PW), f"{desc}: update buttons exceed page width"


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
    print("PASS: launcher and setup UI layout and DPI geometry checks at 100%, 125%, 150%, 200%, 300%")
