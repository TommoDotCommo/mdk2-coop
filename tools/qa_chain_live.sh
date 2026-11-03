#!/usr/bin/env bash
# dinput8.chain.dll in the real game (co-op mod standard J3): one background game, three starts.
#   none     no chain file: the game process has no dinput8.chain.dll module
#   plugin   a chain DLL without DirectInput8Create: loaded into the game process (its module list says so)
#   proxy    a chain DLL that is a full dinput8 proxy: loaded, and the game's DirectInput8Create went through it (the fixture
#            counts its calls into chain-hits.txt beside itself)
# Asserted from the game process (its module list, read with 32-bit PowerShell) and the fixture's file; the bridge log
# is only kept as evidence. A crash dump or crash dialog fails the run (tools/mdk.sh crash guard).
#   MUTANT=ignored  installs a DLL built with -DCOOP_MUTANT_NO_CHAIN (the chain file is never loaded): this script must
#                   then FAIL.
#   MDK2_N=1 tools/qa_chain_live.sh
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N=1
source "$here/mdk.sh"
if [ "${MUTANT:-}" = ignored ]; then
    make -s -B -C "$here/../bridge" COOP_MUTANT_CFLAGS=-DCOOP_MUTANT_NO_CHAIN >/dev/null || exit 1
fi
MDK2_BACKGROUND=1 "$here/install.sh" >/dev/null || exit 1
[ "${MUTANT:-}" = ignored ] && make -s -B -C "$here/../bridge" >/dev/null    # leave a normal DLL in the tree
d="$(mdk_wsl "$(mdk_dir 1)")"
out="$here/../local/qa/$(date +%Y%m%d-%H%M%S)-$$-chain-live"; mkdir -p "$out"
cleanup() { mdk_kill; sleep 1; rm -f "$d/dinput8.chain.dll" "$d/chain-hits.txt"; }
trap cleanup EXIT
pass=0; fail=0
check() { local name="$1"; shift
    if "$@" >/dev/null 2>&1; then echo "PASS  $name"; pass=$((pass + 1)); else echo "FAIL  $name"; fail=$((fail + 1)); fi; }
i686-w64-mingw32-gcc -O2 -shared -static-libgcc -Wl,--kill-at -o "$out/plugin.dll" "$here/../bridge/tests/chain_fixture.c" || exit 1
i686-w64-mingw32-gcc -O2 -shared -static-libgcc -Wl,--kill-at -DFULL_PROXY -DHIT_FILE -o "$out/proxy.dll" \
    "$here/../bridge/tests/chain_fixture.c" || exit 1
ps32=/mnt/c/Windows/SysWOW64/WindowsPowerShell/v1.0/powershell.exe     # lists a 32-bit game's modules
modules() { "$ps32" -NoProfile -Command "(Get-Process -Id $1).Modules | ForEach-Object { \$_.ModuleName }" 2>/dev/null | tr -d '\r'; }
run() {     # case: start with the chain file in place, wait for the game to ask for DirectInput, read its modules
    local case=$1 pid i
    mdk_kill; sleep 2
    rm -f "$d/dinput8.chain.dll" "$d/chain-hits.txt" "$d/coop/bridge.log"
    [ "$case" = none ] || cp "$out/$case.dll" "$d/dinput8.chain.dll"
    pid=$(mdk_start 1) || return 1
    echo "$pid" > "$MDK2_PIDS"
    for i in $(seq 1 60); do grep -q 'launcher: shown\|lua hook installed' "$d/coop/bridge.log" 2>/dev/null && break; sleep 1; done
    sleep 3
    modules "$pid" > "$out/$case.modules.txt"
    cp "$d/coop/bridge.log" "$out/$case.bridge.log" 2>/dev/null
    [ -f "$d/chain-hits.txt" ] && cp "$d/chain-hits.txt" "$out/$case.hits.txt"
    mdk_alive "$pid"
}
check "none: the game starts"                               run none
check "none: no chained module in the game"                 bash -c "! grep -qix 'dinput8.chain.dll' '$out/none.modules.txt'"
check "plugin: the game starts"                             run plugin
check "plugin: dinput8.chain.dll is loaded into the game"   grep -qix 'dinput8.chain.dll' "$out/plugin.modules.txt"
check "plugin: the mod itself is loaded too"                grep -qix 'dinput8.dll' "$out/plugin.modules.txt"
check "proxy: the game starts"                              run proxy
check "proxy: dinput8.chain.dll is loaded into the game"    grep -qix 'dinput8.chain.dll' "$out/proxy.modules.txt"
check "proxy: the game's DirectInput8Create went through it" bash -c "[ \"\$(cat '$out/proxy.hits.txt' 2>/dev/null)\" -ge 1 ]"
mdk_kill; sleep 1
mdk_crash_report || fail=$((fail + 1))
echo "evidence: $out"
echo "$pass passed, $fail failed"
[ "$fail" = 0 ]
