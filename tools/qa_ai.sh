#!/usr/bin/env bash
# Direct native Grunt targeting, approach and damage, with the other player far away.
# Requires two disposable copies. MUTANT=targeting or hostaoe must exit nonzero.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N=2
source "$here/mdk.sh"
scenario=ai
source "$here/presence_common.sh"
MDK2_BACKGROUND=1 MDK2_START='1 2' "$here/install.sh" >/dev/null
mdk_boot
lua 1 'God(0) coop_presence_test(0,0,0,1)'
lua 2 'God(0)'
case "${MUTANT:-}" in
    targeting) lua 1 'coop_presence_test(0,1,0,1)' ;;
    hostaoe) lua 1 'coop_presence_test(0,0,1,1)' ;;
    '') : ;;
    *) echo 'Unknown AI mutant' >&2; exit 2 ;;
esac
run_case() {
    local who="$1" kind=ai target='coop_gob("@p2")' t passed=0
    lua 1 'if (qa_melee) then omGobDelete(qa_melee) qa_melee=nil end'
    if [ "$who" = host ]; then
        kind=ai-host; target='mdkGetPlayerGob()'
        lua 2 'mdkGobSetPosition(mdkGetPlayerGob(),"l1_r1grengen")'
        lua 1 'mdkGobSetPosition(mdkGetPlayerGob(),"l1_r8doganland1")'
    else
        lua 1 'mdkGobSetPosition(mdkGetPlayerGob(),"l1_r1grengen")'
        lua 2 'mdkGobSetPosition(mdkGetPlayerGob(),"l1_r8doganland1")'
    fi
    sleep 3
    # Reset the level's native target override to its host default. This never
    # names the client: selecting the nearby puppet still requires the mod.
    lua 1 'mdkSetAITargetGob(mdkGetPlayerGob())'
    # 0x434e25..0x434e3d supplies the native strike's 50-unit reach and pi/4
    # angle to its LOS check. Start outside reach; let the default AI approach.
    # Alert matches the game's guard spawn scripts. No motion/attack is injected.
    lua 1 "local p=$target qa_melee=mdkCreateObjectLua(\"qa_melee\",OBJ_GRUNT,mdkGetScene(),nil,nil,p.position.x-64,p.position.y,p.position.z+1,1,0,0,0,nil,0,0,0,0)"
    capture "${who}_before"
    if [ "${AI_NO_ALERT:-0}" != 1 ]; then lua 1 'mdkWalkerAlert(qa_melee,1)'; fi
    for t in $(seq 1 "${AI_SECONDS:-25}"); do
        sleep 1
        capture "${who}_after"
        if python3 "$here/presence_check.py" "$kind" "$out/${who}_before_1.txt" "$out/${who}_before_2.txt" "$out/${who}_after_1.txt" "$out/${who}_after_2.txt" >"$out/${who}_oracle.txt"; then passed=1; break; fi
    done
    check "$who selected, approached and directly hurt; distant player unharmed; AOE excluded" test "$passed" -eq 1
    cat "$out/${who}_oracle.txt"
}
# The damage hook admits only native Grunt/Conehead direct strike callers
# 0x434e56/0x434dfb. Projectiles, explosions and AOE cannot satisfy either case.
run_case client
run_case host
# A bad native script task must not leave the host impersonating its target.
# The expected Lua error is caught by ScriptUpdate's protected call.
if [ -z "${MUTANT:-}" ]; then
lua 1 'mdkGobSetPosition(qa_melee,"l1_r1grengen") local saved=qa_melee.stack qa_melee.stack={script={{function(g) error("qa_ai expected task error") end}},nexttask=1} ScriptUpdate(qa_melee) qa_melee.stack=saved'
capture restored
check 'script error restores native player handle and scope depth' python3 - "$out/restored_1.txt" "$here" <<'PYRESTORE'
import sys
sys.path.insert(0,sys.argv[2])
from presence_check import load,number
state=load(sys.argv[1])
assert number(state,'ai_scope_depth') == 0
assert state['engine_player'] == [['@p1']]
PYRESTORE
fi
summary
