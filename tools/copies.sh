#!/usr/bin/env bash
# Make game copies for more players: copies 1..MDK2_N (see tools/mdk.sh for the folders) from
# MDK2_SRC (required). mdk2.tar (1.6 GB) is hard-linked, so a copy costs ~2 MB.
# Configure disposable destinations first; see docs/DEVELOPMENT.md.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
source "$here/mdk.sh"
src="$(mdk_wsl "${MDK2_SRC:?set MDK2_SRC to a source copy}")"
for i in $(mdk_seq); do
    d="$(mdk_wsl "$(mdk_dir "$i")")"
    [ "$d" = "$src" ] && continue
    mkdir -p "$d/coop"
    for f in mdk2hd.exe steam_api.dll steam_appid.txt; do cp -u "$src/$f" "$d/"; done
    [ -e "$d/mdk2.tar" ] || ln "$src/mdk2.tar" "$d/mdk2.tar"
    echo "$d: $(stat -c '%h links' "$d/mdk2.tar")"
done
