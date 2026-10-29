#!/usr/bin/env python3
"""The level-one boss's ring turrets fire at, and hurt, a client far from the host."""
import argparse
import os
from check_room_ai import RoomCheck

# turret01_xx / turret02_xx are OBJ_SCENERY guns on l1_r8outerguns / l1_r8innerguns. They fire from
# their ANIM_SHOOTING animation at the engine's player, so before the fix every shot went to the host.
HOST_ROOM = 'l1_r1grengen'


def turret_shots(rows, player):
    return sum(int(r[4]) for r in rows
               if r[0] == 'ai_fire' and int(r[3]) == player and len(r) > 5 and r[5].startswith('turret0'))


def run(dirs, output):
    q = RoomCheck(dirs, output)
    mutant = os.environ.get('MUTANT', '')
    if mutant not in ('', 'targeting', 'hostaoe', 'near'):
        raise ValueError('supported MUTANT: targeting, hostaoe, near')
    for player in (1, 2):
        q.command(player, 'God(1) coop.save_interval=999999 StopScript(l1_r8) l1_r8.entered=1 l1_r8.bossstarted=1')
    # Geometry (docs/TESTING.md, tools/targeting_geometry.py): the client stands in the arena inside the
    # ring of turrets; the host waits in level one's first room, more than a thousand units away.
    q.command(2, 'mdkGobSetPosition(mdkGetPlayerGob(),"l1_r8lowcenter")')
    if mutant == 'near':
        q.command(1, 'mdkGobSetPosition(mdkGetPlayerGob(),"l1_r8wp")')
    else:
        q.command(1, f'mdkGobSetPosition(mdkGetPlayerGob(),"{HOST_ROOM}")')
    q.far('ring turrets: client is far from the host')
    if mutant == 'targeting':
        q.command(1, 'coop_presence_test(0,1,0,0)')
    elif mutant == 'hostaoe':
        # Damage the turrets deal directly to the client is dropped: only what lands on the host
        # (a thousand units away) could remain.
        q.command(1, 'coop_presence_test(0,0,1,0)')
    # The authored phase 2 start: all four sniper locks done.
    q.command(1, 'l1_r8esnipe01.LockOn=1 l1_r8esnipe02.LockOn=1 l1_r8esnipe03.LockOn=1 '
                 'l1_r8esnipe04.LockOn=1 l1_r8.SnipeLock(1)')
    host = q.state(1)
    before = turret_shots(host, 1), turret_shots(host, 2)
    q.wait('ring turrets fire at the client',
           lambda h, c: turret_shots(h, 2) - before[1] >= 4, seconds=40)
    h = q.state(1)
    at_host = turret_shots(h, 1) - before[0]
    (q.out/'shots.txt').write_text(f'host {at_host} client {turret_shots(h, 2) - before[1]}\n')
    assert at_host == 0, f'{at_host} turret shots went to the far host'
    print('PASS no turret shot went to the far host', flush=True)
    q.command(2, 'God(0)')
    q.wait('a ring turret hurts the client',
           lambda h, c: q.hp(c) < 100 and any(r[0] == 'direct_source' and r[1].startswith('turret0') for r in c),
           seconds=30)
    q.far('ring turrets: client still far from the host after the hits')
    print(f'{q.passed + 1} passed, 0 failed', flush=True)


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--dirs', nargs=2, required=True)
    p.add_argument('--output', required=True)
    a = p.parse_args()
    run(a.dirs, a.output)
