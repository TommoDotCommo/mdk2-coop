#!/usr/bin/env bash
# The three movement and utility items, used by the host and then by the client, with the other player far away
# (docs/TESTING.md): Kurt's parachute (the canopy), Max's jetpack, Doc's leaf blower. For each owner:
#   use           the item is there and the game's own input uses it (held jump; Doc's fire button with the blower taken up)
#   local effect  read from the owner's own game: a glide far slower than a fall; a lift; the enemy blown away
#   seen by other the other game's copy of the owner: its canopy state and descent; its flight pose and height
#   shared effect the world matches in both games: the blown enemy is in the same place in the host's and the client's game
#                 (the client's blower used to move only its own copy; the host now pushes for it, tools/qa_blower_live.sh)
# Everything is read back from the game processes (Lua values through each game's own log, the state dumps).
#   ONLY=chute|jet|blower   one scenario only (default: all three; each reloads its own level)
#   MUTANT=noblowsync       the host builds no push on a client's Doc ([test] no_blowsync=1): the blower scenario must fail
# Needs MDK2_N=2, isolated game copies and MDK2_BACKGROUND (see tools/mdk.sh); about 6 minutes.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N=2 MDK2_BACKGROUND=1
case "${ONLY:-}" in jet|chute|blower|'') : ;; *) echo 'ONLY is chute, jet or blower' >&2; exit 2 ;; esac
case "${MUTANT:-}" in ''|noblowsync) : ;; *) echo 'Unknown items mutant' >&2; exit 2 ;; esac
first='2 8'; [ "${ONLY:-}" = chute ] && first='1 5'; [ "${ONLY:-}" = blower ] && first='3 6'
export MDK2_START="$first"
source "$here/mdk.sh"
"$here/install.sh" >/dev/null || exit 1
if [ "${MUTANT:-}" = noblowsync ]; then
    for i in 1 2; do printf 'no_blowsync=1\r\n' >> "$(mdk_wsl "$(mdk_dir "$i")")/coop/coop.ini"; done      # the file ends in the [test] section
fi

pass=0; fail=0; results=()
out="$here/../local/qa/$(date +%Y%m%d-%H%M%S)-items"; mkdir -p "$out"
check() { local name="$1"; shift
    if "$@" >"$out/check-$((pass + fail + 1)).txt" 2>&1; then results+=("PASS  $name"); pass=$((pass + 1))
    else results+=("FAIL  $name"); fail=$((fail + 1)); fi
    echo "${results[-1]}"; }
