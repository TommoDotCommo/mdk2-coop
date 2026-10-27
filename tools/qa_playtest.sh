#!/usr/bin/env bash
# Paired regression for shared native pickups, native enemy health fractions and
# level 1's mortar dome results. MUTANT=pickups or dome disables that protection.
# The dome fixture delivers collision + explosion callbacks, not a flight test.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export MDK2_N=2
source "$here/mdk.sh"
MDK2_BACKGROUND=1 MDK2_START='1 1' "$here/install.sh" >/dev/null || exit 1
out="$here/../local/qa/$(date +%Y%m%d-%H%M%S)-playtest"; mkdir -p "$out"
pass=0; fail=0
lua() { mdk_cmd "$(mdk_pid "$1")" "$2"; }
has() { grep -qaE "$2" "$(mdk_log "$1")"; }
q() {
    local tag="Q$RANDOM$RANDOM" t
    lua "$1" "coop_log(format(\"$tag %s\", tostring($2)))" || return 1
    for t in $(seq 1 20); do
        if has "$1" "^lua: $tag "; then
            grep -aE "^lua: $tag " "$(mdk_log "$1")" | tail -1 | sed 's/^lua: [A-Z0-9]* //' | tr -d '\r'; return
        fi
        sleep 0.1
    done
    return 1
}
waitq() { local t; for t in $(seq 1 "${4:-10}"); do [ "$(q "$1" "$2")" = "$3" ] && return 0; sleep 1; done; return 1; }
check() { local name="$1"; shift; if "$@" >"$out/check-$((pass+fail+1)).txt" 2>&1; then echo "PASS $name"; pass=$((pass+1)); else echo "FAIL $name"; fail=$((fail+1)); fi; }
finish() { local i; for i in 1 2; do cp "$(mdk_log "$i")" "$out/game$i.log" 2>/dev/null || :; done; mdk_kill; }
trap finish EXIT
mdk_boot
check 'both games reached level 1' waitq 2 'coop.level' 1 30
if [ "${MUTANT:-}" = pickups ]; then for i in 1 2; do lua "$i" 'coop.SharedPickup = function(name, itemtype) end'; done; fi
if [ "${MUTANT:-}" = dome ]; then lua 2 'coop.DomeTick = function() end'; fi
for i in 1 2; do
    lua "$i" 'God(0) mdkGobSetPosition(mdkGetPlayerGob(), "l1_r1grengen")'
    check "player $i starts with a full native enemy bar" waitq "$i" 'floor(mdkGobGetHealth(r2_dog01)*100+0.5)' 100
    check "player $i sees scaled maximum hp" waitq "$i" 'coop_max_hp(r2_dog01)' 150
    lua "$i" 'mdkDealDamage(nil, mdkGetPlayerGob(), 60, 2, -1)'
    check "player $i fixture hp is 40" waitq "$i" 'coop_hp(mdkGetPlayerGob())' 40
    lua "$i" 'qaheal=mdkCreateObjectLua("qaheal", OBJ_HAM, mdkGetScene(), nil, nil, 0,0,0, 1,0,0,0, nil,0,0,0,0) mdkGobSetPosition(qaheal,"l1_r8doganland1")'
done
lua 1 'mdkGobSetPositionXYZ(qaheal, mdkGetPlayerGob().position.x, mdkGetPlayerGob().position.y, mdkGetPlayerGob().position.z)'
for i in 1 2; do check "host's health pickup heals player $i by 50" waitq "$i" 'coop_hp(mdkGetPlayerGob())' 90; done
for i in 1 2; do
    lua "$i" 'qammo=mdkCreateObjectLua("qammo", OBJ_SNIPERMORTARITEM, mdkGetScene(), nil, nil, 0,0,0, 1,0,0,0, nil,0,0,0,0) mdkGobSetPosition(qammo,"l1_r8doganland1")'
    lua "$i" 'qa_before=coop_item_count(mdkGetPlayerGob(), OBJ_SNIPERMORTARITEM)'
done
lua 2 'mdkGobSetPositionXYZ(qammo, mdkGetPlayerGob().position.x, mdkGetPlayerGob().position.y, mdkGetPlayerGob().position.z)'
for i in 1 2; do
    check "client's mortar pickup gives player $i five rounds" waitq "$i" 'coop_item_count(mdkGetPlayerGob(), OBJ_SNIPERMORTARITEM)-qa_before' 5
    check "player $i removed the source ammo item" waitq "$i" 'coop_alive(qammo)' nil
    lua "$i" 'qa_before=coop_item_count(mdkGetPlayerGob(), OBJ_GRENADEITEM)'
done
lua 1 'local p=mdkGetPlayerGob() mdkCreateObjectLua("", OBJ_GRENADEITEM, mdkGetScene(), nil, nil, p.position.x,p.position.y,p.position.z, 1,0,0,0, nil,0,0,0,0)'
for i in 1 2; do check "unnamed pickup grants player $i five grenades" waitq "$i" 'coop_item_count(mdkGetPlayerGob(), OBJ_GRENADEITEM)-qa_before' 5; done
lua 1 'mdkDealDamage(mdkGetPlayerGob(),r2_dog01,50,DAMAGE_SNIPER,-1)'
hp=$(q 1 'coop_hp(r2_dog01)')
for i in 1 2; do
    check "player $i sees the host's reduced enemy hp" waitq "$i" 'coop_hp(r2_dog01)' "$hp"
    check "player $i native bar matches scaled hp" waitq "$i" 'floor(mdkGobGetHealth(r2_dog01)*150+0.5)' "$hp"
