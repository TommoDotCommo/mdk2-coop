#!/usr/bin/env bash
# Build a 32-bit command helper that works from either PowerShell architecture.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
resource=$(mktemp --suffix=.o)
vres=$(mktemp --suffix=.o)
vrc=$(mktemp --suffix=.rc)
trap 'rm -f "$resource" "$vres" "$vrc"' EXIT
# An explicit execution level prevents Windows installer-name heuristics from
# demanding elevation for this read-only helper in an extracted download.
(cd "$root/dist" && i686-w64-mingw32-windres install-policy.rc -o "$resource")
# VERSIONINFO with the mod version (MOD_VERSION from tools/release.sh, else git describe)
version="${MOD_VERSION:-$(git -C "$root" describe --tags --always --dirty 2>/dev/null || echo dev)}"
rm -f "$vrc"; python3 "$root/tools/versioninfo.py" "$version" install-policy.exe "MDK2 Co-op Setup policy helper" "$vrc"
i686-w64-mingw32-windres "$vrc" -o "$vres"
i686-w64-mingw32-gcc -O2 -Wall -Wextra -Werror -I"$root/bridge" -o "${1:-$root/dist/install-policy.exe}" "$root/dist/install-policy.c" "$resource" "$vres"
