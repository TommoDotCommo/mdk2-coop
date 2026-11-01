#!/usr/bin/env bash
# dist/install-linux.sh against fixture game folders (no game, no Windows), plus mutants that must fail:
#   no-backup        overwrites an existing dinput8.dll without keeping it
#   unowned-removal  uninstall removes a file whose content is not the one it installed
#   no-record-check  uninstall works without its record (removes files it cannot prove are its own)
set -uo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
tmp=$(mktemp -d)
trap 'rm -rf "${tmp:?}"' EXIT
fails=0

# Run the whole scenario list against the script given as $1; prints one line per failed expectation.
suite() {
    local script=$1 rel game out
    rel=$(mktemp -d "$tmp/rel.XXXXXX"); mkdir -p "$rel/coop"
    printf 'NEW-DLL\n' > "$rel/dinput8.dll"; printf 'NEW-LUA\n' > "$rel/coop/coop.lua"; cp "$script" "$rel/install-linux.sh"
    expect() { [ "$1" = ok ] || echo "  expectation failed: $2"; }
    ok() { "$@" >/dev/null 2>&1 && echo ok || echo no; }
    bad() { "$@" >/dev/null 2>&1 && echo no || echo ok; }
    # 1. fresh install, then uninstall
    game=$(mktemp -d "$tmp/g.XXXXXX"); : > "$game/mdk2hd.exe"; echo keep > "$game/save.dat"
    expect "$(ok sh "$rel/install-linux.sh" install "$game")" "fresh install succeeds"
    expect "$([ "$(cat "$game/dinput8.dll")" = NEW-DLL ] && [ "$(cat "$game/coop/coop.lua")" = NEW-LUA ] && echo ok)" "files copied"
    expect "$(bad sh "$rel/install-linux.sh" install "$game")" "a second install is refused"
    expect "$(ok sh "$rel/install-linux.sh" uninstall "$game")" "uninstall succeeds"
    expect "$([ ! -e "$game/dinput8.dll" ] && [ ! -e "$game/coop/coop.lua" ] && [ ! -e "$game/coop/install-linux.record" ] && [ "$(cat "$game/save.dat")" = keep ] && echo ok)" "uninstall removes exactly the installed files"
    # 2. an existing dinput8.dll is kept and put back
    game=$(mktemp -d "$tmp/g.XXXXXX"); : > "$game/mdk2hd.exe"; printf 'ORIGINAL\n' > "$game/dinput8.dll"
    expect "$(ok sh "$rel/install-linux.sh" install "$game")" "install over another dinput8.dll succeeds"
    expect "$([ "$(cat "$game/dinput8.dll.coop-original" 2>/dev/null)" = ORIGINAL ] && echo ok)" "the existing dinput8.dll is backed up first"
    expect "$(ok sh "$rel/install-linux.sh" uninstall "$game")" "uninstall after a backup succeeds"
    expect "$([ "$(cat "$game/dinput8.dll" 2>/dev/null)" = ORIGINAL ] && [ ! -e "$game/dinput8.dll.coop-original" ] && echo ok)" "the original dinput8.dll comes back"
    # 3. a file changed after install is not ours any more
    game=$(mktemp -d "$tmp/g.XXXXXX"); : > "$game/mdk2hd.exe"
    sh "$rel/install-linux.sh" install "$game" >/dev/null 2>&1
    printf 'USER-EDIT\n' > "$game/dinput8.dll"
    sh "$rel/install-linux.sh" uninstall "$game" >/dev/null 2>&1
    expect "$([ "$(cat "$game/dinput8.dll" 2>/dev/null)" = USER-EDIT ] && echo ok)" "uninstall leaves a file that changed since install"
    expect "$([ ! -e "$game/coop/coop.lua" ] && echo ok)" "the untouched file is still removed"
    # 4. no record: nothing to remove
    game=$(mktemp -d "$tmp/g.XXXXXX"); : > "$game/mdk2hd.exe"; printf 'SOMEONE-ELSE\n' > "$game/dinput8.dll"; mkdir "$game/coop"; printf 'MINE\n' > "$game/coop/coop.lua"
    expect "$(bad sh "$rel/install-linux.sh" uninstall "$game")" "uninstall without a record is refused"
    expect "$([ "$(cat "$game/dinput8.dll")" = SOMEONE-ELSE ] && [ "$(cat "$game/coop/coop.lua")" = MINE ] && echo ok)" "uninstall without a record removes nothing"
    # 5. refusals
    game=$(mktemp -d "$tmp/g.XXXXXX")
    expect "$(bad sh "$rel/install-linux.sh" install "$game")" "a folder without mdk2hd.exe is refused"
    game=$(mktemp -d "$tmp/g.XXXXXX"); : > "$game/mdk2hd.exe"; echo outside > "$tmp/outside.txt"; ln -s "$tmp/outside.txt" "$game/dinput8.dll"
    expect "$(bad sh "$rel/install-linux.sh" install "$game")" "a symbolic-link target is refused"
    expect "$([ "$(cat "$tmp/outside.txt")" = outside ] && echo ok)" "nothing is written through the link"
    mv "$rel/dinput8.dll" "$rel/dinput8.gone"; game=$(mktemp -d "$tmp/g.XXXXXX"); : > "$game/mdk2hd.exe"
    expect "$(bad sh "$rel/install-linux.sh" install "$game")" "a release folder without dinput8.dll is refused"
    expect "$([ ! -e "$game/coop/install-linux.record" ] && echo ok)" "a refused install leaves no record"
}

check() {   # name script expect_fail
    local out
    out=$(suite "$2")
    if [ "$3" = pass ]; then
        if [ -n "$out" ]; then echo "FAIL ($1):"; echo "$out"; fails=$((fails + 1)); fi
    elif [ -z "$out" ]; then echo "FAIL: mutant $1 survived"; fails=$((fails + 1)); fi
}

sh -n "$here/dist/install-linux.sh" || { echo "FAIL: syntax"; exit 1; }
check real "$here/dist/install-linux.sh" pass
mutate() {  # name sed-expression
    local copy="$tmp/mutant-$1.sh"
    sed "$2" "$here/dist/install-linux.sh" > "$copy"
    cmp -s "$copy" "$here/dist/install-linux.sh" && { echo "FAIL: mutant $1 does not change the script"; fails=$((fails + 1)); return; }
    check "$1" "$copy" fail
}
mutate no-backup 's/^            backup_original "\$target"$/            :/'
mutate unowned-removal 's/\[ "\$(hash_of "\$target")" != "\$sum" \]/false/'
mutate no-record-check 's/^    \[ -f "\$record" \] || die .*$/    [ -f "$record" ] || : > "$record"/'
mutate overwrites-link 's/^        refuse_link "\$target"$/        :/'
[ $fails -eq 0 ] && echo "PASS: install-linux.sh install, backup, uninstall and refusals, with 4 mutants" || exit 1
