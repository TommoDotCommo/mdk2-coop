#!/usr/bin/env bash
# Doc's leaf blower against an enemy whose AI is running (level 3 checkpoint 5, the lounge with the plant), blown by
# the client and then by the host. The enemies are the host's; a client's blowing used to move only the client's own
# copies, which follow the host's every frame, so they rocked in place and the host's never moved (tools/qa_items.sh
# held its enemy still, which hid that). The owner stands at x 10 facing +x with the enemy 5 units ahead; the other
# player stands 15 units behind the owner and 12 to the side, out of the blower's reach (it pushes ahead only) but in
# the lounge, so its game keeps its copy of the room awake (a game leaves the copies in rooms nobody there is in).
# For each owner, read from the processes:
#   push          the host's game has the blower's push volume on the owner's item, switched on while blowing and
#                 listed among the scene's push volumes (the client's: the copy the host builds on its puppet; the
#                 host's: the game's own), and off again after
#   blown (host)  the enemy ends at least 5 units further along +x in the host's game, its AI running (the old
#                 relayed positions nudged it about 4 units, then it walked back to where it stood); in the host's
#                 own round, which is the game's unchanged blower, it gets at least 5 units further at some point
#   blown (other) the same in the client's game
#   shared        it ends in the same place in both games (within 3 units)
#   players       last, the client blows at the host's hero standing 5 units ahead: it stays where it is in the
#                 host's game (within 1.5 units; the blower pushes no player, the owner's call)
#   MUTANT=noblowsync  the host builds no push volume on a client's Doc ([test] no_blowsync=1): the client checks must fail
#   MUTANT=blowplayers the host's copy of the client's blower pushes players too ([test] blow_players=1): the players
#                      check must fail
# Needs MDK2_N=2, isolated game copies and MDK2_BACKGROUND (see tools/mdk.sh); about 2 minutes.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N=2 MDK2_BACKGROUND=1 MDK2_START="3 5"
case "${MUTANT:-}" in ''|noblowsync|blowplayers) : ;; *) echo 'Unknown blower mutant' >&2; exit 2 ;; esac
source "$here/mdk.sh"
"$here/install.sh" >/dev/null || exit 1
if [ -n "${MUTANT:-}" ]; then
    key=no_blowsync; [ "$MUTANT" = blowplayers ] && key=blow_players
    for i in 1 2; do printf '%s=1\r\n' "$key" >> "$(mdk_wsl "$(mdk_dir "$i")")/coop/coop.ini"; done      # the file ends in the [test] section
fi

pass=0; fail=0; results=()
out="$here/../local/qa/$(date +%Y%m%d-%H%M%S)-blowerlive"; mkdir -p "$out"
check() { local name="$1"; shift
    if "$@" >"$out/check-$((pass + fail + 1)).txt" 2>&1; then results+=("PASS  $name"); pass=$((pass + 1))
    else results+=("FAIL  $name"); fail=$((fail + 1)); fi
    echo "${results[-1]}"; }
note() { echo "$*" | tee -a "$out/evidence.txt"; }
has() { grep -qaE "$2" "$(mdk_log "$1")"; }
lua() { mdk_cmd "$(mdk_pid "$1")" "$2"; }
q() {   # q <game> <lua value expression>: that game's value, read back from its own log
    local tag="Q$RANDOM$RANDOM" i
    lua "$1" "coop_log(format(\"$tag %s\", tostring($2)))"
    for i in $(seq 1 40); do has "$1" "^lua: $tag " && break; sleep .25; done
    grep -aE "^lua: $tag " "$(mdk_log "$1")" | tail -1 | sed 's/^lua: [A-Z0-9]* //' | tr -d '\r'
}
ge() { python3 -c "import sys; sys.exit(0 if float('$1') >= float('$2') else 1)" 2>/dev/null; }
within() { python3 -c "import sys; sys.exit(0 if abs(float('$1') - float('$2')) <= float('$3') else 1)" 2>/dev/null; }
dist() { python3 -c "import math; print(round(math.dist(($1), ($2)), 2))" 2>/dev/null; }
# agree <dog> <units> <seconds>: the host's enemy and the client's copy of it come within <units> of each other
agree() { local t; for t in $(seq 1 $((${3} * 4))); do ge "$2" "$(dist "$(xy 1 "$1")" "$(xy 2 "$1")")" && return 0; sleep .25; done
    echo "host $(xy 1 "$1"), client $(xy 2 "$1")"; return 1; }
