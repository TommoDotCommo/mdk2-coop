#!/usr/bin/env bash
# Deterministic projectile checks with real games, including type 417's separate launch.
# Uses the same launcher as the Doganboy (mdkShootBullet -> 0x403b00), without waiting
# for its AI to choose grenades. MDK2_N=3 also checks client -> host -> other client.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
source "$here/mdk.sh"
[ "$MDK2_N" -ge 2 ] || { echo 'qa_shots requires at least two games' >&2; exit 1; }
MDK2_BACKGROUND=1 MDK2_START="1 2" "$here/install.sh" >/dev/null
out="$here/../local/qa-shots/$(date +%Y%m%d-%H%M%S)"
mkdir -p "$out"
cleanup() {
    local i
    for i in $(mdk_seq); do cp "$(mdk_log "$i")" "$out/game$i.log" 2>/dev/null || true; done
    mdk_kill
}
trap cleanup EXIT
mdk_boot

wait_log() {
    local i
    for i in $(seq 1 100); do
        if grep -qE "$2" "$(mdk_log "$1")"; then return 0; fi
        sleep 0.1
    done
    echo "FAIL: game $1 missing $2" >&2
    return 1
}

for i in $(mdk_seq); do
    wait_log "$i" '^level 1 checkpoint 2 started'
    wait_log "$i" '^launch_shot hook installed'
done

for source in 1 2; do
    tag="p${source}_417"
    replay_before=()
    # Only the source may run shooter scripts. Peers expose the actual foreign gob
    # through a background-only helper for flight and damage assertions.
    for i in $(mdk_seq); do
        replay_before[$i]=$(grep -cE '^replay fire type 417 .*created' "$(mdk_log "$i")" || true)
        mdk_cmd "$(mdk_pid "$i")" "qa_seen=nil; qa_owner=coop_gob(\"@p$source\"); mdkSetLuaEvent(qa_owner,\"OnSpawn\",function(owner,shot) if (mdkGetGobType(shot)==417) then qa_seen=shot; coop_log(\"qa: $tag native launch\") end end)"
    done
    mdk_cmd "$(mdk_pid "$source")" 'local p=mdkGetPlayerGob(); qa_grenade=mdkCreateObjectLua("qa_grenade",417,mdkGetScene(),nil,nil,p.position.x,p.position.y,p.position.z+3,1,0,0,0,nil,0,0,0,0)'
    # No event until launched (the unit test additionally covers NULL pos/quat and attachment).
    sleep 0.5
    for i in $(mdk_seq); do
        if grep -q "^lua: qa: $tag native launch" "$(mdk_log "$i")"; then
            echo "FAIL: game $i launched a grenade at creation" >&2
            exit 1
        fi
    done
    mdk_cmd "$(mdk_pid "$source")" 'mdkShootBulletLua(qa_grenade,mdkGetPlayerGob(),nil,0,0,1)'
    wait_log "$source" "^lua: qa: $tag native launch"
    for i in $(mdk_seq); do
        if [ "$i" != "$source" ]; then
            for n in $(seq 1 100); do
                current=$(grep -cE '^replay fire type 417 .*created' "$(mdk_log "$i")" || true)
                [ "$current" -gt "${replay_before[$i]}" ] && break
                sleep 0.1
            done
            test "$current" -eq "$((replay_before[$i] + 1))"
            mdk_cmd "$(mdk_pid "$i")" 'qa_seen=coop_test_shot(417)'
        fi
        mdk_cmd "$(mdk_pid "$i")" "if (coop_alive(qa_seen)) then coop_log(\"qa: $tag captured\") end"
        wait_log "$i" "^lua: qa: $tag captured"
    done
    for sample in 0 1; do
        for i in $(mdk_seq); do
            mdk_cmd "$(mdk_pid "$i")" "if (coop_alive(qa_seen)) then coop_log(format(\"qa: $tag sample $sample %.3f %.3f %.3f\",qa_seen.position.x,qa_seen.position.y,qa_seen.position.z)) end"
        done
        sleep 0.5
    done
    # A replay may never deal damage locally; the real hit travels through HIT/HURT/APPLY.
    for i in $(mdk_seq); do
        if [ "$i" != "$source" ]; then
            mdk_cmd "$(mdk_pid "$i")" "local p=mdkGetPlayerGob(); local hp=coop_hp(p); mdkDealDamage(qa_seen,p,1,2,-1); if (coop_hp(p)==hp) then coop_log(\"qa: $tag foreign damage suppressed\") end"
            wait_log "$i" "^lua: qa: $tag foreign damage suppressed"
        fi
        # Replays initialise once. Counting callbacks catches duplicate launches as well.
        test "$(grep -c "^lua: qa: $tag captured" "$(mdk_log "$i")")" -eq 1
        expected=0; [ "$i" != "$source" ] || expected=1
        test "$(grep -c "^lua: qa: $tag native launch" "$(mdk_log "$i")" || true)" -eq "$expected"
        mdk_cmd "$(mdk_pid "$i")" 'mdkSetLuaEvent(qa_owner,"OnSpawn",nil)'
    done
done

for i in $(mdk_seq); do cp "$(mdk_log "$i")" "$out/game$i.log"; done
python3 - "$out" "$MDK2_N" <<'PY'
import math
import pathlib
import re
import sys

out = pathlib.Path(sys.argv[1])
for i in range(1, int(sys.argv[2]) + 1):
    log = (out / f"game{i}.log").read_text()
    assert "LUA ERROR" not in log, f"game {i}: Lua error"
    for source in (1, 2):
        tag = f"p{source}_417"
        samples = re.findall(rf"^lua: qa: {tag} sample ([01]) ([-\d.]+) ([-\d.]+) ([-\d.]+)", log, re.M)
        assert [s[0] for s in samples] == ["0", "1"], f"game {i}, {tag}: missing flight samples"
        a, b = [tuple(map(float, s[1:])) for s in samples]
        assert math.dist(a, b) > 0.05, f"game {i}, {tag}: grenade is stationary"
        print(f"PASS: game {i}, {tag}: one launch, grenade flies" + (", no replay damage" if i != source else ""))
print(f"Projectile checks passed for {sys.argv[2]} games")
PY
echo "logs: $out"