note() { echo "$*" | tee -a "$out/evidence.txt"; }
want() { [ -z "${ONLY:-}" ] || [ "$ONLY" = "$1" ]; }
has() { grep -qaE "$2" "$(mdk_log "$1")"; }
count() { grep -acE "$2" "$(mdk_log "$1")"; }
lua() { mdk_cmd "$(mdk_pid "$1")" "$2"; }
q() {   # q <game> <lua value expression>: that game's value, read back from its own log
    local tag="Q$RANDOM$RANDOM" i
    lua "$1" "coop_log(format(\"$tag %s\", tostring($2)))"
    for i in $(seq 1 40); do has "$1" "^lua: $tag " && break; sleep .25; done
    grep -aE "^lua: $tag " "$(mdk_log "$1")" | tail -1 | sed 's/^lua: [A-Z0-9]* //' | tr -d '\r'
}
wait_q() { local t; for t in $(seq 1 "$4"); do [ "$(q "$1" "$2")" = "$3" ] && return 0; sleep 1; done; return 1; }
# ge <a> <b>: a >= b as numbers (an empty or non-numeric value fails)
ge() { python3 -c "import sys; sys.exit(0 if float('$1') >= float('$2') else 1)" 2>/dev/null; }
le() { python3 -c "import sys; sys.exit(0 if float('$1') <= float('$2') else 1)" 2>/dev/null; }
within() { python3 -c "import sys; sys.exit(0 if abs(float('$1') - float('$2')) <= float('$3') else 1)" 2>/dev/null; }
capture() {
    local tag="$1" i path t
    for i in $(mdk_seq); do
        path="$(mdk_wsl "$(mdk_dir "$i")")/coop/state_${tag}_${i}.txt"
        rm -f "$path"
        lua "$i" "coop_state(\"${tag}_${i}\")" || return 1
        for t in $(seq 1 150); do [ -f "$path" ] && break; sleep .1; done
        cp "$path" "$out/${tag}_${i}.txt" || return 1
    done
}
# the games' state dumps under tag $1 agree that player $2's canopy is in animation $3 (66 open, 67 closed)
canopy_is() { python3 "$here/presence_check.py" canopy --owner "$2" --expect "$3" "$out/${1}_1.txt" "$out/${1}_2.txt"; }
# the owner's own dump (tag $1, owner $2) has its canopy controller in animation $3
own_canopy_is() { [ "$(awk -v o="$2" '$1 == "canopy" && $2 == o {print $4}' "$out/${1}_${2}.txt")" = "$3" ]; }
far() { mdk_assert_far 2 "$1"; }
# a level again on both games (the host loads it, the client follows)
reload() {
    local n1 n2 t
    n1=$(count 1 "^level $1 checkpoint $2 started"); n2=$(count 2 "^level $1 checkpoint $2 started")
    lua 1 "mdkNewGame($1, $2)"
    for t in $(seq 1 90); do
        [ "$(count 1 "^level $1 checkpoint $2 started")" -gt "$n1" ] && [ "$(count 2 "^level $1 checkpoint $2 started")" -gt "$n2" ] && break; sleep 1; done
    sleep 12
}
# the hero of game $1 at a marker / room centre / point (a little above the floor)
at_marker() { lua "$1" "mdkGobSetPosition(mdkGetPlayerGob(), \"$2\")"; }
at_room() { lua "$1" "local x, y, z = coop_room_center(\"$2\") mdkGobSetPositionXYZ(mdkGetPlayerGob(), x, y, z + 1)"; }
at_xyz() { lua "$1" "mdkGobSetPositionXYZ(mdkGetPlayerGob(), $2, $3, $4)"; }
# gobs of game $1 whose name is $2, from its own gob list (informational: a puppet's copy of an item is one more than the game's own)
gob_count() { local n; n=$(grep -ac "" "$(mdk_log "$1")"); lua "$1" 'coop_listgobs()'; sleep 2.5
    tail -n +"$n" "$(mdk_log "$1")" | grep -ac "gob\[[0-9]*\] $2 "; }
zof() { q "$1" 'mdkGetPlayerGob().position.z'; }
trap 'cp "$(mdk_log 1)" "$out/bridge_1.log" 2>/dev/null; cp "$(mdk_log 2)" "$out/bridge_2.log" 2>/dev/null; mdk_kill' EXIT

mdk_boot
note "build: copy 1 mdk2hd.exe sha256 $(sha256sum "$(mdk_wsl "$(mdk_dir 1)")/mdk2hd.exe" | cut -c1-8), copy 2 $(sha256sum "$(mdk_wsl "$(mdk_dir 2)")/mdk2hd.exe" | cut -c1-8) (Overhaul build: 6b8b2400)"
check "both games started and each has the other's puppet" bash -c "grep -qa 'puppet spawned for player 2' '$(mdk_log 1)' && grep -qa 'puppet spawned for player 1' '$(mdk_log 2)'"
if [[ "${results[-1]}" == FAIL* ]]; then echo "the games did not start together: nothing to test"; exit 1; fi
first_level="${first%% *}"
check "both games are in level $first_level" bash -c "grep -q '^level $first_level checkpoint' '$(mdk_log 1)' && grep -q '^level $first_level checkpoint' '$(mdk_log 2)'"
lua 1 'God(1)'; lua 2 'God(1)'
cur="$first_level"

