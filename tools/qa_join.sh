#!/usr/bin/env bash
# Client starts at title, waits without choosing New Game, follows host, handles
# host loss, cancellation and retry. Two disposable copies, no desktop input.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N=2
source "$here/mdk.sh"
scenario=join
source "$here/presence_common.sh"
# No autostart: the host deliberately remains at the title screen.
MDK2_BACKGROUND=1 MDK2_START='' "$here/install.sh" >/dev/null
mdk_kill
for i in 1 2; do pid=$(mdk_start "$i"); echo "$pid" >> "$MDK2_PIDS"; sleep 2; done
sleep 5
case "${MUTANT:-}" in
    join) lua 2 'coop.JoinScreenTick=function() end coop.join_state=nil coop_join_screen(0)' ;;
    '') : ;;
    *) echo 'Unknown join mutant' >&2; exit 2 ;;
esac
capture waiting
check 'client waits on its own screen at title' python3 "$here/presence_check.py" join --expect 1 "$out/waiting_2.txt"
lua 1 'mdkNewGame(1,2)'
sleep 15
capture pulled
check 'host pulls client into play automatically' python3 "$here/presence_check.py" join --expect 3 "$out/pulled_2.txt"
lua 1 'mdkNewGame(0,0)'
sleep 5
# Client keeps its current level on host exit; existing reconnect waiting UI
# handles the level, while the portable join state records host readiness.
capture hostleft
check 'client notices host leaving the level' python3 -c 'import sys; from pathlib import Path; assert "join 1" in Path(sys.argv[1]).read_text().splitlines()' "$out/hostleft_2.txt"
lua 2 'mdkNewGame(0,0)'
sleep 4
lua 2 'coop.CancelJoin()'
sleep 1
capture cancelled
check 'cancel stops joining' python3 "$here/presence_check.py" join --expect 7 "$out/cancelled_2.txt"
lua 2 'coop.join_cancelled=nil coop.join_state=nil coop_join_retry()'
sleep 3
capture retry
check 'retry returns to waiting screen' python3 "$here/presence_check.py" join --expect 1 "$out/retry_2.txt"
# A real network outage at the title must show an actionable timeout.
lua 2 'coop_netblock(65000)'
for t in $(seq 1 63); do sleep 1; done
capture timeout
check 'network outage times out with a visible waiting screen' python3 "$here/presence_check.py" join --expect 5 "$out/timeout_2.txt"
sleep 4
# Send a real packet with the prior protocol magic from the configured host.
# The client must reject it before decoding its world or events.
lua 1 'coop_presence_test(0,0,0,0,1)'
sleep 2
capture mismatch
check 'incompatible host protocol gives a visible update message' python3 "$here/presence_check.py" join --expect 6 "$out/mismatch_2.txt"
lua 1 'coop_presence_test(0,0,0,0,0)'
summary
