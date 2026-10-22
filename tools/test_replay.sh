#!/usr/bin/env bash
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
python3 - "$here" <<'PY'
from pathlib import Path
import subprocess
import sys
import tempfile
import shutil

root = Path(sys.argv[1])
(root / "local/tests").mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix="route-", dir=root / "local/tests") as temp:
    binary = str(Path(temp) / "route-test")
    subprocess.run(["cc", "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror",
                    str(root / "tests/route_test.c"), str(root / "bridge/route.c"), "-lm", "-o", binary], check=True)
    subprocess.run([binary], check=True)
    if shutil.which("i686-w64-mingw32-gcc") and Path("/proc/sys/fs/binfmt_misc/WSLInterop").exists():
        windows_binary = str(Path(temp) / "input-test.exe")
        subprocess.run(["i686-w64-mingw32-gcc", "-O2", "-Wall", "-Wextra", "-Wno-unused-parameter",
                        str(root / "tests/input_test.c"), "-ldxguid", "-lgdi32", "-lwinmm",
                        "-static-libgcc", "-o", windows_binary], check=True)
        subprocess.run([windows_binary], check=True)
PY
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s "$here/tests" -p '*_test.py'
bash -n "$here/tools/replay.sh"