dump() {    # dump <game> <tag>: that game's state dump, copied into the evidence folder
    local path t
    path="$(mdk_wsl "$(mdk_dir "$1")")/coop/state_$2.txt"; rm -f "$path"
    lua "$1" "coop_state(\"$2\")" || return 1
    for t in $(seq 1 150); do [ -f "$path" ] && break; sleep .1; done
    cp "$path" "$out/$2.txt"
}
# the host's dump (tag $1) has player $2's blower push volume in state $3 (-1 none, 0 off, 1 on)
push_is() { [ "$(tr -d '\r' < "$out/$1.txt" | awk -v o="$2" '$1 == "blowerpush" && $2 == o {print $3 " " $4}')" = "$3" ]; }
xy() { q "$1" "format(\"%.3f,%.3f\", $2.position.x, $2.position.y)"; }
at_xyz() { lua "$1" "mdkGobSetPositionXYZ(mdkGetPlayerGob(), $2, $3, $4)"; }
# the game builds the blower when the item is taken up: give the item, then cycle Doc's left hand (scroll G/F, hand key Q) until it exists
take_blower() {
    local g=$1 dir i
    lua "$g" 'mdkDocGiveItem(mdkGetPlayerGob(), OBJ_LEAFBLOWER, 1)'; sleep 1
    for dir in 34 33 34 33 34 33 34 33; do for i in 1 2 3 4; do
        [ "$(q "$g" 'coop_alive(blower)')" = 1 ] && return 0
        lua "$g" "coop_press_frames($dir, 3)"; sleep .6; lua "$g" 'coop_press_frames(16, 3)'; sleep .6
    done; done
    [ "$(q "$g" 'coop_alive(blower)')" = 1 ]
}
trap 'cp "$(mdk_log 1)" "$out/bridge_1.log" 2>/dev/null; cp "$(mdk_log 2)" "$out/bridge_2.log" 2>/dev/null; mdk_kill' EXIT

mdk_boot
check "both games started and each has the other's puppet" bash -c "grep -qa 'puppet spawned for player 2' '$(mdk_log 1)' && grep -qa 'puppet spawned for player 1' '$(mdk_log 2)'"
if [[ "${results[-1]}" == FAIL* ]]; then echo "the games did not start together: nothing to test"; exit 1; fi
check "both games are in level 3 checkpoint 5" bash -c "grep -qa '^level 3 checkpoint 5 started' '$(mdk_log 1)' && grep -qa '^level 3 checkpoint 5 started' '$(mdk_log 2)'"
lua 1 'God(1)'; lua 2 'God(1)'
for g in 1 2; do check "player $g: the leaf blower is taken up (the game's own blower gob exists)" take_blower "$g"; done

