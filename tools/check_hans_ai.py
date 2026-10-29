#!/usr/bin/env python3
"""Exercise the installed game's authored Hans task and native projectile flight."""
import argparse
import math
import os
import time
from check_room_ai import RoomCheck

# The host waits in level one's first room, about 1400 units from the arena (rooms 1 and 8 are the
# level's two ends). The arena marker used by the room-AI test is inside the arena here.
HOST_ROOM = 'l1_r1grengen'


def run(dirs, output):
    q = RoomCheck(dirs, output)
    # Skip the ship movie/pilot phases, retaining the game's own final spawn
    # sequence, attack task, timers and native animation callbacks.
    for player in (1, 2):
        q.command(player, 'God(1) coop.save_interval=999999 StopScript(l1_r8) l1_r8.entered=1 l1_r8.bossstarted=1')
    q.command(1, '''
        qa_hans_task=Level.r8HansAttack qa_remote=0 qa_other=0
        Level.r8HansAttack=function(g)
            if (mdkGetPlayerGob()==coop_gob("@p2") and mdkGetAITargetGob()==coop_gob("@p2")) then
                qa_remote=qa_remote+1
            else qa_other=qa_other+1 end
            return qa_hans_task(g)
        end
        l1_r8.BossFlag=1 hanspilot.state=3
        hanspilot.OnDamage(hanspilot,mdkGetPlayerGob(),1,1,-1)
    ''')
    q.wait('authored final Hans spawn exists',
           lambda h,c: any(r[0]=='ent' and r[-1]=='hans' for r in h), seconds=20)
    # Geometry (docs/TESTING.md, tools/targeting_geometry.py): the client stands in the arena, Hans
    # spawns about 250 units from it, and the host waits in another room more than a thousand units
    # away, alive. Nothing aimed at the host can splash onto the client, and Hans can only aim at
    # the client by choosing the nearest player.
    near = os.environ.get('MUTANT') == 'near'
    q.command(2, 'mdkGobSetPosition(mdkGetPlayerGob(),"l1_r8lowcenter") local p=mdkGetPlayerGob() mdkGobSetPositionXYZ(p,p.position.x+35,p.position.y,p.position.z)')
    if near:
        q.command(1, 'mdkGobSetPosition(mdkGetPlayerGob(),"l1_r8wp")')   # the old placement: 60 units from the client
    else:
        q.command(1, f'mdkGobSetPosition(mdkGetPlayerGob(),"{HOST_ROOM}")')
    q.far('Hans: client is far from the host')
    mutant = os.environ.get('MUTANT', '')
    if mutant == 'script-scope':
        q.command(1, 'ScriptUpdate=coop.scriptupdate_orig')
    elif mutant == 'targeting':
        q.command(1, 'coop_presence_test(0,1,0,0)')
    elif mutant == 'hostaoe':
        # Direct actor damage is not routed to the remote player: only damage landing on the host or
        # on the host's splash could remain, and the host is a thousand units away.
        q.command(1, 'coop_presence_test(0,0,1,0)')
    elif mutant and mutant != 'near':
        raise ValueError('supported MUTANT: script-scope, targeting, hostaoe, near')
    q.command(1, 'qa_remote=0 qa_other=0')
    # Read probe values through native gob positions in the existing atomic
    # state dump. The markers stay in stasis and cannot grant pickups.
    for player in (1, 2):
        q.command(player, 'qa_probe=mdkCreateObjectLua("qa_hans_probe",OBJ_GRENADEITEM,mdkGetScene(),nil,nil,0,0,0,1,0,0,0,nil,0,0,0,0) omGobEnterStasis(qa_probe) qa_serial=0')
    q.command(2, 'qa_stamp=mdkCreateObjectLua("qa_hans_stamp",OBJ_GRENADEITEM,mdkGetScene(),nil,nil,0,0,0,1,0,0,0,nil,0,0,0,0) omGobEnterStasis(qa_stamp)')
    def probe(player):
        if player == 1:
            q.command(1, 'mdkGobSetPositionXYZ(qa_probe,qa_remote,qa_other,0)')
        else:
            q.command(2, 'local s=coop_test_shot(421) if (s) then if (s~=qa_last_shot) then qa_serial=qa_serial+1 qa_last_shot=s end mdkGobSetPositionXYZ(qa_probe,s.position.x,s.position.y,s.position.z) else mdkGobSetPositionXYZ(qa_probe,0,0,0) end mdkGobSetPositionXYZ(qa_stamp,qa_serial,0,0)')
        rows = q.state(player)
        marker = next(r for r in rows if r[0]=='item' and r[-1]=='qa_hans_probe')
        values = [float(v) for v in marker[2:5]]
        if player == 1:
            return values[:2]
        if values == [0,0,0]:
            return []
        body = next(r for r in rows if r[0]=='player')
        serial = next(float(r[2]) for r in rows if r[0]=="item" and r[-1]=="qa_hans_stamp")
        return values + [float(v) for v in body[3:6]] + [serial]
    deadline = time.monotonic()+20
    previous = None
    flight = False
    while time.monotonic()<deadline:
        sample = probe(2)
        if sample and previous and sample[6] == previous[6]:
            delta = [sample[i]-previous[i] for i in range(3)]
            toward = [sample[i+3]-previous[i] for i in range(3)]
            length = math.sqrt(sum(v*v for v in delta))
            distance = math.sqrt(sum(v*v for v in toward))
            if .2 < length < 25 and distance > 2:
                cosine = sum(a*b for a,b in zip(delta,toward))/(length*distance)
                if cosine > .9:
                    (q.out/'flight.txt').write_text(f'{previous}\n{sample}\ncosine {cosine}\n')
                    flight = True
                    break
        previous = sample or None
        time.sleep(.08)
    assert flight, 'no native Hans projectile traveled toward remote player'
    print('PASS native Hans projectile actually travels toward the remote player', flush=True)
    counts = probe(1)
    (q.out/'task.txt').write_text(str(counts)+'\n')
    assert counts[0]>0 and counts[1]==0, f'authored task player/AI getters: {counts}'
    print('PASS authored task sees remote player in both native getters', flush=True)
    host = q.state(1)
    boss = next(r[1] for r in host if r[0]=='ent' and r[-1]=='hans')
    assert any(r[0:3]==['ai','hans','@p2'] and int(r[3])>0 for r in host), 'no native Hans target query'
    q.command(2, 'God(0)')
    q.wait('native Hans attack hurts the remote player',
           lambda h,c: q.hp(c)<100 and ['direct_source','n'+boss] in c and any(r[0]=='ai_fire' and r[2:4]==['204','2'] for r in h), seconds=12)
    # The host, alive in another room (kept invulnerable: that room's own enemies would otherwise
    # make its health meaningless), was never a target of Hans, and the client is still far from it.
    host = q.state(1)
    assert not any(r[0]=='ai_fire' and r[2]=='204' and r[3]=='1' and int(r[4])>0 for r in host), 'Hans fired at the host'
    q.far('Hans: client still far from the host after the attack')
    q.wait('target scopes restore host handle and stack',
           lambda h,c: ['ai_scope_depth','0'] in h and ['engine_player','@p1'] in h)
    print(f'{q.passed+2} passed, 0 failed', flush=True)


if __name__ == '__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--dirs', nargs=2, required=True)
    p.add_argument('--output', required=True)
    a=p.parse_args()
    run(a.dirs,a.output)
