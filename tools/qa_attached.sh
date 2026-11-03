#!/usr/bin/env bash
# The equipment and attached visuals of the other players, on three peers: Kurt's canopy, Max's jetpack (and its
# flame while thrusting) and Doc's leaf blower (the item and its air puffs). Each is a separate gob the game builds
# on the player's body; every peer reads it from its own gob table: alive, attached to that player's body, on the
# same bone, shown, and in the owner's animation (jetpack: 62 idle, 77 thrusting; blower: 65 idle, 77 blowing), with
# the sub-gobs its model made shown or hidden as on the owner's side (the jetpack's flame while thrusting), and on the
# pose of the bone the owner's game puts it on (within 0.02 units / 3 degrees, tools/presence_check.py); it must be
# gone on every peer when it ends on the owner's side. Players start in different rooms, at least 300 units
# apart (asserted from the dumps, tools/targeting_geometry.py). Three scenarios, one boot each: Kurt (level 1),
# Max (level 2) and Doc (level 3), every peer taking its turn as the owner.
#   MUTANT=gear    peers never build the equipment gobs of other players: must fail
#   MUTANT=canopy  peers never copy the canopy state: must fail
#   MUTANT=shown   peers leave the canopy's drawn/hidden bit to the game's room handling: must fail
#   MUTANT=subs    peers leave the item's sub-gobs to the copied animation (the flame comes on late): must fail
#   MUTANT=gearbone  peers build the items on another bone of the body: must fail
#   ONLY=kurt|max|doc  one scenario only (the default runs all three)
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N=3
source "$here/mdk.sh"
scenario=attached
source "$here/presence_common.sh"
case "${MUTANT:-}" in ''|gear|canopy|shown|subs|gearbone) ;; *) echo 'Unknown attached mutant' >&2; exit 2 ;; esac
check_py() { python3 "$here/presence_check.py" "$@"; }
files() { echo "$out/$1_1.txt" "$out/$1_2.txt" "$out/$1_3.txt"; }
# settle <tag> <oracle args...> [+ <oracle args...>]: capture until the oracle (every one of them, on the same
# dumps) passes (up to ~10 s), then report each once
settle() {
    local tag="$1" i j ok; shift
    local -a sets=() cur=()
    for a in "$@"; do
        if [ "$a" = + ]; then sets+=("${cur[*]}"); cur=(); else cur+=("$a"); fi
    done
    sets+=("${cur[*]}")
    for i in $(seq 1 16); do
        hook
        capture "$tag"; ok=1
        if [ -n "${QA_KEEP:-}" ]; then for j in 1 2 3; do cp "$out/${tag}_$j.txt" "$out/${tag}.try$i.$j.txt"; done; fi
        for j in "${sets[@]}"; do check_py $j $(files "$tag") >/dev/null 2>&1 || ok=0; done
        [ $ok = 1 ] && break
        sleep .6
    done
    for j in "${sets[@]}"; do check "$tag: $j" check_py $j $(files "$tag"); done
}
hook() { :; }       # runs before every capture of a settle; the Kurt scenario uses it to keep the owner airborne
want() { [ -z "${ONLY:-}" ] || [ "$ONLY" = "$1" ]; }
boot_level() {  # <level> <checkpoint>
    mdk_kill; sleep 2
    MDK2_BACKGROUND=1 MDK2_START="$1 $2" "$here/install.sh" >/dev/null
    mdk_boot; sleep 14
    case "${MUTANT:-}" in
        gear) for i in 1 2 3; do lua "$i" 'coop_presence_test(0,0,0,0,0,1)'; done ;;
        canopy) for i in 1 2 3; do lua "$i" 'coop_presence_test(1,0,0,0,0,0)'; done ;;
        shown) for i in 1 2 3; do lua "$i" 'coop_presence_test(0,0,0,0,0,0,1)'; done ;;
        subs) for i in 1 2 3; do lua "$i" 'coop_presence_test(0,0,0,0,0,0,0,1)'; done ;;
        gearbone) for i in 1 2 3; do lua "$i" 'coop_presence_test(0,0,0,0,0,0,0,0,1)'; done ;;
    esac
}
place() {  # <peer> <marker>: the peer's player on a marker gob
    lua "$1" "mdkGobSetPosition(mdkGetPlayerGob(),\"$2\")"
}
placexyz() {  # <peer> <x> <y> <z>: the peer's player at a point just above a floor
    lua "$1" "mdkGobSetPositionXYZ(mdkGetPlayerGob(),$2,$3,$4)"
}

