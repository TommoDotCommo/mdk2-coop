from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

TOOL = Path(__file__).resolve().parents[1] / 'tools' / 'flyers_check.py'

# Level 2 room 4 mantas as the campaign survey's end dumps had them (the drift this checker was written for)
HOST = [(144.75, 1004.15, 41.88), (103.54, 899.54, 47.12)]
DRIFTED = [(435.48, 968.76, 48.26), (309.71, 986.08, 17.23)]


def dump(manta, turret=(10.0, 20.0, 30.0)):
    return (f'role x\nent 10039 230 66 {manta[0]:.2f} {manta[1]:.2f} {manta[2]:.2f} l2_r4manta00\n'
            f'ent 10041 230 50 {turret[0]:.2f} {turret[1]:.2f} {turret[2]:.2f} l2r3turret01\n'
            'ent 10061 217 25 1.00 2.00 3.00 l2_rleftpoop\n')


class FlyersCheck(unittest.TestCase):
    def run_pairs(self, pairs, *args):
        with tempfile.TemporaryDirectory() as d:
            files = []
            for i, (host, client) in enumerate(pairs):
                for side, text in (('h', host), ('c', client)):
                    p = Path(d) / f'{i}{side}.txt'
                    p.write_text(text)
                    files.append(str(p))
            return subprocess.run([sys.executable, str(TOOL), *args, *files], capture_output=True, text=True)

    def test_drift_fails(self):
        r = self.run_pairs([(dump(h), dump(c)) for h, c in zip(HOST, DRIFTED)])
        self.assertEqual(r.returncode, 1, r.stdout)
        self.assertIn('FAIL  l2_r4manta00', r.stdout)

    def test_close_follow_passes(self):
        near = [(x + 3, y - 2, z) for x, y, z in HOST]
        r = self.run_pairs([(dump(h), dump(c)) for h, c in zip(HOST, near)])
        self.assertEqual(r.returncode, 0, r.stdout)

    def test_flyers_that_never_moved_prove_nothing(self):
        r = self.run_pairs([(dump(HOST[0]), dump(HOST[0]))] * 3)
        self.assertEqual(r.returncode, 1, r.stdout)

    def test_fixed_turret_apart_fails_too(self):
        r = self.run_pairs([(dump(h), dump(h, turret=(90.0, 20.0, 30.0))) for h in HOST])
        self.assertEqual(r.returncode, 1, r.stdout)

    def test_odd_dump_count_is_an_error(self):
        r = self.run_pairs([(dump(HOST[0]), dump(HOST[0]))], '--limit', '5')
        self.assertEqual(r.returncode, 1)
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / 'one.txt'
            p.write_text(dump(HOST[0]))
            self.assertEqual(subprocess.run([sys.executable, str(TOOL), str(p)], capture_output=True).returncode, 2)


if __name__ == '__main__':
    unittest.main()
