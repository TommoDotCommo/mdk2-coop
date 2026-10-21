#!/usr/bin/env bash
# Build the player zip: local/release/mdk2-coop-<version>.zip with dinput8.dll, coop\coop.lua,
# the installer (Install.cmd, Uninstall.cmd, mdk2-coop.ps1), README.txt and LICENSE.
#   tools/release.sh            version from `git describe --tags --always`
#   tools/release.sh v0.1.0     explicit version
# Publishing is a separate step: gh release create <version> local/release/mdk2-coop-<version>.zip
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
ver="${1:-$(git -C "$here" describe --tags --always --dirty)}"
name="mdk2-coop-$ver"
out="$here/local/release"
stage="$out/$name"

make -s -C "$here/bridge"
rm -rf "$stage" "$out/$name.zip"
mkdir -p "$stage/coop"
cp "$here/bridge/dinput8.dll" "$stage/"
sed 's/\r$//; s/$/\r/' "$here/lua/coop_dev.lua" > "$stage/coop/coop.lua"
for f in Install.cmd Uninstall.cmd mdk2-coop.ps1; do sed 's/\r$//; s/$/\r/' "$here/dist/$f" > "$stage/$f"; done
sed 's/\r$//; s/$/\r/; s/@VERSION@/'"$ver"'/' "$here/dist/README.txt" > "$stage/README.txt"
sed 's/\r$//; s/$/\r/' "$here/LICENSE" > "$stage/LICENSE.txt"
(cd "$out" && python3 -c 'import sys, zipfile, os
z = zipfile.ZipFile(sys.argv[1] + ".zip", "w", zipfile.ZIP_DEFLATED)
for root, _, files in os.walk(sys.argv[1]):
    for f in sorted(files):
        z.write(os.path.join(root, f))
z.close()' "$name")
echo "$out/$name.zip ($(du -h "$out/$name.zip" | cut -f1))"
