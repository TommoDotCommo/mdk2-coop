from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from motion_check import check


def good_lines():
    rows = []
    for level in range(1, 4):
        for case, planar, rise, dx, dy in [('idle', 0, 0, 0, 0), ('forward', 2, 0, 0, 2),
                                          ('back', 2, 0, 0, -2), ('left', 2, 0, -2, 0),
                                          ('right', 2, 0, 2, 0), ('jump', 0, 1, 0, 0)]:
            rows.append(f'lua: qa-motion l{level}_{case} type {99+level} frames 60 bad 0 '
                        f'planar {planar}.000 rise {rise}.000 fall 0.000 dx {dx}.000 dy {dy}.000')
    return rows


class MotionOracle(unittest.TestCase):
    def test_three_characters_two_games(self):
        log = '\n'.join(good_lines())
        self.assertFalse(check([log, log]))

    def test_missing_duplicate_and_single_game(self):
        rows = good_lines()
        log = '\n'.join(rows)
        for mutant in ('\n'.join(rows[1:]), log + '\n' + rows[0], ''):
            self.assertTrue(check([log, mutant]))
        self.assertTrue(check([log]))

    def test_bad_observations_fail(self):
        rows = good_lines()
        mutations = [(0, 'type 100', 'type 101'), (0, 'frames 60', 'frames 0'),
                     (0, 'bad 0', 'bad 1'), (0, 'planar 0.000', 'planar 0.300'),
                     (0, 'rise 0.000', 'rise nan'), (0, 'fall 0.000', 'fall inf'),
                     (0, 'fall 0.000', 'fall -1'), (0, 'fall 0.000', 'fall nil'),
                     (1, 'planar 2.000', 'planar 0.000'), (2, 'dy -2.000', 'dy 2.000'),
                     (5, 'rise 1.000', 'rise 0.000')]
        for index, old, new in mutations:
            mutant = rows.copy()
            mutant[index] = mutant[index].replace(old, new)
            with self.subTest(old=old, new=new):
                self.assertTrue(check(['\n'.join(rows), '\n'.join(mutant)]))

    def test_suppressed_input_and_lua_error(self):
        log = '\n'.join(good_lines())
        mutant = log.replace('planar 2.000', 'planar 0.000').replace('rise 1.000', 'rise 0.000')
        self.assertTrue(check([log, mutant]))
        self.assertTrue(check([log, log + '\nLUA ERROR: nil call']))


if __name__ == '__main__':
    unittest.main()
