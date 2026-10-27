"""Mutate process evidence to ensure AOE and cosmetic-only outcomes cannot pass."""
import copy
from pathlib import Path
import sys
import tempfile
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
import presence_check as oracle


def sample(role, slot):
    return {'role': [[role]], 'me': [[str(slot)]], 'level': [['1']], 'life': [['0']],
            'ai_hook': [['1']], 'join': [['3']], 'join_screen': [['0']],
            'melee_only': [['1']], 'direct_receipts': [['0']], 'direct_source': [['none']],
            'player': [['100','100','0' if slot == 1 else '50','0','0']]}


class PresenceOracles(unittest.TestCase):
    def fixture(self):
        bh, bc = sample('host', 1), sample('client', 2)
        host, client = copy.deepcopy(bh), copy.deepcopy(bc)
        host['ai'] = [['qa_melee','@p2','10','1']]
        host['ent'] = [['123','202','100','51','0','0','qa_melee']]
        client['direct_receipts'] = [['1']]
        client['direct_source'] = [['qa_melee']]
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

    def test_canopy_process_memory_not_just_body_pose(self):
        host, client = sample('host',1), sample('client',2)
        for d in host,client: d['canopy'] = [['2','2','66','67','1','.8','0','0']]
        oracle.canopy([host,client],2,66)
        host['canopy'][0][2] = '67'
        with self.assertRaises(AssertionError): oracle.canopy([host,client],2,66)
        host.pop('canopy')
        with self.assertRaises(AssertionError): oracle.canopy([host,client],2,66)

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
