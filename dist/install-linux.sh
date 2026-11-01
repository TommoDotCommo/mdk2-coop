#!/bin/sh
# mdk2-coop manual install for Linux and the Steam Deck (the game runs through Proton). POSIX sh, coreutils only.
#   sh install-linux.sh install   <MDK2 HD game folder>     copy the two mod files in, keeping what was there
#   sh install-linux.sh uninstall <MDK2 HD game folder>     take out exactly what install put there, put back the originals
# The game folder is the one that holds mdk2hd.exe, e.g. ~/.local/share/Steam/steamapps/common/"MDK 2 HD".
# Run it from the unpacked release folder (it reads dinput8.dll and coop/coop.lua beside itself).
# Two files change: <game folder>/dinput8.dll and <game folder>/coop/coop.lua. A file that was there before is kept as
# <file>.coop-original. A record of what was installed lives in <game folder>/coop/install-linux.record; uninstall removes
# only files whose content still matches that record, and refuses to do anything without it.
# After installing, set the Steam launch option for MDK2 HD (Properties > General > Launch Options):
#   WINEDLLOVERRIDES="dinput8=n,b" %command%
# Proton has its own built-in dinput8; without that line it is used instead of the mod's and co-op stays off.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
record_name=coop/install-linux.record
files="dinput8.dll coop/coop.lua"

die() { echo "install-linux: $*" >&2; exit 1; }
hash_of() { sha256sum "$1" | cut -d' ' -f1; }
usage() { echo "usage: sh install-linux.sh install|uninstall <game folder with mdk2hd.exe>" >&2; exit 2; }

[ $# -eq 2 ] || usage
action=$1
game=$2
case "$action" in install|uninstall) ;; *) usage ;; esac
[ -d "$game" ] || die "not a folder: $game"
[ -f "$game/mdk2hd.exe" ] || die "no mdk2hd.exe in $game: give the MDK2 HD game folder"
record="$game/$record_name"

# never write through a link that points somewhere else
refuse_link() { [ ! -L "$1" ] || die "$1 is a symbolic link; not touching it"; }

backup_original() {     # $1 = target; keeps a file that is already there, never replaces an earlier backup
    [ -e "$1" ] || return 0
    [ ! -e "$1.coop-original" ] || die "$1.coop-original already exists; resolve it by hand first"
    cp -p "$1" "$1.coop-original" || die "could not back up $1"
    [ "$(hash_of "$1")" = "$(hash_of "$1.coop-original")" ] || die "backup of $1 did not verify"
    echo "  kept the existing $(basename "$1") as $(basename "$1").coop-original"
}

do_install() {
    for f in $files; do [ -f "$here/$f" ] || die "$here/$f is missing: run this from the unpacked release folder"; done
    [ ! -e "$record" ] || die "already installed here (see $record). Uninstall first to reinstall."
    mkdir -p "$game/coop"
    : > "$record" || die "cannot write in $game/coop"      # filled one file at a time: an interrupted install can still be undone
    for f in $files; do
        target="$game/$f"
        refuse_link "$target"
        had=new
        if [ -e "$target" ]; then
            backup_original "$target"
            had=backup
        fi
        tmp="$target.coop-new"
        cp "$here/$f" "$tmp" || die "could not copy $f"
        mv -f "$tmp" "$target" || die "could not place $f"
        echo "$(hash_of "$target") $had $f" >> "$record"
        echo "  installed $f"
    done
    echo "Done. Now set this launch option for MDK2 HD in Steam (Properties > General > Launch Options):"
    echo '  WINEDLLOVERRIDES="dinput8=n,b" %command%'
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
        if [ "$had" = backup ] && [ -f "$target.coop-original" ] && [ ! -e "$target" ]; then
            mv "$target.coop-original" "$target"
            echo "  put back the original $f"
        fi
    done < "$record"
    rm -f "$record"
    echo "Done. Remove the launch option from Steam too. The coop folder (settings, logs, saves) was kept."
}

case "$action" in install) echo "Installing into $game"; do_install ;; uninstall) echo "Removing from $game"; do_uninstall ;; esac
