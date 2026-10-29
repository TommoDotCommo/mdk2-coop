"""The live damage oracle must fail when telemetry is absent or misattributed."""
import copy
from pathlib import Path
import sys
import tempfile
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import combatcheck


def fixture(hp=100, damage=0, slot=1):
    players = {p: (0, 0, 0., 0.) for p in range(1, 17)}
    players[2] = (damage, damage, damage / 5., float(damage))
    events = {(7, 1): (7, 1, 2, 42, 200, damage, 2049, 0, 1., 2., 3.)} if damage else {}
    return {'me': slot, 'role': 'host' if slot == 1 else 'client', 'epoch': 7, 'events': events, 'players': players, 'entities': {'enemy': (42, hp)},
            'meter': 1, 'size': len(events), 'scroll': 0, 'page': 0}


class CombatOracle(unittest.TestCase):
    def setUp(self):
        self.before = [fixture(slot=p) for p in range(1, 4)]
        self.after = [fixture(80, 20, slot=p) for p in range(1, 4)]

    def check_damage(self):
        return combatcheck.verify(self.before, self.after, 2, 'enemy', 20)

    def test_measured_damage_and_all_peers(self):
        self.assertIn('PASS', self.check_damage())

    def test_deaf_client_mutant(self):
        self.after[1] = fixture(80, slot=2)
        with self.assertRaises(ValueError):
            self.check_damage()

    def test_missing_event_on_every_peer(self):
        for state in self.after:
            state['events'] = {}
        with self.assertRaises(ValueError):
            self.check_damage()

    def test_missing_or_duplicate_participant(self):
        with self.assertRaises(ValueError):
            combatcheck.verify(self.before[:2], self.after)
        self.after[2]['me'] = 2
        with self.assertRaises(ValueError):
            self.check_damage()

    def test_wrong_source_mutant(self):
        with self.assertRaises(ValueError):
            combatcheck.verify(self.before, self.after, 1, 'enemy', 20)

    def test_inflated_amount_mutant(self):
        for state in self.after:
            state['players'][2] = (21, 21, 4.2, 21.)
        with self.assertRaises(ValueError):
            combatcheck.verify(self.before, self.after, 2, 'enemy', 20, tol=0)

    def test_dps_meter_values(self):
        self.assertIn('PASS', self.check_damage())
        self.after[2]['players'][2] = (20, 20, 0., 20.)
        with self.assertRaises(ValueError):
            self.check_damage()
        self.after = [fixture(80, 20, slot=p) for p in range(1, 4)]
        self.after[1]['players'][2] = (20, 20, 4., 0.)
        with self.assertRaises(ValueError):
            self.check_damage()
        self.after[1]['players'][2] = (20, 20, 4., 9.)
        with self.assertRaises(ValueError):
            self.check_damage()

    def test_wrong_target_hp_mutant(self):
        for state in self.after:
            state['entities']['enemy'] = (42, 90)
        with self.assertRaises(ValueError):
            self.check_damage()

    def test_idle_and_reset(self):
        idle = copy.deepcopy(self.after)
        for state in idle:
            state['players'][2] = (20, 20, 0., 20.)
        combatcheck.verify(self.after, idle, idle=True)
        reset = copy.deepcopy(idle)
        for state in reset:
            state['players'][2] = (0, 0, 0., 0.)
        combatcheck.verify(idle, reset, reset=True)
        with self.assertRaises(ValueError):
            combatcheck.verify(self.after, self.after, idle=True)
        with self.assertRaises(ValueError):
            combatcheck.verify(self.after, self.after, reset=True)
        with self.assertRaises(ValueError):
            combatcheck.verify(self.after, self.after, meter=0)

    def test_parser_rejects_incomplete_duplicate_and_nonfinite(self):
        header = 'me 1\nrole host\ncombat 7 1 1 0 0\n'
        players = ''.join(f'combat_player {p} 0 0 0 0\n' for p in range(1, 17))
        event = 'combat_event 7 1 2 42 200 20 2049 0 1 2 3\n'
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'state.txt'
            path.write_text(header + players + event)
            self.assertEqual(combatcheck.load(path)['size'], 1)
            for invalid in (header + event, header + players + event * 2,
                            header + players + event.replace('1 2 3\n', 'nan 2 3\n')):
                path.write_text(invalid)
                with self.assertRaises(ValueError):
                    combatcheck.load(path)


if __name__ == '__main__':
    unittest.main()