# the lounge (room l3_treed00, x -41..66, y 38..66, its tree near x -15): the enemy's script and walker are left
# running; it turns on the players and attacks while it is blown. Each round has an enemy of its own, and the blow
# keeps it in the room: the host's own blower is the stronger one and sent its enemy through the east side (x 66) in
# 200 frames, after which the client's copy stopped short of it (a separate issue: copies in a room no player is in).
for owner in 2 1; do
    other=$((3 - owner))
    if [ "$owner" = 2 ]; then dog=l3_dogan02; frames=200; else dog=l3_dogan00; frames=120; fi
    at_xyz "$other" -5 40 0.6
    lua "$owner" "local d = mdkGetPlayerGob() mdkGobSetPositionXYZ(d, 10, 52, 0.6) coop.Face(d, 1, 0, 0)"
    sleep 1
    lua 1 "mdkGobSetPositionXYZ($dog, 15, 52, 0.4)"
    sleep 2
    o=$(xy "$owner" 'mdkGetPlayerGob()'); p=$(xy "$other" 'mdkGetPlayerGob()')
    note "player $owner: owner at $o, the other player at $p"
    check "player $owner: the other player stands behind the owner, out of the blower's reach (process state)" \
        python3 -c "import sys; o = ($o); p = ($p); sys.exit(0 if p[0] < o[0] - 10 else 1)"
    check "player $owner: before: $dog is alive on the host" ge "$(q 1 "coop_hp($dog)")" 1
    check "player $owner: before: $dog stands in the same place in both games (within 1.5 units)" agree "$dog" 1.5 5
    h0=$(xy 1 "$dog"); c0=$(xy 2 "$dog")
    lua "$owner" "coop_press_frames(256, $frames)"
    hs=(); cs=()
    for i in $(seq 1 10); do sleep .3; hs+=("$(xy 1 "$dog")"); cs+=("$(xy 2 "$dog")"); [ "$i" = 1 ] && dump 1 "push_on_$owner"; done
    note "player $owner: $dog x from $h0 / $c0: host ${hs[*]}; client ${cs[*]}"
    check "player $owner: push: the host's game has the owner's push volume switched on and listed while blowing" push_is "push_on_$owner" "$owner" "1 1"
    sleep 2
    dump 1 "push_off_$owner"
    check "player $owner: push: switched off again after the blow" push_is "push_off_$owner" "$owner" "0 1"
    # the client's round counts where the enemy ends (the old relayed push nudged it and it walked back); the host's own
    # blower is the game's, unchanged, and its enemy sometimes walks back after the blow: that round counts the furthest
    pick=-1; how=ends; [ "$owner" = 1 ] && { pick=max; how="got at most"; }
    hd=$(python3 -c "v=[float(x.split(',')[0]) for x in '${hs[*]}'.split()]; print(round((max(v) if '$pick' == 'max' else v[-1]) - float('$h0'.split(',')[0]), 2))")
    cd=$(python3 -c "v=[float(x.split(',')[0]) for x in '${cs[*]}'.split()]; print(round((max(v) if '$pick' == 'max' else v[-1]) - float('$c0'.split(',')[0]), 2))")
    check "player $owner: blown (host): $dog $how $hd units further along +x in the host's game, its AI running" ge "$hd" 5
    check "player $owner: blown (other): $dog $how $cd units further along +x in the client's game" ge "$cd" 5
    check "player $owner: shared: $dog ends in the same place in both games (within 3 units)" agree "$dog" 3 4
    lua 1 "mdkGobSetPositionXYZ($dog, 45, 63, 0.4)"     # out of the next round's way (an unblown one stood on its spot)
    sleep 3
done
# the client blows at the host's hero 5 units ahead of it (no enemy in between): the hero stays put in the host's game
lua 2 "local d = mdkGetPlayerGob() mdkGobSetPositionXYZ(d, 10, 52, 0.6) coop.Face(d, 1, 0, 0)"
lua 1 "local d = mdkGetPlayerGob() mdkGobSetPositionXYZ(d, 15, 52, 0.6) coop.Face(d, -1, 0, 0)"
sleep 2
p0=$(xy 1 'mdkGetPlayerGob()')
lua 2 "coop_press_frames(256, 200)"
far=0
for i in $(seq 1 8); do sleep .3; d=$(dist "$p0" "$(xy 1 'mdkGetPlayerGob()')"); python3 -c "import sys; sys.exit(0 if $d > $far else 1)" && far=$d; done
note "players: the host's hero from $p0, moved at most $far units while the client blew at it"
check "players: the host's hero stays where it is while the client blows at it (moved $far, at most 1.5)" ge 1.5 "$far"
printf '%s\n' "${results[@]}"
echo "$pass passed, $fail failed"
echo "process evidence: $out"
mdk_crash_report
[ "$fail" -eq 0 ]