# ---- Kurt's parachute (level 1): held jump in the air opens the canopy ----
if want chute; then
[ "$cur" = 1 ] || { reload 1 5; cur=1; lua 1 'God(1)'; lua 2 'God(1)'; }
# the owner falls from 70 above the landing and holds jump (the hero is lifted back up when low, so the canopy stays open until read)
for owner in 1 2; do
    other=$((3 - owner)); ospot=l1_r8doganland1; [ "$owner" = 2 ] && ospot=l1_r8doganland1
    at_marker "$owner" "$ospot"; at_room "$other" l1_r1
    sleep 3
    check "chute: player $owner: the other player is at least 300 units away (process state)" far "chute $owner"
    z0=$(zof "$owner"); lua "$owner" "qa_z0 = mdkGetPlayerGob().position.z mdkGobSetPositionXYZ(mdkGetPlayerGob(), mdkGetPlayerGob().position.x, mdkGetPlayerGob().position.y, qa_z0 + 70)"
    sleep .3
    # free fall first (no key held): the speed to compare the glide with
    ta=$(date +%s.%N); za=$(zof "$owner"); sleep 1; zb=$(zof "$owner"); tb=$(date +%s.%N)
    fall=$(python3 -c "print('%.1f' % (($za - $zb) / ($tb - $ta)))")
    lua "$owner" "mdkGobSetPositionXYZ(mdkGetPlayerGob(), mdkGetPlayerGob().position.x, mdkGetPlayerGob().position.y, qa_z0 + 70) coop_press(57, 20000)"
    sleep 2.5
    lua "$owner" "local p = mdkGetPlayerGob() if p.position.z < qa_z0 + 30 then mdkGobSetPositionXYZ(p, p.position.x, p.position.y, qa_z0 + 70) end"
    ta=$(date +%s.%N); za=$(zof "$owner"); sleep 1.2; zb=$(zof "$owner"); tb=$(date +%s.%N)
    glide=$(python3 -c "print('%.1f' % (($za - $zb) / ($tb - $ta)))")
    capture "chute_open_$owner"
    pz=$(q "$other" "coop.puppets[$((owner - 1))].position.z"); oz=$(zof "$owner")
    note "chute player $owner: free fall $fall units/s, with the canopy $glide units/s; owner z $oz, the other game's copy z $pz"
    check "chute: player $owner: used (held jump in the air opens the canopy: animation 66 in the owner's own game)" own_canopy_is "chute_open_$owner" "$owner" 66
    check "chute: player $owner: local effect (the descent under the canopy is under 8 units/s, free fall was over 15)" bash -c "$(declare -f le ge); le $glide 8 && ge $fall 15"
    check "chute: player $owner: seen by the other (the other game's copy of the canopy is in animation 66 as the owner's, same flags and phase)" canopy_is "chute_open_$owner" "$owner" 66
    check "chute: player $owner: seen by the other (the other game's copy of the hero is within 6 units of the owner's height)" bash -c "$(declare -f within); within $oz $pz 6"
    lua "$owner" 'coop_press(57, 1)'
    at_marker "$owner" "$ospot"
    sleep 1.5
    capture "chute_land_$owner"
    check "chute: player $owner: the canopy closes on landing in both games (animation 67)" canopy_is "chute_land_$owner" "$owner" 67
    sleep 3
done
fi

# ---- Max's jetpack (level 2, checkpoint 8: the jetpack and fuel are given at the start) ----
if want jet; then
[ "$cur" = 2 ] || { reload 2 8; cur=2; lua 1 'God(1)'; lua 2 'God(1)'; }
read -r sx sy sz < <(printf '%s %s %s\n' "$(q 1 'mdkGetPlayerGob().position.x')" "$(q 1 'mdkGetPlayerGob().position.y')" "$(q 1 'mdkGetPlayerGob().position.z')")
for owner in 1 2; do
    other=$((3 - owner)); slot=$((owner - 1))
    at_xyz "$owner" "$sx" "$sy" "$sz"; at_room "$other" l2_r1
    sleep 4
    check "jet: player $owner: the other player is at least 300 units away (process state)" far "jet $owner"
    z0=$(zof "$owner"); pz0=$(q "$other" "coop.puppets[$slot].position.z")
    lua "$owner" 'coop_press(57, 4500)'
    sleep 1.6
    z1=$(zof "$owner"); pz1=$(q "$other" "coop.puppets[$slot].position.z")
    own_up=$(q "$owner" 'omAnimIsPlaying(mdkGetPlayerGob(), ANIM_UP)'); pup_up=$(q "$other" "omAnimIsPlaying(coop.puppets[$slot], ANIM_UP)")
    note "jet player $owner: owner z $z0 -> $z1, the other game's copy z $pz0 -> $pz1; flight pose (ANIM_UP) owner $own_up, copy $pup_up"
    check "jet: player $owner: used (held jump, flight pose ANIM_UP playing in the owner's game)" test "$own_up" = 1
    check "jet: player $owner: local effect (lifted at least 8 units in 1.6 s)" bash -c "$(declare -f ge); ge \$(python3 -c \"print($z1 - $z0)\") 8"
    check "jet: player $owner: seen by the other (the copy rose at least 6 units, in the flight pose)" bash -c "$(declare -f ge); ge \$(python3 -c \"print($pz1 - $pz0)\") 6 && test '$pup_up' = 1"
    sleep 6
