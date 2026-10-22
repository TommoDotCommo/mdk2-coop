#!/usr/bin/env bash
# The character wheel with a partner (2 games, level 1-2): the host switches hero and the client's puppet
# for them becomes that hero (the puppet is replaced), and again when the host goes back.
#   MDK2_N=2 tools/qa_char2.sh
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N="${MDK2_N:-2}"
source "$here/mdk.sh"
MDK2_BACKGROUND=1 MDK2_START="1 2" "$here/install.sh" >/dev/null

pass=0; fail=0; results=()
check() { local name="$1"; shift
    if "$@" >/dev/null 2>&1; then results+=("PASS  $name"); pass=$((pass + 1)); else results+=("FAIL  $name"); fail=$((fail + 1)); fi; }
lua() { mdk_cmd "$(mdk_pid "$1")" "$2"; }
# ask <instance> <lua expression>: the value, logged there
ask() { local tag="ask$RANDOM"; lua "$1" "coop_log(\"$tag \" .. tostring($2))"; sleep 1
        grep -E "^lua: $tag " "$(mdk_log "$1")" | tail -1 | tr -d '\r' | sed "s/^lua: $tag //"; }
puppet_type() { ask 2 'mdkGetGobType(coop.puppets[0])'; }

mdk_boot
sleep 3
check "the client sees a puppet of the host's hero"     test "$(puppet_type)" = "$(ask 1 'mdkGetGobType(mdkGetPlayerGob())')"
lua 1 'coop_press(36, 2500)'; sleep 0.6; lua 1 'coop_mouse(80, 0)'; sleep 3.5
check "the host is Max"                                 test "$(ask 1 'mdkGetGobType(mdkGetPlayerGob())')" = "$(ask 1 OBJ_MAX)"
sleep 1
check "the client's puppet for the host is Max"         test "$(puppet_type)" = "$(ask 1 OBJ_MAX)"
lua 1 'coop_press(36, 2500)'; sleep 0.6; lua 1 'coop_mouse(0, 80)'; sleep 3.5
sleep 1
check "and Doc"                                         test "$(puppet_type)" = "$(ask 1 OBJ_DOC)"
check "puppet replaced in the log"                      grep -q "changed character: puppet replaced" "$(mdk_log 2)"
check "both games running"                              bash -c "$(declare -f mdk_alive); mdk_alive $(mdk_pid 1) && mdk_alive $(mdk_pid 2)"
check "no Lua errors"                                   bash -c "! grep -q 'LUA ERROR' '$(mdk_log 1)' '$(mdk_log 2)'"
mdk_kill
printf '%s\n' "${results[@]}"
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
