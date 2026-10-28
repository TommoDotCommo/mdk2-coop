#!/usr/bin/env bash
# Direct client LOS targeting: real Grunt AI, host >25 units away, no AOE damage.
# Requires two disposable copies. MUTANT=targeting or hostaoe must exit nonzero.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N=2
source "$here/mdk.sh"
scenario=ai
source "$here/presence_common.sh"
MDK2_BACKGROUND=1 MDK2_START='1 2' "$here/install.sh" >/dev/null
mdk_boot
# Both stand on known room markers. The enemy must cross the gap itself.
lua 1 'God(0) mdkGobSetPosition(mdkGetPlayerGob(),"l1_r1grengen") coop_presence_test(0,0,0,1)'
lua 2 'God(0) mdkGobSetPosition(mdkGetPlayerGob(),"l1_r8doganland1")'
sleep 3
case "${MUTANT:-}" in
    targeting) lua 1 'coop_presence_test(0,1,0,1)' ;;
    hostaoe) lua 1 'coop_presence_test(0,0,1,1)' ;;
    '') : ;;
    *) echo 'Unknown AI mutant' >&2; exit 2 ;;
esac
capture before
# Grunt (202) has a native direct strike with a target/angle check at 0x434e16.
# Its damage call returns to 0x434e56. Test mode rejects every other damage
# caller on the host, including all projectile/explosion/AOE paths.
lua 1 'local p=coop_gob("@p2") qa_melee=mdkCreateObjectLua("qa_melee",OBJ_GRUNT,mdkGetScene(),nil,nil,p.position.x+8,p.position.y,p.position.z+1,1,0,0,0,nil,0,0,0,0)'
sleep 2
lua 1 'coop_listgobs() coop_animdump(qa_melee)'
# Allow acquisition, chase and native animation callbacks to make a direct hit.
passed=0
for t in $(seq 1 "${AI_SECONDS:-20}"); do
    sleep 1
    capture after
    if python3 "$here/presence_check.py" ai "$out/before_1.txt" "$out/before_2.txt" "$out/after_1.txt" "$out/after_2.txt" >"$out/oracle.txt"; then passed=1; break; fi
done
check 'client selected, chased and directly hurt; host far away and unharmed; AOE excluded' test "$passed" -eq 1
cat "$out/oracle.txt"
summary
