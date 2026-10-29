#!/usr/bin/env python3
"""A game started at a checkpoint past the jetpack gives every player the jetpack, natively collected."""
import argparse
import os
import time
from check_room_ai import RoomCheck


def counters(q, player):
    """(collected checkpoint items, rewards still waiting, checkpoint) from the game's own Lua state,
    carried out through a probe gob's position in the state dump."""
    q.command(player, 'if (not qa_cpi or coop_alive(qa_cpi) ~= 1) then qa_cpi=mdkCreateObjectLua("qa_cpitems",OBJ_GRENADEITEM,mdkGetScene(),nil,nil,0,0,0,1,0,0,0,nil,0,0,0,0) omGobEnterStasis(qa_cpi) end '
                      'local n=0 local k=next(coop.rewards,nil) while (k) do n=n+1 k=next(coop.rewards,k) end '
                      'mdkGobSetPositionXYZ(qa_cpi,coop.cp_given or 0,n,coop.cp or -1)')
    row = next(r for r in q.state(player) if r[0] == 'item' and r[-1] == 'qa_cpitems')
    return tuple(int(round(float(v))) for v in row[2:5])


def wait_counts(q, label, want, cp, seconds=30):
    deadline = time.monotonic() + seconds
    while True:
        got = [counters(q, p) for p in (1, 2)]
        if all(g == (want, 0, cp) for g in got):
            q.passed += 1
            print(f'PASS {label}', flush=True)
            return
        if time.monotonic() > deadline:
            raise AssertionError(f'{label}: (given, waiting, checkpoint) per game {got}, want {(want, 0, cp)}')
        time.sleep(.5)


def run(dirs, output):
    q = RoomCheck(dirs, output)
    mutant = os.environ.get('MUTANT', '')
    if mutant not in ('', 'nogrant'):
        raise ValueError('supported MUTANT: nogrant')
    if mutant == 'nogrant':
        # the item table does nothing: a restart of the same checkpoint then has no jetpack
        for player in (1, 2):
            q.command(player, 'coop.CheckpointItems=function(l, cp) end')
        q.command(1, 'mdkNewGame(2,8)')
        time.sleep(20)
    for player in (1, 2):
        q.command(player, 'God(1) coop.save_interval=999999')
    wait_counts(q, 'level 2 checkpoint 8 (past room 5): each player collected the jetpack', 1, 8)
    # Checkpoint 5 starts in the jetpack's room, with the pickup still there: nothing is given.
    q.command(1, 'mdkNewGame(2,5)')
    time.sleep(20)
    wait_counts(q, 'level 2 checkpoint 5 (jetpack still in the room): nothing given', 0, 5)
    print(f'{q.passed} passed, 0 failed', flush=True)


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--dirs', nargs=2, required=True)
    p.add_argument('--output', required=True)
    a = p.parse_args()
    run(a.dirs, a.output)
