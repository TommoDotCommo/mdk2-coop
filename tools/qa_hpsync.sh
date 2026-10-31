#!/usr/bin/env bash
# An enemy's hp changed by the game without a hit reaches the clients. Two background copies start level 2 at checkpoint 4.
# The host changes one enemy's hp with no damage event (coop_test_sethp, the way a script tops a turret up or a downed
# grunt stands up again at full hp), first to 33 and then to 61; each time both games dump their state and
# tools/hpsync_check.py fails unless the client holds the host's number within HPSYNC_WAIT (3) seconds.
# Geometry (docs/TESTING.md): the client stands at HPSYNC_CLIENT_XYZ (default in front of the arena's exit door), far
# outside every splash radius of the host, asserted from both games' dumps before and after.
# MUTANT=nosync: the host does not send hp changes that come without a hit (coop.ini [test] no_hpsync=1, the bug): must fail.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N=2 MDK2_BACKGROUND=1 MDK2_START='2 4'
source "$here/mdk.sh"
scenario=hpsync
source "$here/presence_common.sh"
case "${MUTANT:-}" in ''|nosync) : ;; *) echo 'Unknown hpsync mutant' >&2; exit 2 ;; esac
"$here/install.sh" >/dev/null
if [ "${MUTANT:-}" = nosync ]; then
    printf 'no_hpsync=1\r\n' >> "$(mdk_wsl "$(mdk_dir 1)")/coop/coop.ini"
fi
mdk_boot
lua 1 'God(1)'
lua 2 'God(1)'
read -r cx cy cz <<< "${HPSYNC_CLIENT_XYZ:-500 1026 6}"
lua 2 "mdkGobSetPositionXYZ(mdkGetPlayerGob(),$cx,$cy,$cz)"
sleep 3
check "the client is outside every splash radius of the host (process state, before)" mdk_assert_far 2 "hpsync before"
capture before
read -r nid hp0 < <(python3 "$here/hpsync_check.py" pick "$out/before_1.txt")
echo "victim $nid, hp $hp0 on the host"
for want in 33 61; do
    lua 1 "coop_test_sethp(coop_gob(\"n$nid\"), $want)"
    sleep "${HPSYNC_WAIT:-3}"
    capture "set$want"
    check "the client holds the host's hp $want after a change without a hit" \
        python3 "$here/hpsync_check.py" check "$nid" "$want" "$out/set${want}_1.txt" "$out/set${want}_2.txt"
done
check "the client is outside every splash radius of the host (process state, after)" mdk_assert_far 2 "hpsync after"
cp "$(mdk_log 1)" "$out/bridge_1.log"; cp "$(mdk_log 2)" "$out/bridge_2.log"
summary
