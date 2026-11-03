#!/usr/bin/env python3
"""Builds the Vortex extension zip for the Nexus "Vortex extensions" page (and copies the MO2 plugin beside it).

  build.py <output folder>     writes mdk2hd-vortex-extension.zip (game-mdk2hd/ contents at the zip root) and game_mdk2hd.py
"""
from pathlib import Path
import shutil
import sys
import zipfile

HERE = Path(__file__).resolve().parent
NAME = 'mdk2hd-vortex-extension.zip'


def build(out):
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    src = HERE / 'vortex' / 'game-mdk2hd'
    zpath = out / NAME
    with zipfile.ZipFile(zpath, 'w', zipfile.ZIP_DEFLATED) as z:
        for f in sorted(src.iterdir()):                 # deterministic: fixed order and timestamps
            info = zipfile.ZipInfo(f.name, date_time=(2020, 1, 1, 0, 0, 0))
            info.external_attr = 0o100644 << 16
            z.writestr(info, f.read_bytes(), compress_type=zipfile.ZIP_DEFLATED)
    shutil.copyfile(HERE / 'mo2' / 'game_mdk2hd.py', out / 'game_mdk2hd.py')
    return zpath


if __name__ == '__main__':
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        sys.exit(2)
    print(build(sys.argv[1]))