# ---- Kurt: the canopy child, opened by the player's own held jump in the air ----
if want kurt; then
boot_level 1 5
place 1 l1_r8doganland1; place 2 l1_r1grengen
sleep 3
for owner in 1 2 3; do
    check "kurt $owner: every player is at least 300 units from the others" mdk_assert_far "$owner" "kurt $owner"
    lua "$owner" 'qa_z0=mdkGetPlayerGob().position.z mdkGobSetPositionXYZ(mdkGetPlayerGob(),mdkGetPlayerGob().position.x,mdkGetPlayerGob().position.y,qa_z0+70) coop_press(57,14000)'
    # the owner holds jump and is lifted back up whenever it comes low, so the canopy stays open until every peer was read
    hook() { lua "$owner" 'local p=mdkGetPlayerGob() if p.position.z < qa_z0+30 then mdkGobSetPositionXYZ(p,p.position.x,p.position.y,qa_z0+70) end'; }
    settle "kurt_open_$owner" canopy --owner "$owner" --expect 66 + gear --owner "$owner" --name chute --hidden 0
    hook() { :; }
    lua "$owner" 'coop_press(57,1)'
    sleep 4
done
fi

# ---- Max: the jetpack gob, thrust animation, and its end ----
if want max; then
boot_level 2 8
placexyz 2 1343 899 250; placexyz 3 107 695 3
sleep 4
for owner in 1 2 3; do
    check "max $owner: every player is at least 300 units from the others" mdk_assert_far "$owner" "max $owner"
    settle "max_idle_$owner" gear --owner "$owner" --name maxjetpack --expect 62
    lua "$owner" 'coop_press(57,4500)'
    sleep 1.5
    # the owner's game shows the flame the moment the thrust starts (and hides it when it ends); a puppet left to its
    # copied animation followed 0.36 s or more late: every peer's flame changes within 200 ms of the owner's
    # (measured about 100 ms)
    settle "max_thrust_$owner" gear --owner "$owner" --name maxjetpack --expect 77 --lag 200
    sleep 5
    settle "max_rest_$owner" gear --owner "$owner" --name maxjetpack --expect 62 --lag 200
    lua "$owner" 'omGobDelete(coop_child(mdkGetPlayerGob(),"maxjetpack"))'
    settle "max_gone_$owner" gear --owner "$owner" --name maxjetpack --absent
done
fi

# ---- Doc: the leaf blower item (the game's own take-up code), blowing, and its end ----
if want doc; then
boot_level 3 6
placexyz 2 -28 322 3; placexyz 3 -137 -271 3
sleep 4
for owner in 1 2 3; do
    check "doc $owner: every player is at least 300 units from the others" mdk_assert_far "$owner" "doc $owner"
    lua "$owner" 'coop_test_blower(mdkGetPlayerGob())'
    sleep 2
    settle "doc_idle_$owner" gear --owner "$owner" --name docleafblower --expect 65
    lua "$owner" 'coop_press(256,4500)'
    sleep 1.5
    settle "doc_blow_$owner" gear --owner "$owner" --name docleafblower --expect 77
    sleep 4
    settle "doc_rest_$owner" gear --owner "$owner" --name docleafblower --expect 65
    lua "$owner" 'omGobDelete(coop_child(mdkGetPlayerGob(),"docleafblower"))'
    settle "doc_gone_$owner" gear --owner "$owner" --name docleafblower --absent
done
fi
summary
