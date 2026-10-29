#!/usr/bin/env bash
# Per-instance user data (bridge/redirect.c): two game copies with different redirects each make a
# co-op save; the real %LOCALAPPDATA%\MDK2HD must be byte-identical afterwards, and each copy's own
# folder must hold its own files and nothing of the other's.
#   tools/qa_redirect.sh                   the check (MDK2_N is 2)
#   MUTANT=shared tools/qa_redirect.sh     both copies pointed at one folder: must FAIL
# The real folder is only read (hashed before and after). Switching the redirect off cannot be
# run as a live mutant without the game writing into it; bridge/tests/redirect_test.c covers "off".
# Other sessions that run without the redirect write the real folder at the same time, which makes the
# byte-identical check meaningless; MDK2_REAL_SHARED=1 then only requires that none of the files these
# two games wrote (same name, same bytes) appears in the real folder.
# Game copies: see tools/mdk.sh. About 2 minutes.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N=2
source "$here/mdk.sh"
unset MDK2_SAVE_ISOLATE      # this suite is about the default
real="$(mdk_real_profile)"
real_hash() {   # one hash over the paths and contents of everything the game keeps there
    ( cd "$real" 2>/dev/null && find save crash settings.lua -type f -print0 2>/dev/null | sort -z |
        xargs -0 -r sha256sum | sha256sum | cut -d' ' -f1 ) }
before=$(real_hash)
start=$(date +%s)
crash_before=$(mdk_crash_dumps | wc -l)

seed=$(mktemp -d); mkdir -p "$seed/save"; echo seed > "$seed/save/seed.txt"
MDK2_SAVE_SEED="$seed" MDK2_BACKGROUND=1 MDK2_START="1 2" MDK2_SAVE_INTERVAL=600 "$here/install.sh" >/dev/null
# [test] save_dir of each copy, as an absolute Windows path
save_dir_of() { local d; d="$(mdk_dir "$1")"; echo "$d\\profile"; }
if [ "${MUTANT:-}" = shared ]; then    # copy 2 uses copy 1's folder
    ini="$(mdk_wsl "$(mdk_dir 2)")/coop/coop.ini"
    sed -i "s|^save_dir=.*|save_dir=$(save_dir_of 1 | sed 's/\\/\\\\/g')|" "$ini"
fi
profile_of() {   # WSL path of the MDK2HD folder copy $1 will use (from its coop.ini)
    local v; v=$(tr -d '\r' < "$(mdk_wsl "$(mdk_dir "$1")")/coop/coop.ini" | sed -n 's/^save_dir=//p')
    case "$v" in ?:*) echo "$(mdk_wsl "$v")/MDK2HD";; *) echo "$(mdk_wsl "$(mdk_dir "$1")")/$v/MDK2HD";; esac; }
for i in $(mdk_seq); do mkdir -p "$(profile_of "$i")/save"; echo "$i" > "$(profile_of "$i")/save/own-$i.marker"; done

pass=0; fail=0; results=()
out="$here/../local/qa/$(date +%Y%m%d-%H%M%S)-redirect${MUTANT:+-$MUTANT}"; mkdir -p "$out"
check() { local name="$1"; shift
    if "$@" >/dev/null 2>&1; then results+=("PASS  $name"); pass=$((pass + 1))
    else results+=("FAIL  $name"); fail=$((fail + 1)); fi; }
all_alive() { local p; for p in $(mdk_pids); do mdk_alive "$p" || return 1; done; }
has() { grep -qE "$2" "$(mdk_log "$1")"; }
saved() { has "$1" '^co-op save [0-9]+ written'; }
both() { local k i; for k in $(seq 1 "$2"); do local ok=1; for i in 1 2; do "$1" "$i" || ok=0; done; [ $ok = 1 ] && return 0; sleep 1; done; return 1; }
only_own() {    # copy $1's save folder holds its marker and the other's is absent
    local p other=$((3 - $1)); p="$(profile_of "$1")/save"
    [ -f "$p/own-$1.marker" ] && [ ! -e "$p/own-$other.marker" ] && [ -f "$p/seed.txt" ]; }
has_sav() { ls "$(profile_of "$1")"/save/c*.sav >/dev/null 2>&1; }
redirected() { has "$1" '^redirect: user data folder of the game is'; }
loader_read() { has "$1" '^loader: line present in settings.lua|^loader: '; }
real_untouched() {
    if [ "${MDK2_REAL_SHARED:-0}" != 1 ]; then [ "$(real_hash)" = "$before" ]; return; fi
    local i f n; for i in $(mdk_seq); do for f in "$(profile_of "$i")"/save/c*.sav; do
        n="$real/save/$(basename "$f")"
        [ ! -f "$n" ] || [ "$(sha256sum < "$n")" != "$(sha256sum < "$f")" ] || return 1
    done; done; }
distinct() { [ "$(profile_of 1)" != "$(profile_of 2)" ]; }

mdk_boot
check "both games running"                              all_alive
check "game 1 logged the redirect"                      redirected 1
check "game 2 logged the redirect"                      redirected 2
check "both copies made a co-op save"                   both saved 40
sleep 3
check "the copies use different folders"                distinct
check "game 1 wrote its save into its own folder"       has_sav 1
check "game 2 wrote its save into its own folder"       has_sav 2
check "folder 1 holds its own files only"               only_own 1
check "folder 2 holds its own files only"               only_own 2
check "the real save/crash/settings are untouched${MDK2_REAL_SHARED:+ (shared mode)}" real_untouched
check "no new crash dump anywhere"                      bash -c "[ $(mdk_crash_dumps | wc -l) = $crash_before ]"
check "all games still running"                         all_alive
for i in $(mdk_seq); do cp "$(mdk_log "$i")" "$out/game$i.log"; ls -lR "$(dirname "$(profile_of "$i")")" > "$out/profile$i.txt" 2>&1; done
mdk_kill
rm -rf "$seed"

printf '%s\n' "${results[@]}"
echo "logs and folder listings: $out"
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
