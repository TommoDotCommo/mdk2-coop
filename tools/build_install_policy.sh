#!/usr/bin/env bash
# Build a 32-bit command helper that works from either PowerShell architecture.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
resource=$(mktemp --suffix=.o)
trap 'rm -f "$resource"' EXIT
# An explicit execution level prevents Windows installer-name heuristics from
# demanding elevation for this read-only helper in an extracted download.
(cd "$root/dist" && i686-w64-mingw32-windres install-policy.rc -o "$resource")
i686-w64-mingw32-gcc -O2 -Wall -Wextra -Werror -o "${1:-$root/dist/install-policy.exe}" "$root/dist/install-policy.c" "$resource"
