#!/usr/bin/env bash
# Attached canopy controller replication on three peers, including client relays.
# Controller replication is isolated first, followed by native Space/glide input,
# steering, landing and release. MUTANT=canopy must exit nonzero.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N=3
source "$here/mdk.sh"
scenario=canopy
source "$here/presence_common.sh"
MDK2_BACKGROUND=1 MDK2_START='1 5' "$here/install.sh" >/dev/null
mdk_boot
if [ "${MUTANT:-}" = canopy ]; then
    for i in 1 2 3; do lua "$i" 'coop_presence_test(1,0,0,0)'; done
elif [ -n "${MUTANT:-}" ]; then echo 'Unknown canopy mutant' >&2; exit 2; fi
for owner in 1 2 3; do
    for phase in deploy open steer land cutaway; do
        case "$phase" in
            deploy|open|steer) anim=ANIM_OPEN; expect=66 ;;
            *) anim=ANIM_CLOSE; expect=67 ;;
        esac
        # Use the actual attached native canopy, not a synthetic ghost or Lua flag.
        lua "$owner" "omAnimPlay(coop_canopy(mdkGetPlayerGob()),$anim,ANIMFLAG_NOTRANS)"
        if [ "$phase" = steer ]; then lua "$owner" 'omAnimPlay(mdkGetPlayerGob(),ANIM_CHUTEL,ANIMFLAG_NOTRANS)'; fi
        sleep 1
        tag="${phase}_${owner}"
        capture "$tag"
        check "$phase canopy of player $owner matches in every process" python3 "$here/presence_check.py" canopy --owner "$owner" --expect "$expect" "$out/${tag}_1.txt" "$out/${tag}_2.txt" "$out/${tag}_3.txt"
    done
done
# End-to-end input fixture. Do not set native canopy state here: the game's
# player controller must deploy/close it in response to input and ground contact.
for owner in 1 2 3; do
    lua "$owner" 'local p=mdkGetPlayerGob() mdkGobSetPosition(p,"l1_r8doganland1") mdkGobSetPositionXYZ(p,p.position.x,p.position.y,p.position.z+40) coop_press(57,12000)'
    # The player has to fall and the game to read the held key before it opens the canopy; a game that runs slowly
    # (the Steam build in the background) needs more than a second. Wait for the owner's own canopy to reach the
    # state the real input produces, as that process reports it, then let all peers be judged on that same state.
    tag="input_open_$owner"
    for _ in $(seq 1 20); do
        sleep .5
        capture "$tag"
        python3 "$here/presence_check.py" canopy --owner "$owner" --expect 66 "$out/${tag}_1.txt" "$out/${tag}_2.txt" "$out/${tag}_3.txt" >/dev/null 2>&1 && break
    done
    check "native Space deploy of player $owner reaches all peers" python3 "$here/presence_check.py" canopy --owner "$owner" --expect 66 "$out/${tag}_1.txt" "$out/${tag}_2.txt" "$out/${tag}_3.txt"
    lua "$owner" 'coop_press(30,1500)'
    sleep .5
    tag="input_steer_$owner"
    capture "$tag"
    check "canopy stays open while player $owner steers" python3 "$here/presence_check.py" canopy --owner "$owner" --expect 66 "$out/${tag}_1.txt" "$out/${tag}_2.txt" "$out/${tag}_3.txt"
    lua "$owner" 'mdkGobSetPosition(mdkGetPlayerGob(),"l1_r8doganland1")'
    sleep 1
    tag="input_land_$owner"
    capture "$tag"
    check "native ground contact closes player $owner canopy" python3 "$here/presence_check.py" canopy --owner "$owner" --expect 67 "$out/${tag}_1.txt" "$out/${tag}_2.txt" "$out/${tag}_3.txt"
    lua "$owner" 'coop_press(57,1)'
    sleep .2
    lua "$owner" 'local p=mdkGetPlayerGob() mdkGobSetPositionXYZ(p,p.position.x,p.position.y,p.position.z+40) coop_press(57,3000)'
    sleep 1
    lua "$owner" 'coop_press(57,1)'
    sleep .5
    tag="input_cut_$owner"
    capture "$tag"
    check "native release cuts away player $owner canopy" python3 "$here/presence_check.py" canopy --owner "$owner" --expect 67 "$out/${tag}_1.txt" "$out/${tag}_2.txt" "$out/${tag}_3.txt"
done
summary
