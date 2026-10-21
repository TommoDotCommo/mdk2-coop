#!/usr/bin/env bash
# Measure the host's upload: MDK2_N games (default 8) in background test mode at a checkpoint
# (default 2-1, 71 enemies), 30 s with everyone standing, then 30 s with everyone walking, turning
# and firing. Prints the host's "net out:" lines (netstats.c, every 10 s) for each phase and the
# average, plus the busiest client's upload.
#   MDK2_N=8 tools/netmeasure.sh [level checkpoint]
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N="${MDK2_N:-8}"
source "$here/mdk.sh"
lvl="${1:-2}"; cp="${2:-1}"
MDK2_BACKGROUND=1 MDK2_START="$lvl $cp" "$here/install.sh" >/dev/null
mdk_boot
H=$(mdk_log 1)
mark() { wc -l < "$H"; }
avg() {   # avg <from line>: mean of the total and per-kind kbit/s over the net out lines since then
    tail -n +"$1" "$H" | grep '^net out:' | tail -n +2 |
        sed -E 's/^net out: ([0-9]+) kbit.* player ([0-9]+) .* relay ([0-9]+) .* ent ([0-9]+) .* other ([0-9]+) .*/\1 \2 \3 \4 \5/' |
        awk '{t+=$1; p+=$2; r+=$3; e+=$4; o+=$5; n++} END {if (n) printf "total %.0f kbit/s (player %.0f, relay %.0f, ent %.0f, other %.0f) over %d samples\n", t/n, p/n, r/n, e/n, o/n, n; else print "no samples"}'
}

sleep 5
a=$(mark); sleep 32
echo "standing ($MDK2_N games, level $lvl-$cp): $(avg "$a")"
b=$(mark)
end=$(( $(date +%s) + 32 ))
while [ "$(date +%s)" -lt "$end" ]; do
    for i in $(mdk_seq); do
        p=$(mdk_pid "$i")
        mdk_cmd "$p" "coop_press($(( (RANDOM % 2) ? 17 : 31 )), 900)" >/dev/null 2>&1
        mdk_cmd "$p" "coop_press($(( (RANDOM % 2) ? 30 : 32 )), 400)" >/dev/null 2>&1
        mdk_cmd "$p" "coop_press(256, 300)" >/dev/null 2>&1
    done
    sleep 1
done
echo "moving   ($MDK2_N games, level $lvl-$cp): $(avg "$b")"
for i in $(seq 2 "$MDK2_N"); do
    grep '^net out:' "$(mdk_log "$i")" | tail -3 | sed -E 's/^net out: ([0-9]+) kbit.*/\1/' | awk -v i="$i" '{t+=$1;n++} END {if (n) printf "%.0f client%d\n", t/n, i}'
done | sort -n | tail -1 | awk '{print "busiest client upload: " $1 " kbit/s (" $2 ")"}'
grep -c 'LUA ERROR' "$H" | awk '{if ($1) print "host lua errors: " $1}'
mdk_kill
