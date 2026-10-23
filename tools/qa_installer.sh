#!/usr/bin/env bash
# Validate the current installer against disposable fixtures, reading only a leased executable.
# No game is launched. The source executable and the owner's saves/settings are never changed.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
source "$here/tools/mdk.sh"
: "${MDK2_RUN:?set MDK2_RUN to a disposable directory on a Windows filesystem}"
make -s -C "$here/bridge"
mkdir -p "$here/local/installer"
out=$(mktemp -d "$here/local/installer/run-XXXXXXXX")
# Win32 profile API fixtures need a Windows filesystem.
fixtures=$(mktemp -d "$MDK2_RUN/installer-XXXXXXXX")
printf '%s\n' "$fixtures" > "$out/fixtures.txt"
mkdir -p "$out/package/coop"
cp "$here/bridge/dinput8.dll" "$out/package/"
cp "$here/lua/coop_dev.lua" "$out/package/coop/coop.lua"
cp "$here/dist/mdk2-coop.ps1" "$out/package/"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(wslpath -w "$here/tools/test_installer.ps1")" \
    -GameExe "$(mdk_dir 1)\\mdk2hd.exe" -PackageDir "$(wslpath -w "$out/package")" \
    -TestRoot "$(wslpath -w "$fixtures")"
echo "installer fixtures: $out"
