#!/usr/bin/env bash
# dist/install-linux.sh against fixture game folders, a fixture Steam library with a Proton prefix and a fixture /proc
# (no game, no Windows), plus mutants that must fail:
#   no-backup            overwrites an existing dinput8.dll without keeping it
#   unowned-removal      uninstall removes a file whose content is not the one it installed
#   no-record-check      uninstall works without its record (removes files it cannot prove are its own)
#   overwrites-link      writes through a symbolic link
#   no-chain             another mod's dinput8.dll is kept as .coop-original instead of dinput8.chain.dll (never loaded)
#   wrong-exe            the Proton DLL override is written for another exe name
#   override-left        uninstall leaves the Proton DLL override in the prefix
#   no-running-check     installs while the game runs (exit code 4 never given)
#   no-build-check       installs over an unknown game build without being asked to (exit code 3 never given)
set -uo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
tmp=$(mktemp -d)
trap 'rm -rf "${tmp:?}"' EXIT
fails=0
python3 "$here/tools/fixtures/make_build_fixtures.py" "$tmp/builds" >/dev/null || { echo "FAIL: build fixtures"; exit 1; }
mkdir -p "$tmp/noproc"      # a /proc with no game in it (the real one may list anything)
REG0=$'WINE REGISTRY Version 2\n;; All keys relative to \\\\User\n\n[Software\\\\Wine] 1700000000\n#time=1\n"Version"="win10"\n'
KEY='[Software\\Wine\\AppDefaults\\mdk2hd.exe\\DllOverrides]'

