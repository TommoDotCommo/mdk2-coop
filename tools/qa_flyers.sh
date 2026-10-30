#!/usr/bin/env bash
# Flying enemies stay where the host has them on a client. Two background copies start level 2 at checkpoint 4,
# where the room 4 mantas leave their flock and attack. Both games dump their state at the same moment once a
# second for FLYERS_SECONDS (20); tools/flyers_check.py fails when a manta is more than FLYERS_LIMIT (30) units
# from the host's position in the client's game, or when no manta moved on the host.
# Geometry (docs/TESTING.md): the client stands at FLYERS_CLIENT_XYZ (default in front of the arena's exit door),
# far outside every splash radius of the host, asserted from both games' dumps before and after.
# MUTANT=attached: the client keeps its mirrored enemies on their parent's bone (coop.ini [test]
# keep_attached=1, the bug): must fail.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N=2 MDK2_BACKGROUND=1 MDK2_START='2 4'
source "$here/mdk.sh"
scenario=flyers
source "$here/presence_common.sh"
case "${MUTANT:-}" in ''|attached) : ;; *) echo 'Unknown flyers mutant' >&2; exit 2 ;; esac
"$here/install.sh" >/dev/null
if [ "${MUTANT:-}" = attached ]; then
    printf 'keep_attached=1\r\n' >> "$(mdk_wsl "$(mdk_dir 2)")/coop/coop.ini"
fi
mdk_boot
lua 1 'God(1)'
lua 2 'God(1)'
# FLYERS_CLIENT_XYZ=none leaves the client beside the host (diagnosis only: the geometry checks fail)
read -r cx cy cz <<< "${FLYERS_CLIENT_XYZ:-500 1026 6}"
if [ "$cx" != none ]; then lua 2 "mdkGobSetPositionXYZ(mdkGetPlayerGob(),$cx,$cy,$cz)"; fi
sleep 3
check "the client is outside every splash radius of the host (process state, before)" mdk_assert_far 2 "flyers before"
pairs=()
for t in $(seq 1 "${FLYERS_SECONDS:-20}"); do
    tag="fly$t"
    for i in $(mdk_seq); do rm -f "$(mdk_wsl "$(mdk_dir "$i")")/coop/state_$tag.txt"; done
    lua 1 "coop_state(\"$tag\")"
    for k in $(seq 1 50); do
        [ -f "$(mdk_wsl "$(mdk_dir 1)")/coop/state_$tag.txt" ] && [ -f "$(mdk_wsl "$(mdk_dir 2)")/coop/state_$tag.txt" ] && break
        sleep 0.1
    done
    for i in $(mdk_seq); do mv "$(mdk_wsl "$(mdk_dir "$i")")/coop/state_$tag.txt" "$out/${tag}_$i.txt"; done
    pairs+=("$out/${tag}_1.txt" "$out/${tag}_2.txt")
    sleep 1
done
check "the client is outside every splash radius of the host (process state, after)" mdk_assert_far 2 "flyers after"
check "the client's flyers stay within ${FLYERS_LIMIT:-30} units of the host's" \
    python3 "$here/flyers_check.py" --limit "${FLYERS_LIMIT:-30}" "${pairs[@]}"
python3 "$here/flyers_check.py" --limit "${FLYERS_LIMIT:-30}" "${pairs[@]}" > "$out/flyers_check.txt" || true
cat "$out/flyers_check.txt"
cp "$(mdk_log 1)" "$out/bridge_1.log"; cp "$(mdk_log 2)" "$out/bridge_2.log"
summary
