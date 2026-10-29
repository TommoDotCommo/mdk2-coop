"""The shared geometry rule of targeting tests: unit tests, a source mutant and the scan."""
import io
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from contextlib import redirect_stdout

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
sys.path.insert(0, str(TOOLS))
import targeting_geometry as geo


def dump(role, slot, at, level=1, seen=()):
    text = f'role {role}\nlevel {level}\nme {slot}\nplayer 100 100 {at[0]:.2f} {at[1]:.2f} {at[2]:.2f}\n'
    for n, p in seen:
        text += f'puppet {n} {p[0]:.2f} {p[1]:.2f} {p[2]:.2f}\n'
    return geo.parse(text)


FAR = 1200.0
NEAR = 3.0


def pair(client_at=(FAR, 0.0, 0.0), seen_by_host=None, seen_by_client=None):
    host_at = (0.0, 0.0, 0.0)
    host = dump('host', 1, host_at, seen=[(2, seen_by_host or client_at)])
    client = dump('client', 2, client_at, seen=[(1, seen_by_client or host_at)])
    return [host, client]


class Rule(unittest.TestCase):
    def test_minimum_is_three_times_the_largest_radius(self):
        self.assertEqual(geo.MIN_SEPARATION, geo.AOE_FACTOR * max(r for r, _ in geo.SPLASH_RADII.values()))
        # the known direct reaches and script areas are all well inside it
        self.assertGreaterEqual(geo.MIN_SEPARATION, 3 * 50)

    def test_far_client_passes(self):
        self.assertAlmostEqual(geo.assert_separation(pair()), FAR)

    def test_client_beside_the_host_is_rejected(self):
        for at in ((NEAR, 0, 0), (0, 0, 40), (60, 60, 0), (geo.MIN_SEPARATION - 1, 0, 0)):
            with self.assertRaisesRegex(AssertionError, 'units from target'):
                geo.assert_separation(pair(at))

    def test_exactly_the_minimum_passes(self):
        geo.assert_separation(pair((geo.MIN_SEPARATION, 0, 0)))

    def test_weakened_minimum_is_rejected(self):
        with self.assertRaisesRegex(AssertionError, 'below the rule'):
            geo.assert_separation(pair(), minimum=100)

    def test_stricter_minimum_is_honoured(self):
        with self.assertRaises(AssertionError):
            geo.assert_separation(pair(), minimum=FAR + 1)

    def test_host_can_be_the_target(self):
        a, b = pair()
        self.assertAlmostEqual(geo.assert_separation([a, b], target=1), FAR)
        with self.assertRaises(AssertionError):
            geo.assert_separation(pair((NEAR, 0, 0)), target=1)

    def test_three_players_every_other_player_counts(self):
        h = dump('host', 1, (0, 0, 0))
        c2 = dump('client', 2, (FAR, 0, 0))
        c3 = dump('client', 3, (2, 0, 0))
        with self.assertRaises(AssertionError):
            geo.assert_separation([h, c2, c3], target=1)
        far3 = dump('client', 3, (-FAR, 5, 0))
        self.assertAlmostEqual(geo.assert_separation([h, c2, far3], target=1), FAR)

    def test_stale_view_of_another_player_is_rejected(self):
        # the host believes the client is far away, but the client's own game says it is next to the host
        with self.assertRaisesRegex(AssertionError, 'sees player 2'):
            geo.assert_separation(pair((NEAR, 0, 0), seen_by_host=(FAR, 0, 0)))
        # the client's own game is far away but the host's view of it is next to the host
        with self.assertRaisesRegex(AssertionError, 'sees player 2'):
            geo.assert_separation(pair((FAR, 0, 0), seen_by_host=(NEAR, 0, 0)))
        with self.assertRaisesRegex(AssertionError, 'sees player 1'):
            geo.assert_separation(pair((FAR, 0, 0), seen_by_client=(FAR, 0, 0)))

    def test_bad_evidence_is_rejected(self):
        h, c = pair()
        broken = [
            lambda h, c: h.pop('player'),
            lambda h, c: c.pop('me'),
            lambda h, c: c['player'][0].__setitem__(2, 'nan'),
            lambda h, c: c['player'][0].__setitem__(2, 'inf'),
            lambda h, c: c['player'].append(list(c['player'][0])),
            lambda h, c: c['player'][0].pop(),
            lambda h, c: c['me'][0].__setitem__(0, '1'),          # two dumps for player 1
            lambda h, c: c['level'][0].__setitem__(0, '2'),       # another level
            lambda h, c: c['role'][0].__setitem__(0, 'host'),     # two hosts
            lambda h, c: h['role'][0].__setitem__(0, 'client'),   # no host
        ]
        for mutate in broken:
            h, c = pair()
            mutate(h, c)
            with self.assertRaises(AssertionError, msg=str(mutate)):
                geo.assert_separation([h, c])

    def test_needs_two_games(self):
        with self.assertRaises(AssertionError):
            geo.assert_separation(pair()[:1])

    def test_command_line(self):
        with tempfile.TemporaryDirectory() as tmp:
            paths = []
            for i, rows in enumerate(pair()):
                p = Path(tmp) / f'{i}.txt'
                p.write_text(''.join(f'{k} {" ".join(f)}\n' for k, fs in rows.items() for f in fs))
                paths.append(str(p))
            out = io.StringIO()
            with redirect_stdout(out):
                self.assertEqual(geo.main(['check', '--target', '2', '--label', 'x'] + paths), 0)
            self.assertIn('PASS geometry x', out.getvalue())
            near = Path(tmp) / 'near.txt'
            near.write_text('role client\nlevel 1\nme 2\nplayer 100 100 3 0 0\npuppet 1 0 0 0\n')
            out = io.StringIO()
            with redirect_stdout(out):
                self.assertEqual(geo.main(['check', paths[0], str(near)]), 1)
            self.assertIn('FAIL geometry', out.getvalue())
            with redirect_stdout(io.StringIO()):
                self.assertEqual(geo.main(['check', paths[0], str(Path(tmp) / 'missing.txt')]), 1)


