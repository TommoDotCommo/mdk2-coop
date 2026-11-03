#!/bin/sh
# mdk2-coop manual install for Linux and the Steam Deck (the game runs through Proton). POSIX sh, coreutils, sed and awk.
#   sh install-linux.sh install   <MDK2 HD game folder> [--yes]   copy the two mod files in, keeping what was there
#   sh install-linux.sh uninstall <MDK2 HD game folder> [--yes]   take out exactly what install put there, put back the originals
#   sh install-linux.sh status [--json] [<MDK2 HD game folder>]   what is installed (without a folder: the Steam libraries found)
# --yes: no questions (this script asks none; accepted so every co-op Setup takes the same words).
# --accept-unknown-build: install although mdk2hd.exe is not a build the mod knows (co-op then stays off in the game).
# Exit codes: 0 done, 1 failed, 2 usage, 3 unsupported game build, 4 the game is running.
# The game folder is the one that holds mdk2hd.exe, e.g. ~/.local/share/Steam/steamapps/common/"MDK 2 HD".
# Run it from the unpacked release folder (it reads dinput8.dll and coop/coop.lua beside itself).
# Two files change: <game folder>/dinput8.dll and <game folder>/coop/coop.lua. Another mod's dinput8.dll that was there is
# kept as dinput8.chain.dll (the co-op DLL loads it, so both keep working); any other file that was there is kept as
# <file>.coop-original. A record of what was installed lives in <game folder>/coop/install-linux.record; uninstall removes
# only files whose content still matches that record, and refuses to do anything without it.
# Proton has its own built-in dinput8; the game must be told to load the mod's. When the game's Proton prefix exists
# (steamapps/compatdata/213350/pfx), install writes "dinput8"="native,builtin" for mdk2hd.exe into its user.reg (a backup
# first; uninstall undoes exactly that). Without a prefix (the game never started yet), set this Steam launch option for
# MDK2 HD (Properties > General > Launch Options), or start the game once and run install again:
#   WINEDLLOVERRIDES="dinput8=n,b" %command%
set -eu

here=$(cd "$(dirname "$0")" && pwd)
record_name=coop/install-linux.record
proton_name=coop/install-linux.proton
files="dinput8.dll coop/coop.lua"
appid=213350
exe_name=mdk2hd.exe
OVERRIDE_KEY="[Software\\\\Wine\\\\AppDefaults\\\\$exe_name\\\\DllOverrides]"
OVERRIDE_LINE='"dinput8"="native,builtin"'
export OVERRIDE_KEY OVERRIDE_LINE     # awk reads them from ENVIRON (awk -v would undo the doubled backslashes)
launch_option='WINEDLLOVERRIDES="dinput8=n,b" %command%'
mark='mdk2-coop bridge attached'      # a string only the mod's own DLL contains

die() { echo "install-linux: $*" >&2; exit 1; }
hash_of() { sha256sum "$1" | cut -d' ' -f1; }
usage() {
    echo "usage: sh install-linux.sh install|uninstall <game folder with mdk2hd.exe> [--yes] [--accept-unknown-build]" >&2
    echo "       sh install-linux.sh status [--json] [<game folder>]" >&2
    exit 2
}

# ---- arguments ----
[ $# -ge 1 ] || usage
action=$1; shift
json=0; yes=0; accept_unknown=0; game=
for a in "$@"; do
    case "$a" in
        --json) json=1 ;;
        --yes|-y) yes=1 ;;
        --accept-unknown-build) accept_unknown=1 ;;
        -*) usage ;;
        *) [ -z "$game" ] || usage; game=$a ;;
    esac
done
case "$action" in
    install|uninstall) [ -n "$game" ] && [ $json -eq 0 ] || usage ;;
    status) [ $accept_unknown -eq 0 ] || usage ;;
    *) usage ;;
esac
: "$yes"