# Run the whole scenario list against the script given as $1; prints one line per failed expectation.
suite() {
    local script=$1 rel game out lib reg code
    export INSTALL_LINUX_PROC="$tmp/noproc"
    rel=$(mktemp -d "$tmp/rel.XXXXXX"); mkdir -p "$rel/coop"
    printf 'NEW-DLL mdk2-coop bridge attached\n' > "$rel/dinput8.dll"; printf 'NEW-LUA\n' > "$rel/coop/coop.lua"; cp "$script" "$rel/install-linux.sh"
    printf '{"version": "v9.9.9", "files": [{"path": "dinput8.dll", "sha256": "%s", "size": 1}, {"path": "coop/coop.lua", "sha256": "%s", "size": 1}]}\n' \
        "$(sha256sum "$rel/dinput8.dll" | cut -d' ' -f1)" "$(sha256sum "$rel/coop/coop.lua" | cut -d' ' -f1)" > "$rel/coop-mod.json"
    expect() { [ "$1" = ok ] || echo "  expectation failed: $2"; }
    ok() { "$@" >/dev/null 2>&1 && echo ok || echo no; }
    bad() { "$@" >/dev/null 2>&1 && echo no || echo ok; }
    code() { "$@" >/dev/null 2>&1; echo $?; }
    newgame() { game=$(mktemp -d "$tmp/g.XXXXXX"); cp "$tmp/builds/${1:-steam}.exe" "$game/mdk2hd.exe"; }
    # 1. fresh install, then uninstall
    newgame; echo keep > "$game/save.dat"
    expect "$(ok sh "$rel/install-linux.sh" install "$game")" "fresh install succeeds"
    expect "$([ "$(cat "$game/dinput8.dll")" = "NEW-DLL mdk2-coop bridge attached" ] && [ "$(cat "$game/coop/coop.lua")" = NEW-LUA ] && echo ok)" "files copied"
    expect "$(bad sh "$rel/install-linux.sh" install "$game")" "a second install is refused"
    expect "$(ok sh "$rel/install-linux.sh" uninstall "$game" --yes)" "uninstall succeeds"
    expect "$([ ! -e "$game/dinput8.dll" ] && [ ! -e "$game/coop/coop.lua" ] && [ ! -e "$game/coop/install-linux.record" ] && [ "$(cat "$game/save.dat")" = keep ] && echo ok)" "uninstall removes exactly the installed files"
    # 2. another mod's dinput8.dll is kept as dinput8.chain.dll (the mod loads it) and put back
    newgame overhaul; printf 'ORIGINAL\n' > "$game/dinput8.dll"
    expect "$(ok sh "$rel/install-linux.sh" install "$game" --yes)" "install over another dinput8.dll succeeds"
    expect "$([ "$(cat "$game/dinput8.chain.dll" 2>/dev/null)" = ORIGINAL ] && [ ! -e "$game/dinput8.dll.coop-original" ] && echo ok)" "the other mod's dinput8.dll is kept as dinput8.chain.dll"
    expect "$(ok sh "$rel/install-linux.sh" uninstall "$game")" "uninstall after a chain succeeds"
    expect "$([ "$(cat "$game/dinput8.dll" 2>/dev/null)" = ORIGINAL ] && [ ! -e "$game/dinput8.chain.dll" ] && echo ok)" "the original dinput8.dll comes back"
    # 2b. an older copy of the mod's own DLL is a backup, never a chain
    newgame; printf 'OLD mdk2-coop bridge attached\n' > "$game/dinput8.dll"
    sh "$rel/install-linux.sh" install "$game" >/dev/null 2>&1
    expect "$([ -f "$game/dinput8.dll.coop-original" ] && [ ! -e "$game/dinput8.chain.dll" ] && echo ok)" "an old copy of the mod is kept as .coop-original, not chained"
    # 3. a file changed after install is not ours any more
    newgame
    sh "$rel/install-linux.sh" install "$game" >/dev/null 2>&1
    printf 'USER-EDIT\n' > "$game/dinput8.dll"
    sh "$rel/install-linux.sh" uninstall "$game" >/dev/null 2>&1
    expect "$([ "$(cat "$game/dinput8.dll" 2>/dev/null)" = USER-EDIT ] && echo ok)" "uninstall leaves a file that changed since install"
    expect "$([ ! -e "$game/coop/coop.lua" ] && echo ok)" "the untouched file is still removed"
    # 4. no record: nothing to remove
    newgame; printf 'SOMEONE-ELSE\n' > "$game/dinput8.dll"; mkdir "$game/coop"; printf 'MINE\n' > "$game/coop/coop.lua"
    expect "$(bad sh "$rel/install-linux.sh" uninstall "$game")" "uninstall without a record is refused"
    expect "$([ "$(cat "$game/dinput8.dll")" = SOMEONE-ELSE ] && [ "$(cat "$game/coop/coop.lua")" = MINE ] && echo ok)" "uninstall without a record removes nothing"
    # 5. refusals and exit codes
    game=$(mktemp -d "$tmp/g.XXXXXX")
    expect "$([ "$(code sh "$rel/install-linux.sh" install "$game")" = 1 ] && echo ok)" "a folder without mdk2hd.exe: exit 1"
    expect "$([ "$(code sh "$rel/install-linux.sh" install)" = 2 ] && [ "$(code sh "$rel/install-linux.sh" frobnicate "$game")" = 2 ] && [ "$(code sh "$rel/install-linux.sh" install "$game" --what)" = 2 ] && echo ok)" "usage errors: exit 2"
    newgame unknown-pointer
    expect "$([ "$(code sh "$rel/install-linux.sh" install "$game" --yes)" = 3 ] && [ ! -e "$game/dinput8.dll" ] && [ ! -e "$game/coop" ] && echo ok)" "an unknown game build: exit 3, nothing written"
    expect "$([ "$(code sh "$rel/install-linux.sh" install "$game" --yes --accept-unknown-build)" = 0 ] && echo ok)" "--accept-unknown-build installs anyway"
    newgame; mkdir -p "$tmp/proc-run/4242"; printf 'mdk2hd.exe\n' > "$tmp/proc-run/4242/comm"; printf 'Z:\\games\\MDK 2 HD\\mdk2hd.exe\0' > "$tmp/proc-run/4242/cmdline"
    expect "$([ "$(INSTALL_LINUX_PROC="$tmp/proc-run" code sh "$rel/install-linux.sh" install "$game" --yes)" = 4 ] && [ ! -e "$game/dinput8.dll" ] && echo ok)" "the game running: install exits 4 and writes nothing"
    sh "$rel/install-linux.sh" install "$game" >/dev/null 2>&1
    expect "$([ "$(INSTALL_LINUX_PROC="$tmp/proc-run" code sh "$rel/install-linux.sh" uninstall "$game" --yes)" = 4 ] && [ -e "$game/dinput8.dll" ] && echo ok)" "the game running: uninstall exits 4 and keeps the files"
    newgame; echo outside > "$tmp/outside.txt"; ln -sf "$tmp/outside.txt" "$game/dinput8.dll"
    expect "$(bad sh "$rel/install-linux.sh" install "$game")" "a symbolic-link target is refused"
    expect "$([ "$(cat "$tmp/outside.txt")" = outside ] && echo ok)" "nothing is written through the link"
    # 6. status --json: who installed it
    newgame; s=$(sh "$rel/install-linux.sh" status --json "$game")
    expect "$(printf '%s' "$s" | python3 -c 'import json,sys; d=json.load(sys.stdin); i=d["installs"][0]; assert i["state"]=="not-installed" and i["known_build"] is True and i["build"]=="Steam build 3902" and d["package_version"]=="v9.9.9"' 2>/dev/null && echo ok)" "status --json: not installed, known build"
    sh "$rel/install-linux.sh" install "$game" >/dev/null 2>&1
    expect "$(sh "$rel/install-linux.sh" status --json "$game" | python3 -c 'import json,sys; assert json.load(sys.stdin)["installs"][0]["state"]=="installed"' 2>/dev/null && echo ok)" "status --json: installed by this script"
    newgame; mkdir -p "$game/coop"; cp "$rel/dinput8.dll" "$game/"; cp "$rel/coop/coop.lua" "$game/coop/"
    expect "$(sh "$rel/install-linux.sh" status --json "$game" | python3 -c 'import json,sys; assert json.load(sys.stdin)["installs"][0]["state"]=="mod-manager"' 2>/dev/null && echo ok)" "status --json: a plain copy of the release is a mod manager's"
    newgame; mkdir -p "$game/coop"; ln "$rel/dinput8.dll" "$game/dinput8.dll"; ln "$rel/coop/coop.lua" "$game/coop/coop.lua"
    expect "$(sh "$rel/install-linux.sh" status --json "$game" | python3 -c 'import json,sys; assert json.load(sys.stdin)["installs"][0]["state"]=="vortex"' 2>/dev/null && echo ok)" "status --json: hard-linked files are Vortex's"
    expect "$([ "$(code sh "$rel/install-linux.sh" status --json "$tmp/nowhere")" = 1 ] && echo ok)" "status of a folder without the game: exit 1"
    # 7. the Proton prefix: the DLL override for mdk2hd.exe, undone exactly
    lib=$(mktemp -d "$tmp/lib.XXXXXX"); mkdir -p "$lib/steamapps/common/MDK 2 HD" "$lib/steamapps/compatdata/213350/pfx"
    game="$lib/steamapps/common/MDK 2 HD"; cp "$tmp/builds/steam.exe" "$game/mdk2hd.exe"; reg="$lib/steamapps/compatdata/213350/pfx/user.reg"
    printf '%s' "$REG0" > "$reg"
    expect "$(ok sh "$rel/install-linux.sh" install "$game" --yes)" "install with a Proton prefix succeeds"
    expect "$(K="$KEY" awk 'BEGIN { k = ENVIRON["K"] } index($0, k) == 1 { s = 1; next } /^\[/ { s = 0 } s && $0 == "\"dinput8\"=\"native,builtin\"" { f = 1 } END { exit !f }' "$reg" && echo ok)" "user.reg holds dinput8=native,builtin under mdk2hd.exe's DllOverrides"
    expect "$(sh "$rel/install-linux.sh" status --json "$game" | python3 -c 'import json,sys; assert json.load(sys.stdin)["installs"][0]["proton_override"]=="present"' 2>/dev/null && echo ok)" "status --json reports the override"
    expect "$(ok sh "$rel/install-linux.sh" uninstall "$game" --yes)" "uninstall with a Proton prefix succeeds"
    expect "$([ "$(cat "$reg")" = "$(printf '%s' "$REG0")" ] && [ ! -e "$reg.mdk2-coop-backup" ] && echo ok)" "uninstall restores user.reg byte for byte"
    # 7b. Wine rewrote user.reg after install: uninstall takes out only the mod's line; an old dinput8 value comes back
    printf '%s\n%s 1\n"dinput8"="builtin"\n' "$REG0" "$KEY" > "$reg"
    sh "$rel/install-linux.sh" install "$game" >/dev/null 2>&1
    printf '\n[Software\\\\Wine\\\\Other] 1\n"x"="y"\n' >> "$reg"
    sh "$rel/install-linux.sh" uninstall "$game" >/dev/null 2>&1
    expect "$(grep -q '"dinput8"="builtin"' "$reg" && ! grep -q 'native,builtin' "$reg" && grep -q '"x"="y"' "$reg" && echo ok)" "after Wine changed user.reg, uninstall removes only the mod's override"
    # 7c. no prefix: the launch option is printed instead
    lib=$(mktemp -d "$tmp/lib.XXXXXX"); mkdir -p "$lib/steamapps/common/MDK 2 HD"; game="$lib/steamapps/common/MDK 2 HD"; cp "$tmp/builds/steam.exe" "$game/mdk2hd.exe"
    expect "$(sh "$rel/install-linux.sh" install "$game" 2>&1 | grep -q 'WINEDLLOVERRIDES="dinput8=n,b" %command%' && echo ok)" "without a prefix, install prints the launch option"
    mv "$rel/dinput8.dll" "$rel/dinput8.gone"; newgame
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
mutate() {  # name old-text new-text (literal)
    local copy="$tmp/mutant-$1.sh"
    python3 -c 'import sys; s = open(sys.argv[1]).read(); assert s.count(sys.argv[2]) == 1, sys.argv[2]; open(sys.argv[3], "w").write(s.replace(sys.argv[2], sys.argv[4]))' \
        "$here/dist/install-linux.sh" "$2" "$copy" "$3" || { echo "FAIL: mutant $1 does not apply"; fails=$((fails + 1)); return; }
    check "$1" "$copy" fail
}
mutate no-backup '        had=$(keep_original "$target")' '        had=new'
mutate unowned-removal '[ "$(hash_of "$target")" != "$sum" ]' 'false'
mutate no-record-check '    [ -f "$record" ] || die "no record' '    [ -f "$record" ] || : > "$record"; : die "no record'
mutate overwrites-link '        refuse_link "$target"' '        :'
mutate no-chain 'if [ "$(basename "$1")" = dinput8.dll ] && ! is_ours "$1"; then' 'if false; then'
mutate wrong-exe 'AppDefaults\\\\$exe_name\\\\DllOverrides' 'AppDefaults\\\\mdk2.exe\\\\DllOverrides'
mutate override-left '    undo_override "$game"' '    :'
mutate no-running-check '    if game_running; then' '    if false; then'
mutate no-build-check '        [ $accept_unknown -eq 1 ] || exit 3' '        :'
[ $fails -eq 0 ] && echo "PASS: install-linux.sh verbs and exit codes, chain, backup, uninstall, status --json, Proton override, with 9 mutants" || exit 1
