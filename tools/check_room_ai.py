#!/usr/bin/env python3
"""Process-state regression for native level-one ranged enemies and the generator."""
import argparse
import os
from pathlib import Path
import time
import targeting_geometry

# Where the host waits while the client is the target: another room of the level, more than a
# thousand units from every place used below (tools/targeting_geometry.py holds the rule).
FAR_MARKER = 'l1_r8doganland1'


class RoomCheck:
    def __init__(self, dirs, output):
        self.dirs = [Path(d)/'coop' for d in dirs]
        self.out = Path(output)
        self.out.mkdir(parents=True, exist_ok=True)
        self.serial = 0
        self.passed = 0

    def command(self, player, code):
        target = self.dirs[player-1]/'command.lua'
        stage = target.with_suffix('.tmp')
        stage.write_text(code+'\n')
        stage.replace(target)
        deadline = time.monotonic()+12
        while target.exists():
            assert time.monotonic()<deadline, f'player {player} command timeout'
            time.sleep(.05)

    def state(self, player):
        self.serial += 1
        tag = f'room_ai_{self.serial}_{player}'
        self.command(player, f'coop_state("{tag}")')
        p = self.dirs[player-1]/f'state_{tag}.txt'
        deadline = time.monotonic()+5
        while not p.exists():
            assert time.monotonic()<deadline, 'state timeout'
            time.sleep(.05)
        raw=p.read_text()
        (self.out/f'{tag}.txt').write_text(raw)
        return [line.split() for line in raw.splitlines()]

    def wait(self, label, predicate, seconds=25):
        deadline=time.monotonic()+seconds
        while True:
            h,c=self.state(1),self.state(2)
            if predicate(h,c):
                self.passed+=1
                print('PASS '+label,flush=True)
                return h,c
            if time.monotonic()>deadline:
                raise AssertionError(label+'; see final process snapshots')
            time.sleep(.5)

    def far(self, label, target=2):
        """Geometry rule: the target is outside every splash and strike radius from the other player,
        measured from both games' own player positions."""
        rows = [targeting_geometry.parse_rows(self.state(p)) for p in (1, 2)]
        distance = targeting_geometry.assert_separation(rows, target, label=label)
        self.passed += 1
        (self.out/'geometry.txt').open('a').write(f'{label}: {distance:.1f}\n')
        print(f'PASS {label}: players are {distance:.1f} units apart (rule {targeting_geometry.MIN_SEPARATION:g})', flush=True)

    def host_away(self, client_at):
        """Put the host where it cannot be the target. MUTANT=near puts it beside the client instead,
        which the geometry assertion must reject."""
        if os.environ.get('MUTANT') == 'near':
            x, y, z = client_at
            self.command(1, f'mdkGobSetPositionXYZ(mdkGetPlayerGob(),{x+2},{y},{z})')
        else:
            self.command(1, f'mdkGobSetPosition(mdkGetPlayerGob(),"{FAR_MARKER}")')

    @staticmethod
    def hp(rows):
        return next(int(r[2]) for r in rows if r[0]=='player')

    @staticmethod
    def entities(rows):
        return {int(r[1],16):r for r in rows if r[0]=='ent' and r[2]!='gone'}

    @staticmethod
    def fire(rows, kind, player):
        return sum(int(r[4]) for r in rows if r[0]=='ai_fire' and int(r[2])==kind and int(r[3])==player)

    def run(self):
        # No AI target reset, enemy alert or enemy creation. These are the level's
        # real generator and real enemies, activated by a player's room entry.
        self.command(1,'God(0) coop.save_interval=999999 mdkGobSetPosition(mdkGetPlayerGob(),"l1_r1grengen")')
        if os.environ.get('MUTANT') == 'targeting':
            self.command(1,'coop_presence_test(0,1,0,0)')
        elif os.environ.get('MUTANT') == 'hostaoe':
            self.command(1,'coop_presence_test(0,0,1,0)')
        self.command(2,'God(0) coop.save_interval=999999 mdkGobSetPositionXYZ(mdkGetPlayerGob(),180,65,-96)')
        h,c=self.wait('native generator exists and its visible animation is mirrored',
            lambda h,c: any(r[0]=='generator' and r in c for r in h))
        # The host started the generator by entering its room; it now leaves for another room, so
        # the round enemies' only possible target is the client and no splash aimed at the host can reach it.
        self.host_away((180,65,-96))
        self.far('round enemies: client is far from the host')
        self.wait('native round enemies fire at client and actually reduce client HP; host stays unharmed',
            lambda h,c:self.fire(h,217,2)>0 and self.hp(c)<100 and self.hp(h)==100,seconds=35)
        self.command(2,'God(1)')
        # Stop replacements, remove two same-named siblings, and hit the final
        # survivor from the client. Its event destination must remain its ID.
        self.command(1,'mdkSpawnerShutOff(r3dogangen)')
        h=self.state(1)
        spawned=[i for i,r in self.entities(h).items() if int(r[2])==217 and 100<float(r[4])<230 and float(r[6])<0]
        assert len(spawned)>=3, 'expected three native generator children'
        survivor=spawned[0]
        # Reliable spawn announcements may be repeated or arrive after the set
        # of same-named siblings changes. Reuse the binding, not a Lua global.
        for entity in spawned:
            r=self.entities(h)[entity]
            x,y,z=r[4:7]
            self.command(2,f'coop.Spawn("r3dogangen_spawn",{entity},217,{x},{y},{z},{r[3]})')
        self.wait('reannounced siblings retain their bindings without creating stray actors',
            lambda h,c: all(i in self.entities(c) for i in spawned) and
            not any(r[0]=='actor' and int(r[1])==217 and 100<float(r[3])<230 and float(r[5])<0 for r in c))
        for entity in spawned[1:]:
            self.command(1,f'omGobDelete(coop_gob("n{entity:x}"))')
        self.wait('only the surviving generator child remains on both peers',
            lambda h,c: survivor in self.entities(h) and survivor in self.entities(c) and
            all(i not in self.entities(h) and i not in self.entities(c) for i in spawned[1:]))
        before=int(self.entities(self.state(1))[survivor][3])
        self.command(2,f'mdkDealDamage(mdkGetPlayerGob(),coop_gob("n{survivor:x}"),10,1,-1)')
        self.wait('last same-named enemy takes client damage and has identical HP on both peers',
            lambda h,c: survivor in self.entities(h) and survivor in self.entities(c) and
            int(self.entities(h)[survivor][3])<before and self.entities(h)[survivor][3]==self.entities(c)[survivor][3])
        self.command(2,f'mdkDealDamage(mdkGetPlayerGob(),coop_gob("n{survivor:x}"),1000,1,-1)')
        self.wait('client can kill the survivor without leaving an unkillable mirror',
            lambda h,c: survivor not in self.entities(h) and survivor not in self.entities(c),seconds=15)
        # This authored tunnel has walkable floor beside its two turrets.
        # Wall-mounted turrets in the high sky area are not valid spawn points.
        h=self.state(1)
        before=self.fire(h,230,2)
        self.command(2,'mdkGobSetPositionXYZ(mdkGetPlayerGob(),272,-60,-137)')
        self.host_away((272,-60,-137))
        self.far('turrets: client is far from the host')
        self.wait('native turret launches at the nearby client',lambda h,c:self.fire(h,230,2)>before,seconds=30)
        # Sniping the actual sign enables the first behind-glass enemy through
        # its authored sequence. Its OnUpdate calls the native attack directly.
        self.command(2,'God(0) mdkGobSetPositionXYZ(mdkGetPlayerGob(),78,172,-30)')
        before=self.hp(self.state(2))
        dog=next(i for i,r in self.entities(self.state(1)).items() if r[-1]=='r2_dog01')
        for attempt in range(40):
            self.command(2,'if (r2_dog01target and coop_alive(r2_dog01target)) then local t=r2_dog01target local p=mdkGetPlayerGob() mdkGobPointToXYZ(p,t.position.x-p.position.x,t.position.y-p.position.y,t.position.z-p.position.z) coop_test_sniper() end')
            time.sleep(1)
            h=self.state(1)
            if any(r[0]=='ai' and r[1]=='r2_dog01' and int(r[3])>0 for r in h): break
        self.command(2,'mdkGobSetPositionXYZ(mdkGetPlayerGob(),78,156,-30)')
        self.host_away((78,156,-30))
        self.far('behind-glass enemy: client is far from the host')
        self.wait('authored behind-glass enemy directly damages client while host stays distant',
            lambda h,c: self.hp(c)<before and ['direct_source',f'n{dog:x}'] in c and self.hp(h)==100,seconds=30)
        print(f'{self.passed} passed, 0 failed',flush=True)

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--dirs',nargs=2,required=True)
    p.add_argument('--output',required=True)
    a=p.parse_args()
    if os.environ.get('MUTANT', '') not in ('', 'targeting', 'hostaoe', 'near'):
        p.error('supported MUTANT: targeting, hostaoe, near')
    RoomCheck(a.dirs,a.output).run()