# ---- which game build is this? (bridge/build_sigs.h: the builtin table entry of "copytagmethods" points at that string) ----
u16() { od -An -tu2 -j "$2" -N2 "$1" 2>/dev/null | tr -d ' \n'; }
u32() { od -An -tu4 -j "$2" -N4 "$1" 2>/dev/null | tr -d ' \n'; }
va_to_off() {   # file va -> file offset (32-bit PE sections), or nothing
    f=$1; va=$2
    [ "$(od -An -tx1 -N2 "$f" 2>/dev/null | tr -d ' \n')" = 4d5a ] || return 0
    pe=$(u32 "$f" 60); [ -n "$pe" ] || return 0
    [ "$(od -An -tx1 -j "$pe" -N4 "$f" 2>/dev/null | tr -d ' \n')" = 50450000 ] || return 0
    nsec=$(u16 "$f" $((pe + 6))); opt=$(u16 "$f" $((pe + 20)))
    [ "$(u16 "$f" $((pe + 24)))" = 267 ] || return 0
    base=$(u32 "$f" $((pe + 52)))
    [ "$va" -ge "$base" ] || return 0
    rva=$((va - base)); i=0
    while [ "$i" -lt "$nsec" ] && [ "$i" -lt 96 ]; do
        s=$((pe + 24 + opt + i * 40))
        vsize=$(u32 "$f" $((s + 8))); vaddr=$(u32 "$f" $((s + 12))); rawsize=$(u32 "$f" $((s + 16))); raw=$(u32 "$f" $((s + 20)))
        [ -n "$raw" ] || return 0
        span=$vsize; [ "$rawsize" -lt "$span" ] && span=$rawsize
        if [ "$rva" -ge "$vaddr" ] && [ $((rva - vaddr)) -lt "$span" ]; then echo $((raw + rva - vaddr)); return 0; fi
        i=$((i + 1))
    done
}
build_of() {    # prints the build name, or nothing for an unknown build
    for b in "Steam build 3902:$((0x506520)):$((0x4d7d34))" "Overhaul Games build:$((0x505510)):$((0x4d769c))"; do
        name=${b%%:*}; rest=${b#*:}; entry=${rest%%:*}; str=${rest#*:}
        e=$(va_to_off "$1" "$entry"); n=$(va_to_off "$1" "$str")
        [ -n "$e" ] && [ -n "$n" ] || continue
        [ "$(u32 "$1" "$e")" = "$str" ] || continue
        [ "$(od -An -c -j "$n" -N14 "$1" | tr -d ' \n')" = copytagmethods ] || continue
        [ "$(od -An -tu1 -j $((n + 14)) -N1 "$1" | tr -d ' \n')" = 0 ] || continue
        echo "$name"; return 0
    done
}

# ---- is the game running? (under Proton the process is named mdk2hd.exe, and its first argument is the exe's path) ----
game_running() {
    for d in "${INSTALL_LINUX_PROC:-/proc}"/[0-9]*; do
        [ -r "$d/cmdline" ] || continue
        comm=$(cat "$d/comm" 2>/dev/null || true)
        arg0=$(tr '\000' '\n' < "$d/cmdline" 2>/dev/null | head -n 1 | tr 'A-Z\\' 'a-z/')
        case "$(printf '%s' "$comm" | tr 'A-Z' 'a-z')" in "$exe_name") return 0 ;; esac
        case "$arg0" in *"/$exe_name"|"$exe_name") return 0 ;; esac
    done
    return 1
}