class SourceMutants(unittest.TestCase):
    """Broken copies of the helper must fail the same battery, so the rule cannot be weakened unnoticed."""

    def run_battery(self, module):
        far = [dict(d) for d in pair()]
        near = pair((NEAR, 0, 0))
        module.assert_separation(far)
        try:
            module.assert_separation(near)
        except AssertionError:
            return True
        return False

    def load(self, source):
        namespace = {'__name__': 'targeting_geometry_mutant'}
        exec(compile(source, 'mutant', 'exec'), namespace)
        return type('M', (), namespace)

    def test_baseline_and_mutants(self):
        original = (TOOLS / 'targeting_geometry.py').read_text()
        self.assertTrue(self.run_battery(self.load(original)))
        mutants = {
            'minimum of zero': ('MIN_SEPARATION = AOE_FACTOR * LARGEST_RADIUS', 'MIN_SEPARATION = 0'),
            'distance not checked': ('if d < minimum:', 'if False:'),
            'wrong comparison': ('if d < minimum:', 'if d > minimum:'),
            'skips the target': ('if n == target:\n            continue', 'if n != target:\n            continue'),
            'floor on the allowed minimum': ("if minimum < MIN_SEPARATION:", 'if False:'),
        }
        for name, (old, new) in mutants.items():
            self.assertIn(old, original, name)
            broken = original.replace(old, new, 1)
            module = self.load(broken)
            survived = False
            try:
                survived = self.run_battery(module) and self.weakened_minimum_rejected(module)
            except Exception:
                survived = False
            self.assertFalse(survived, f'mutant survived: {name}')

    def weakened_minimum_rejected(self, module):
        try:
            module.assert_separation(pair(), minimum=1)
        except AssertionError:
            return True
        return False


class Scan(unittest.TestCase):
    def tree(self):
        tmp = tempfile.TemporaryDirectory()
        tools = Path(tmp.name) / 'tools'
        tools.mkdir()
        for p in TOOLS.iterdir():
            if p.suffix in ('.sh', '.py') and (p.name.startswith(('qa_', 'check_')) or p.name.endswith(('_check.py', 'showcase.sh'))
                                               or p.name == 'targeting_geometry.py'):
                shutil.copy(p, tools / p.name)
        return tmp, tools

    def test_the_repository_passes(self):
        self.assertEqual(geo.scan(TOOLS.parent), [])

    def test_a_new_targeting_test_without_the_helper_is_found(self):
        tmp, tools = self.tree()
        with tmp:
            (tools / 'qa_newboss.sh').write_text('#!/bin/bash\nlua 1 \'mdkCreateObjectLua("qa_melee",OBJ_GRUNT)\'\n')
            self.assertEqual([n for n, _ in geo.scan(tmp.name)], ['qa_newboss.sh'])
            (tools / 'qa_newboss.sh').write_text('#!/bin/bash\nlua 1 \'mdkCreateObjectLua("qa_melee",OBJ_GRUNT)\'\nmdk_assert_far 2 newboss\n')
            self.assertEqual(geo.scan(tmp.name), [])

    def test_removing_the_call_from_a_real_test_is_found(self):
        tmp, tools = self.tree()
        with tmp:
            for name in ('qa_ai.sh', 'qa_sign.sh', 'check_hans_ai.py', 'check_room_ai.py', 'presence_check.py'):
                path = tools / name
                original = path.read_text()
                self.assertRegex(original, geo.HELPER_CALL, f'{name} must call the helper')
                path.write_text(geo.HELPER_CALL.sub('removed', original))
                self.assertIn(name, [n for n, _ in geo.scan(tmp.name)], name)
                path.write_text(original)
            self.assertEqual(geo.scan(tmp.name), [])

    def test_stale_exemptions_are_found(self):
        tmp, tools = self.tree()
        with tmp:
            (tools / 'qa_playtest.sh').unlink()
            self.assertIn('qa_playtest.sh', [n for n, _ in geo.scan(tmp.name)])


if __name__ == '__main__':
    unittest.main()
