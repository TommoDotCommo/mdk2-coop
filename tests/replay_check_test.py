import copy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from replay_check import check


class ParticipantChecks(unittest.TestCase):
    def setUp(self):
        self.dumps = [dict(me=1, life=0, player=(100, 100, (0, 0, 0)), puppet={2: (1, 0, 0)}),
                      dict(me=2, life=0, player=(100, 100, (1, 0, 0)), puppet={1: (0, 0, 0)})]
        self.logs = ["level 1 checkpoint 1 started\n", "level 1 checkpoint 1 started\n"]

    def test_two_real_participants(self):
        self.assertFalse(check(self.dumps, self.logs, 2))

    def test_missing_participant(self):
        self.assertTrue(check(self.dumps[:1], self.logs[:1], 2))

    def test_failures_are_detected(self):
        for field, value in [("life", 1), ("life", 2), ("player", None), ("puppet", {}),
                             ("me", 1), ("player", (100, 100, (float("nan"), 0, 0))),
                             ("player", (100, 0, (0, 0, 0)))]:
            with self.subTest(field=field, value=value):
                dumps = copy.deepcopy(self.dumps)
                dumps[1][field] = value
                self.assertTrue(check(dumps, self.logs, 2))

    def test_lua_error(self):
        self.assertTrue(check(self.dumps, ["", "error: attempt to call a nil value"], 2))


if __name__ == "__main__":
    unittest.main()
