#!/usr/bin/env bash
# Build a 32-bit command helper that works from either PowerShell architecture.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
i686-w64-mingw32-gcc -O2 -Wall -Wextra -Werror -o "${1:-$root/dist/install-policy.exe}" "$root/dist/install-policy.c"
