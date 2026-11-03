"""Mutate process evidence to ensure AOE and cosmetic-only outcomes cannot pass."""
import copy
from pathlib import Path
import sys
import tempfile
import math
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
import presence_check as oracle


def sample(role, slot):
    return {'role': [[role]], 'me': [[str(slot)]], 'level': [['1']], 'life': [['0']],
            'ai_hook': [['1']], 'join': [['3']], 'join_screen': [['0']],
            'melee_only': [['1']], 'direct_receipts': [['0']], 'direct_source': [['none']],
            'player': [['100','100','-2000' if slot == 1 else '50','0','0']]}


class PresenceOracles(unittest.TestCase):
    def fixture(self):
        bh, bc = sample('host', 1), sample('client', 2)
        host, client = copy.deepcopy(bh), copy.deepcopy(bc)
        bh['ent'] = [['123','202','100','114','0','0','qa_melee']]
        host['ai'] = [['qa_melee','@p2','10','1']]
        host['ent'] = [['123','202','100','51','0','0','qa_melee']]
        client['direct_receipts'] = [['1']]
        client['direct_source'] = [['n123']]
        client['player'][0][1] = '99'
        return bh, bc, host, client

    def test_direct_baseline_and_mutants(self):
        oracle.ai(*self.fixture())
        mutations = [
            lambda h,c: h.pop('ai'),
            lambda h,c: h['ai'][0].__setitem__(1,'@p1'),
            lambda h,c: h['ai'][0].__setitem__(3,'0'),
            lambda h,c: h['melee_only'][0].__setitem__(0,'0'),
            lambda h,c: h['player'][0].__setitem__(2,'50'),
            lambda h,c: h['player'][0].__setitem__(1,'99'),
            lambda h,c: c['player'][0].__setitem__(1,'100'),
            lambda h,c: c['direct_receipts'][0].__setitem__(0,'0'),
            lambda h,c: c['direct_source'][0].__setitem__(0,'grenade'),
            lambda h,c: h['ent'][0].__setitem__(3,'0'),
            lambda h,c: c['level'][0].__setitem__(0,'2'),
        ]
        for mutation in mutations:
            bh,bc,h,c = self.fixture()
            mutation(h,c)
            with self.assertRaises((AssertionError, KeyError)):
                oracle.ai(bh,bc,h,c)

    def test_direct_damage_without_approach_fails(self):
        bh,bc,h,c = self.fixture()
        bh['ent'][0][3] = '54'
        h['ent'][0][3] = '54'
        with self.assertRaises(AssertionError): oracle.ai(bh,bc,h,c)

    def test_other_player_beside_the_target_fails_whatever_else_holds(self):
        # The old failure mode: the host stands next to the client, so area damage aimed at the host
        # would hurt the client. Every other assertion of the fixture passes; the geometry rule must not.
        for stage in ('before', 'after'):
            for distance in ('51', '150', '299'):
                bh,bc,h,c = self.fixture()
                (bh if stage == 'before' else h)['player'][0][2] = distance
                (bc if stage == 'before' else c)['player'][0][2] = '0'
                with self.assertRaisesRegex(AssertionError, 'units from target'):
                    oracle.ai(bh,bc,h,c)
        bh,bc,h,c = self.fixture()
        oracle.ai(bh,bc,h,c)

    def test_host_near_control(self):
        bh,bc,h,c = self.fixture()
        for a,b in ((bh,bc),(h,c)):
            for key in ('player','direct_receipts','direct_source'):
                a[key],b[key] = b[key],a[key]
        h['ai'][0][1] = '@p1'
        oracle.ai(bh,bc,h,c,host_near=True)
        h['ai'][0][1] = '@p2'
        with self.assertRaises(AssertionError): oracle.ai(bh,bc,h,c,host_near=True)

    def test_canopy_process_memory_not_just_body_pose(self):
        host, client = sample('host',1), sample('client',2)
        for d in host,client: d['canopy'] = [['2','2','66','67','1','.8','0','0']]
        oracle.canopy([host,client],2,66)
        host['canopy'][0][2] = '67'
        with self.assertRaises(AssertionError): oracle.canopy([host,client],2,66)
        host.pop('canopy')
        with self.assertRaises(AssertionError): oracle.canopy([host,client],2,66)

    def test_gear_is_read_from_every_process(self):
        host, client = sample('host',1), sample('client',2)
        # the item on the pose of the bone the owner's game puts it on (MAX_TORSO, slot 19), read on each body
        pose = lambda at, q, ref=None: [['1','maxjetpack','19', *map(str, at), *map(str, q), '19',
                                         *map(str, ref or at), *map(str, q)]]
        host['gearpose'] = pose((839, 950.1, 235.9), (1, 0, 0, 0))
        client['gearpose'] = pose((1343, 899.1, 250.8), (1, 0, 0, 0))
        for d in host,client: d['gear'] = [['1','maxjetpack','1','0','1','19','77','62','.5','1','3/0','5000']]
        oracle.gear([host,client],1,'maxjetpack',77)
        # turned a quarter round, still on the bone: the same place
        h = math.sqrt(.5)
        client['gearpose'] = pose((1343, 899.1, 250.8), (h, 0, 0, h))
        oracle.gear([host,client],1,'maxjetpack',77)
        client['gearpose'] = pose((1343, 899.1, 250.8), (1, 0, 0, 0), (1343, 899.1, 250.73))   # on another bone
        with self.assertRaisesRegex(AssertionError, 'units'): oracle.gear([host,client],1,'maxjetpack',77)
        client['gearpose'] = pose((1343, 899.1, 250.8), (1, 0, 0, 0))
        client['gearpose'][0][7:11] = ['0.9659', '0', '0.2588', '0']                             # turned 30 degrees on it
        with self.assertRaisesRegex(AssertionError, 'degrees'): oracle.gear([host,client],1,'maxjetpack',77)
        client['gearpose'] = pose((1343, 899.1, 250.8), (1, 0, 0, 0))
        client['gearpose'][0][10] = '-1'
        with self.assertRaisesRegex(AssertionError, 'not found'): oracle.gear([host,client],1,'maxjetpack',77)
        client['gearpose'] = pose((1343, 899.1, 250.8), (1, 0, 0, 0))
        client['gear'][0][10] = '3/3'                                       # the owner's flame shown, the puppet's not
        with self.assertRaisesRegex(AssertionError, 'sub-gobs'): oracle.gear([host,client],1,'maxjetpack',77)
        client['gear'][0][10] = '3/0'
        client['gear'][0][11] = '5250'                                      # the flame came on 250 ms after the owner's
        oracle.gear([host,client],1,'maxjetpack',77,lag=300)
        client['gear'][0][11] = '5600'                                      # ... or 600 ms after: late
        with self.assertRaisesRegex(AssertionError, 'ms after'): oracle.gear([host,client],1,'maxjetpack',77,lag=300)
        oracle.gear([host,client],1,'maxjetpack',77)                       # without a limit only the bits count
        client['gear'][0][11] = '5000'
        client.pop('gearpose')
        with self.assertRaisesRegex(AssertionError, 'pose row'): oracle.gear([host,client],1,'maxjetpack',77)
        client['gearpose'] = pose((1343, 899.1, 250.8), (1, 0, 0, 0))
        oracle.gear([host,client],1,'maxjetpack')
        client['gear'][0][6] = '62'                                         # the puppet still shows the idle flame
        with self.assertRaisesRegex(AssertionError, 'animation'): oracle.gear([host,client],1,'maxjetpack',77)
        client['gear'][0][6] = '77'
        for column, text in ((2,'not alive'),(4,'not attached'),(3,'hidden')):
            old = client['gear'][0][column]; client['gear'][0][column] = '0' if column != 3 else '1'
            with self.assertRaisesRegex(AssertionError, text): oracle.gear([host,client],1,'maxjetpack',77)
            client['gear'][0][column] = old
        client['gear'][0][5] = '20'
        with self.assertRaisesRegex(AssertionError, 'bones'): oracle.gear([host,client],1,'maxjetpack',77)
        client['gear'][0][5] = '19'
        client['gear'] = []                                                 # a puppet that never got one
        with self.assertRaisesRegex(AssertionError, 'missing'): oracle.gear([host,client],1,'maxjetpack',77)
        with self.assertRaisesRegex(AssertionError, 'still exists'): oracle.gear([host,client],1,'maxjetpack',absent=True)
        host['gear'] = []
        oracle.gear([host,client],1,'maxjetpack',absent=True)
        host['gear'] = client['gear'] = [['1','chute','1','0','1','2']]
        oracle.gear([host,client],1,'chute')
        client['gear'] = [['1','chute','1','1','1','2']]
        with self.assertRaisesRegex(AssertionError, 'hidden'): oracle.gear([host,client],1,'chute')

    def test_wait_requires_actual_menu_and_title(self):
        d=sample('client',2)
        d['join']=[['1']]; d['level']=[['0']]; d['join_screen']=[['1']]
        oracle.join(d,1)
        d['join_screen']=[['0']]
        with self.assertRaises(AssertionError): oracle.join(d,1)

    def test_missing_and_duplicate_state_fail(self):
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)/'state.txt'
            path.write_text('role host\n')
            with self.assertRaises(ValueError): oracle.load(path)
            values=sample('host',1)
            path.write_text('\n'.join(k+' '+' '.join(v) for k,rows in values.items() for v in rows)+'\nrole host\n')
            with self.assertRaises(ValueError): oracle.load(path)

    def test_load_accepts_hex_engine_handles(self):
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)/'state.txt'
            values=sample('host',1)
            values['ent']=[['1004a','200','600','334.95','-150.91','-167.64','l1_r5bif01']]
            path.write_text('\n'.join(k+' '+' '.join(v) for k,rows in values.items() for v in rows)+'\n')
            self.assertEqual(oracle.load(path)['ent'], values['ent'])