# ---- the Proton prefix of the game: <steamapps>/compatdata/213350/pfx/user.reg ----
prefix_reg() {  # $1 = game folder; prints the user.reg path when the prefix exists
    steamapps=$(cd "$1/../.." 2>/dev/null && pwd) || return 0
    [ "$(basename "$(dirname "$(cd "$1" && pwd)")")" = common ] || return 0
    reg="$steamapps/compatdata/$appid/pfx/user.reg"
    [ -f "$reg" ] && echo "$reg"
    return 0
}
override_present() {    # $1 = user.reg: 0 when our exe's DllOverrides key holds dinput8=native,builtin
    awk 'BEGIN { key = ENVIRON["OVERRIDE_KEY"]; line = ENVIRON["OVERRIDE_LINE"] }
        index(tolower($0), tolower(key)) == 1 { inside = 1; next }
        /^\[/ { inside = 0 }
        inside && $0 == line { found = 1 }
        END { exit found ? 0 : 1 }' "$1"
}
write_override() {     # $1 = game folder
    reg=$(prefix_reg "$1")
    if [ -z "$reg" ]; then
        echo "No Proton prefix for MDK2 HD yet (start the game once, then run this again), or set this launch option for MDK2 HD"
        echo "in Steam (Properties > General > Launch Options):"
        echo "  $launch_option"
        return 0
    fi
    refuse_link "$reg"
    [ ! -e "$reg.mdk2-coop-backup" ] || die "$reg.mdk2-coop-backup already exists; resolve it by hand first"
    if override_present "$reg"; then
        echo "  the Proton prefix already loads the mod's dinput8 for $exe_name"
        return 0
    fi
    cp -p "$reg" "$reg.mdk2-coop-backup" || die "could not back up $reg"
    tmp="$reg.mdk2-coop-new"
    # inside an existing key: a "dinput8" value of its own is replaced (and remembered), else our line is added under the key
    old=$(awk 'BEGIN { key = ENVIRON["OVERRIDE_KEY"] }
        index(tolower($0), tolower(key)) == 1 { inside = 1; next }
        /^\[/ { inside = 0 }
        inside && /^"dinput8"=/ { print; exit }' "$reg")
    awk -v now="$(date +%s)" 'BEGIN { key = ENVIRON["OVERRIDE_KEY"]; line = ENVIRON["OVERRIDE_LINE"] }
        index(tolower($0), tolower(key)) == 1 { print; inside = 1; seen = 1; next }
        inside && /^#time=/ { print; next }
        inside && !done { print line; done = 1 }
        inside && /^"dinput8"=/ { next }
        /^\[/ { inside = 0 }
        { print }
        END {
            if (inside && !done) print line
            if (!seen) { print ""; print key " " now; print line }
        }' "$reg" > "$tmp" || die "could not write $tmp"
    mv -f "$tmp" "$reg" || die "could not write $reg"
    override_present "$reg" || die "the DLL override did not land in $reg"
    { echo "$reg"; echo "$(hash_of "$reg")"; echo "${old:--}"; } > "$game/$proton_name"
    echo "  Proton prefix: $exe_name loads the mod's dinput8 ($reg; the old file is kept as user.reg.mdk2-coop-backup)"
}
undo_override() {      # $1 = game folder
    info="$1/$proton_name"
    [ -f "$info" ] || return 0
    reg=$(sed -n 1p "$info"); sum=$(sed -n 2p "$info"); old=$(sed -n 3p "$info")
    if [ -f "$reg" ] && [ "$(hash_of "$reg")" = "$sum" ] && [ -f "$reg.mdk2-coop-backup" ]; then
        mv -f "$reg.mdk2-coop-backup" "$reg"         # untouched since install: the exact old file
        echo "  Proton prefix: restored user.reg as it was"
    elif [ -f "$reg" ]; then                          # Wine rewrote it since: take out only our line
        tmp="$reg.mdk2-coop-new"
        OLD_VALUE=$old awk 'BEGIN { key = ENVIRON["OVERRIDE_KEY"]; line = ENVIRON["OVERRIDE_LINE"]; old = ENVIRON["OLD_VALUE"] }
            index(tolower($0), tolower(key)) == 1 { inside = 1; print; next }
            /^\[/ { inside = 0 }
            inside && $0 == line { if (old != "-") print old; next }
            { print }' "$reg" > "$tmp" && mv -f "$tmp" "$reg"
        rm -f "$reg.mdk2-coop-backup"
        echo "  Proton prefix: removed the mod's DLL override (Wine changed user.reg since install, so only that line)"
    fi
    rm -f "$info"
}

# never write through a link that points somewhere else
refuse_link() { [ ! -L "$1" ] || die "$1 is a symbolic link; not touching it"; }
is_ours() { [ -f "$1" ] && grep -qa "$mark" "$1"; }

keep_original() {     # $1 = target; prints how it was kept: chain, backup or new
    [ -e "$1" ] || { echo new; return 0; }
    keep="$1.coop-original"; how=backup
    if [ "$(basename "$1")" = dinput8.dll ] && ! is_ours "$1"; then keep="$(dirname "$1")/dinput8.chain.dll"; how=chain; fi
    [ ! -e "$keep" ] || die "$keep already exists; resolve it by hand first"
    cp -p "$1" "$keep" || die "could not keep $1"
    [ "$(hash_of "$1")" = "$(hash_of "$keep")" ] || die "the copy of $1 did not verify"
    echo "  kept the existing $(basename "$1") as $(basename "$keep")" >&2
    echo "$how"
}

check_game() {
    [ -d "$game" ] || die "not a folder: $game"
    [ -f "$game/$exe_name" ] || die "no $exe_name in $game: give the MDK2 HD game folder"
    if game_running; then echo "install-linux: MDK2 HD is running. Close the game and run this again." >&2; exit 4; fi
}

do_install() {
    for f in $files; do [ -f "$here/$f" ] || die "$here/$f is missing: run this from the unpacked release folder"; done
    build=$(build_of "$game/$exe_name")
    if [ -z "$build" ]; then
        echo "This game build is not one the co-op mod knows, so co-op would stay off (the game itself plays as before)." >&2
        echo "Supported: MDK2 HD on Steam (build 3902) and the Overhaul Games build. In Steam: Properties > Betas > None, then" >&2
        echo "Installed Files > Verify integrity of game files. To install anyway: add --accept-unknown-build." >&2
        [ $accept_unknown -eq 1 ] || exit 3
    else
        echo "Found: MDK2 HD $build"
    fi
    [ ! -e "$record" ] || die "already installed here (see $record). Uninstall first to reinstall."
    mkdir -p "$game/coop"
    : > "$record" || die "cannot write in $game/coop"      # filled one file at a time: an interrupted install can still be undone
    for f in $files; do
        target="$game/$f"
        refuse_link "$target"
        had=$(keep_original "$target")
        tmp="$target.coop-new"
        cp "$here/$f" "$tmp" || die "could not copy $f"
        mv -f "$tmp" "$target" || die "could not place $f"
        echo "$(hash_of "$target") $had $f" >> "$record"
        echo "  installed $f"
    done
    write_override "$game"
    echo "Done."
}

do_uninstall() {
    [ -f "$record" ] || die "no record of an install in $game ($record_name is missing): nothing was removed"
    while read -r sum had f; do
        target="$game/$f"
        case "$f" in dinput8.dll|coop/coop.lua) ;; *) die "unexpected entry in $record" ;; esac
        if [ ! -e "$target" ]; then echo "  $f is already gone"
        elif [ ! -f "$target" ] || [ -L "$target" ]; then echo "  left $f: not a plain file"; continue
        elif [ "$(hash_of "$target")" != "$sum" ]; then echo "  left $f: it changed since install (an update or your edit)"; continue
        else
            rm -f "$target"
            echo "  removed $f"
        fi
        kept="$target.coop-original"
        [ "$had" = chain ] && kept="$game/dinput8.chain.dll"
        if [ "$had" != new ] && [ -f "$kept" ] && [ ! -e "$target" ]; then
            mv "$kept" "$target"
            echo "  put back the original $f"
        fi
    done < "$record"
    undo_override "$game"
    rm -f "$record"
    echo "Done. The co-op data (settings, logs, saves) in the Proton prefix's %LOCALAPPDATA%\\mdk2-coop was kept."
}

# ---- status ----
json_str() { printf '"%s"' "$(printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g')"; }
manifest_hash() {   # $1 = relative path: its SHA-256 in the coop-mod.json beside this script (or in the game folder)
    for m in "$here/coop-mod.json" "$2/coop-mod.json"; do
        [ -f "$m" ] || continue
        tr -d '\n ' < "$m" | sed 's/},{/}\n{/g' | grep "\"path\":\"$1\"" | sed -n 's/.*"sha256":"\([0-9a-f]\{64\}\)".*/\1/p' | head -1
        return 0
    done
}
links_of() { stat -c %h "$1" 2>/dev/null || echo 1; }
state_of() {    # $1 = game folder: installed, modified, mod-manager, vortex, foreign or not-installed
    g=$1
    if [ -f "$g/$record_name" ]; then
        while read -r sum had f; do
            [ -f "$g/$f" ] && [ "$(hash_of "$g/$f")" = "$sum" ] || { echo modified; return 0; }
        done < "$g/$record_name"
        echo installed; return 0
    fi
    [ -f "$g/dinput8.dll" ] || { echo not-installed; return 0; }
    match=1
    for f in $files; do
        want=$(manifest_hash "$f" "$g")
        { [ -n "$want" ] && [ -f "$g/$f" ] && [ "$(hash_of "$g/$f")" = "$want" ]; } || match=0
    done
    if [ $match -eq 1 ] || is_ours "$g/dinput8.dll"; then
        if [ "$(links_of "$g/dinput8.dll")" -gt 1 ]; then echo vortex; else echo mod-manager; fi
        return 0
    fi
    echo foreign
}
steam_games() {     # MDK2 HD folders in the Steam libraries this user has
    for root in "$HOME/.local/share/Steam" "$HOME/.steam/steam" "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam"; do
        [ -d "$root/steamapps" ] || continue
        echo "$root/steamapps/common/MDK 2 HD"
        vdf="$root/steamapps/libraryfolders.vdf"
        [ -f "$vdf" ] && sed -n 's/^[[:space:]]*"path"[[:space:]]*"\(.*\)"[[:space:]]*$/\1/p' "$vdf" | while IFS= read -r lib; do
            echo "$lib/steamapps/common/MDK 2 HD"; done
    done | awk '!seen[$0]++' | while IFS= read -r d; do [ -f "$d/$exe_name" ] && echo "$d"; done
}
do_status() {
    if [ -n "$game" ]; then list=$game; else list=$(steam_games); fi
    version=$(tr -d '\n' < "$here/coop-mod.json" 2>/dev/null | sed -n 's/.*"version":[[:space:]]*"\([^"]*\)".*/\1/p')
    [ $json -eq 1 ] && printf '{"schema":1,"tool":"install-linux.sh","package_version":%s,"installs":[' "$(json_str "${version:-unknown}")"
    first=1; any=0
    printf '%s\n' "$list" | { while IFS= read -r g; do
        [ -n "$g" ] || continue
        any=1
        if [ ! -f "$g/$exe_name" ]; then b=; st=no-game; known=false
        else b=$(build_of "$g/$exe_name"); st=$(state_of "$g"); if [ -n "$b" ]; then known=true; else known=false; fi; fi
        reg=$(prefix_reg "$g")
        if [ -z "$reg" ]; then ov=no-prefix; elif override_present "$reg"; then ov=present; else ov=absent; fi
        if [ $json -eq 1 ]; then
            [ $first -eq 1 ] || printf ','
            printf '{"path":%s,"build":%s,"known_build":%s,"state":%s,"chain_dll":%s,"proton_override":%s}' \
                "$(json_str "$g")" "$(json_str "${b:-unknown}")" "$known" "$(json_str "$st")" \
                "$([ -f "$g/dinput8.chain.dll" ] && echo true || echo false)" "$(json_str "$ov")"
        else
            echo "$g: ${b:-unknown build}; co-op: $st; Proton override: $ov"
        fi
        first=0
    done
    [ $json -eq 1 ] && printf ']}\n'
    [ $any -eq 1 ] || [ $json -eq 1 ] || echo "No MDK2 HD found in the Steam libraries; give the game folder."
    }
    if [ -n "$game" ] && [ ! -f "$game/$exe_name" ]; then exit 1; fi
    return 0
}

case "$action" in
    status) do_status ;;
    install) check_game; record="$game/$record_name"; echo "Installing into $game"; do_install ;;
    uninstall) check_game; record="$game/$record_name"; echo "Removing from $game"; do_uninstall ;;
esac