done
fi

# ---- Doc's leaf blower (level 3, checkpoint 6): the blower taken up, fired at an enemy that is held still ----
if want blower; then
[ "$cur" = 3 ] || { reload 3 6; cur=3; lua 1 'God(1)'; lua 2 'God(1)'; }
# A player sent from the bar room to the blower spot crosses the teleporter room on the way in the other game's smoothed
# copy of it, and that room's entry starts the boss cutscene (l3_teleport.OnEnterRoom), which takes the input: the
# client's blow was swallowed whenever the cutscene began before it. The scenario needs no cutscene.
lua 1 'l3_teleport.OnEnterRoom = function(gob) end'; lua 2 'l3_teleport.OnEnterRoom = function(gob) end'
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
for owner in 1 2; do
    other=$((3 - owner)); dog=l3_dogan0$owner; py=44
    check "blower: player $owner: used (the leaf blower item is taken up and built: the game's own blower gob exists)" take_blower "$owner"
    # the other player far away (level 3's bar room); the owner 5 units behind the enemy, facing +x; the host holds the enemy still (a sleeping one is put in place)
    at_xyz "$other" -144.3 -239.8 2.5       # beside the level-3 bar room items, 348 units from the enemy and Doc
    lua "$owner" "local d = mdkGetPlayerGob() mdkGobSetPositionXYZ(d, 58, $py, 0.6) coop.Face(d, 1, 0, 0)"
    sleep 1
    lua 1 "StopScript($dog) mdkWalkerStop($dog) mdkGobSetPositionXYZ($dog, 63, $py, 0.4)"
    sleep 3
    check "blower: player $owner: the other player is at least 300 units away (process state)" far "blower $owner"
    h0=$(q 1 "$dog.position.x"); c0=$(q 2 "$dog.position.x")
    check "blower: player $owner: before: the enemy stands in the same place in both games" bash -c "$(declare -f within); within $h0 $c0 1.5"
    lua "$owner" 'coop_press_frames(256, 240)'
    sleep 5
    h1=$(q 1 "$dog.position.x"); c1=$(q 2 "$dog.position.x")
    sleep 4
    h2=$(q 1 "$dog.position.x"); c2=$(q 2 "$dog.position.x")
    capture "blower_$owner"
    note "blower player $owner: enemy $dog x host $h0 -> $h1 -> $h2, client $c0 -> $c1 -> $c2"
    mine0=$h0; mine1=$h1; seen1=$c1; mine2=$h2; seen2=$c2
    [ "$owner" = 2 ] && { mine0=$c0; mine1=$c1; seen1=$h1; mine2=$c2; seen2=$h2; }
    check "blower: player $owner: local effect (the enemy is blown at least 8 units in the owner's game)" bash -c "$(declare -f ge); ge \$(python3 -c \"print($mine2 - $mine0)\") 8"
    check "blower: player $owner: seen by the other (the enemy is blown at least 8 units in the other game too)" bash -c "$(declare -f ge); ge \$(python3 -c \"print($seen2 - $h0 if $owner == 2 else $seen2 - $c0)\") 8"
    check "blower: player $owner: shared effect (the enemy ends in the same place in both games, within 3 units)" bash -c "$(declare -f within); within $h2 $c2 3"
    at_xyz 1 -144.3 -239.8 2.5; at_xyz 2 -144.3 -239.8 2.5
    sleep 2
done
fi

if want blower; then note "blower: gobs named docleafblower in game 1: $(gob_count 1 docleafblower), in game 2: $(gob_count 2 docleafblower) (each game's own blower item is one; a second would be the other player's copy of it)"; fi
printf '%s\n' "${results[@]}"
echo "$pass passed, $fail failed"
echo "process evidence: $out"
[ "$fail" -eq 0 ]