done
# The engine normally calls this pair when a mortar hits and then explodes.
# Make the projectile far away so the peer cannot complete the puzzle by chance.
lua 1 'qa_mortar=mdkCreateObjectLua("qa_mortar",OBJ_SNIPERMORTAR,mdkGetScene(),nil,nil,0,0,0,1,0,0,0,nil,0,0,0,0) Level.PlatformOnCollision(platportal1,qa_mortar,-1) Level.BobOnShotExploded(mdkGetPlayerGob())'
for i in 1 2; do
    check "player $i first dome shattered" waitq "$i" 'platportal1.shattered' 1
    check "player $i advances mortar spawn point" waitq "$i" 'l1_r7.SpawnPoint' l1_r7mortar02
    check "player $i marks first platform complete" waitq "$i" 'l1_r7.plat1dead' 1
done
# A different player can complete the next dome; repeat must be harmless.
lua 2 'qa_mortar=mdkCreateObjectLua("qa_mortar2",OBJ_SNIPERMORTAR,mdkGetScene(),nil,nil,0,0,0,1,0,0,0,nil,0,0,0,0) Level.PlatformOnCollision(platportal2,qa_mortar,-1) Level.BobOnShotExploded(mdkGetPlayerGob())'
for i in 1 2; do
    check "player $i second dome shattered" waitq "$i" 'platportal2.shattered' 1
    check "player $i advances to third mortar point" waitq "$i" 'l1_r7.SpawnPoint' l1_r7mortar03
done
# A world resend must restore dome results without granting consumed ammo again.
lua 2 'qa_ammo_after=coop_item_count(mdkGetPlayerGob(),OBJ_SNIPERMORTARITEM) qa_grenades_after=coop_item_count(mdkGetPlayerGob(),OBJ_GRENADEITEM) coop_ready()'
sleep 9
check 'world resend does not regrant mortar ammo' waitq 2 'coop_item_count(mdkGetPlayerGob(),OBJ_SNIPERMORTARITEM)-qa_ammo_after' 0
check 'world resend does not regrant grenades' waitq 2 'coop_item_count(mdkGetPlayerGob(),OBJ_GRENADEITEM)-qa_grenades_after' 0
check 'world resend preserves puzzle progress' waitq 2 'l1_r7.SpawnPoint' l1_r7mortar03
# Native reward collection also works when teammates choose different heroes.
lua 2 'coop.SwitchChar(mdkGetPlayerGob(),OBJ_DOC)'
check 'client changed to Doc' waitq 2 'mdkGetGobType(mdkGetPlayerGob()) == OBJ_DOC and 1 or 0' 1
for i in 1 2; do
    lua "$i" 'God(0) mdkDealDamage(nil,mdkGetPlayerGob(),40,2,-1) qa_hurt_hp=coop_hp(mdkGetPlayerGob()) qaapple=mdkCreateObjectLua("qaapple",OBJ_APPLE,mdkGetScene(),nil,nil,0,0,0,1,0,0,0,nil,0,0,0,0) mdkGobSetPosition(qaapple,"l1_r8doganland1")'
done
lua 1 'mdkGobSetPositionXYZ(qaapple,mdkGetPlayerGob().position.x,mdkGetPlayerGob().position.y,mdkGetPlayerGob().position.z)'
for i in 1 2; do check "mixed hero $i receives 25 hp from the host's apple" waitq "$i" 'coop_hp(mdkGetPlayerGob())-qa_hurt_hp' 25; done
# Max's drumstick is a different native health type, also shared with Doc.
lua 1 'coop.SwitchChar(mdkGetPlayerGob(),OBJ_MAX)'
check 'host changed to Max' waitq 1 'mdkGetGobType(mdkGetPlayerGob()) == OBJ_MAX and 1 or 0' 1
for i in 1 2; do
    lua "$i" 'God(0) mdkDealDamage(nil,mdkGetPlayerGob(),20,2,-1) qa_hurt_hp=coop_hp(mdkGetPlayerGob()) qadrum=mdkCreateObjectLua("qadrum",311,mdkGetScene(),nil,nil,0,0,0,1,0,0,0,nil,0,0,0,0) mdkGobSetPosition(qadrum,"l1_r8doganland1")'
done
lua 1 'mdkGobSetPositionXYZ(qadrum,mdkGetPlayerGob().position.x,mdkGetPlayerGob().position.y,mdkGetPlayerGob().position.z)'
for i in 1 2; do check "Max health pickup heals player $i by 25" waitq "$i" 'coop_hp(mdkGetPlayerGob())-qa_hurt_hp' 25; done
check 'no Lua errors on either game' bash -c "! grep -qa 'LUA ERROR' '$(mdk_log 1)' '$(mdk_log 2)'"
echo "logs: $out"
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
