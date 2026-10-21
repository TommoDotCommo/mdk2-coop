#!/usr/bin/env bash
# Offline network tests under WSL/Windows. Never connects to Steam or starts the game.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
out="$here/local/tests/network"
mkdir -p "$out"
i686-w64-mingw32-gcc -O2 -Wall -Wextra -Wno-unused-parameter \
    -o "$out/test_network.exe" "$here/tools/test_network.c" -lws2_32 -static-libgcc
winout="$(wslpath -w "$out")"
powershell.exe -NoProfile -Command "Push-Location '$winout'; & '.\test_network.exe'; \$code = \$LASTEXITCODE; Pop-Location; exit \$code"
