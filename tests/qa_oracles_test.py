"""Mutation checks for the oracles used by real campaign and feature suites."""
from pathlib import Path
import subprocess
import contextlib
import io
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import qa_all
import statecheck
import sweep_check

ROOT = Path(__file__).resolve().parents[1]


def dump(slot):
    return (f"role {'host' if slot == 1 else 'client'}\nlevel 1\nme {slot}\nlife 0\n"
            f"player 0 100 {slot} 2 3\npuppet {3-slot} {3-slot} 2 3\n"
            "ent 1 217 100 1 2 3 enemy\nitem 314 1 2 3 decoy\ndoor 0 door\n")


class StateOracle(unittest.TestCase):
    def run_check(self, host, client, *args):
        with tempfile.TemporaryDirectory() as folder:
            paths = [Path(folder) / name for name in ("host", "client")]
            for path, text in zip(paths, (host, client)):
                path.write_text(text)
            return subprocess.run([sys.executable, str(ROOT / "tools/statecheck.py"),
                                   *map(str, paths), *args], capture_output=True, text=True)

    def test_matching_worlds(self):
        result = self.run_check(dump(1), dump(2))
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("1 enemies compared", result.stdout)

    def test_broken_worlds_fail(self):
        mutants = ["", "level 1\n", dump(2).replace("me 2", "me 1"),
                   dump(2).replace("role client", "role host"), dump(2).replace("level 1", "level 2"),
                   dump(2).replace("217 100", "217 99"), dump(2).replace("217 100", "218 100"),
                   dump(2).replace("ent 1 217 100 1 2 3 enemy", "ent 1 gone"),
                   dump(2).replace("item 314 1 2 3 decoy\n", ""),
                   dump(2).replace("puppet 1 1 2 3\n", ""),
                   dump(2).replace("puppet 1 1 2 3", "puppet 1 100 2 3"),
                   dump(2).replace("ent 1 217 100 1 2 3", "ent 1 217 100 100 2 3"),
                   dump(2) + "ent 2 217 100 1 2 3 extra\n"]
        for mutant in mutants:
            with self.subTest(mutant=mutant):
                result = self.run_check(dump(1), mutant)
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertIn("FAIL", result.stdout)

    def test_nonfinite_and_truncated_positions_fail(self):
        for row in ("player 0 100", "puppet 1", "ent 1 217 100", "actor 217 100", "item 314"):
            for position in ("nan 2 3", "inf 2 3", "1 2"):
                with self.subTest(row=row, position=position):
                    result = self.run_check(dump(1), dump(2) + row + " " + position + "\n")
                    self.assertNotEqual(result.returncode, 0, result.stdout)

    def test_invalid_tolerance(self):
        for value in ("nan", "inf", "0", "-1"):
            self.assertNotEqual(self.run_check(dump(1), dump(2), "--tol", value).returncode, 0)

    def test_timing_warnings_remain_warnings(self):
        result = self.run_check(dump(1), dump(2).replace("door 0", "door 1"))
        self.assertEqual(result.returncode, 0)
        self.assertIn("warn", result.stdout)

    def test_missing_living_player_fails_but_spectator_is_allowed(self):
        missing = dump(2).replace("player 0 100 2 2 3\n", "")
        self.assertNotEqual(self.run_check(dump(1), missing).returncode, 0)
        self.assertEqual(self.run_check(dump(1), missing.replace("life 0", "life 3")).returncode, 0)


class SweepOracle(unittest.TestCase):
    row = "| 1-1 | yes | 1/1 | 3/3 | 0 | 0 | 2 | 0 | 0/1 | yes |  |"

    def test_complete_pass(self):
        self.assertEqual(sweep_check.check(self.row, ["1-1"], 2), [])

    def test_each_broken_protection_fails(self):
        fields = [f.strip() for f in self.row.strip("|").split("|")]
        for column, mutant in [(1, "no"), (2, "0/0"), (2, "1"), (3, "3/2"), (3, "3"),
                               (4, "-1"), (5, "1"), (7, "1"), (8, "1/0"), (8, "no dump"),
                               (9, "NO"), (10, "not settled after 8 s;"), (10, "dump saved;")]:
            broken = fields.copy()
            broken[column] = mutant
            with self.subTest(column=column, mutant=mutant):
                self.assertTrue(sweep_check.check("| " + " | ".join(broken) + " |", ["1-1"], 2))

    def test_missing_duplicate_extra_and_shard_failure(self):
        for text in ("", self.row + "\n" + self.row, self.row.replace("1-1", "1-2"),
                     "| 1-1 | not run | | | | | | | | | shard stopped |"):
            self.assertTrue(sweep_check.check(text, ["1-1"], 2))
        self.assertTrue(sweep_check.check(self.row, ["1-1", "1-2"], 2))

    def test_all_clients_required(self):
        self.assertFalse(sweep_check.check(self.row.replace("1/1", "2/2/2").replace("3/3", "3/3/3"), ["1-1"], 3))
        self.assertTrue(sweep_check.check(self.row, ["1-1"], 3))

    def test_removed_client_spawns_are_not_surviving_orphans(self):
        self.assertFalse(sweep_check.check(self.row.replace("3/3 | 0", "3/3 | 3"), ["1-1"], 2))


class RunnerOracle(unittest.TestCase):
    def test_no_hollow_passes(self):
        gate = qa_all.Gate("test", (), 2, result="counts")
        self.assertEqual(qa_all.score(gate, 0, "3 passed, 0 failed"), "PASS")
        for text in ("", "PASS check", "0 passed, 0 failed", "3 passed, 1 failed"):
            self.assertEqual(qa_all.score(gate, 0, text), "FAIL")
        self.assertEqual(qa_all.score(gate, 1, "3 passed, 0 failed"), "FAIL")
        self.assertEqual(qa_all.score(gate, 0, "3 passed, 0 failed", True), "TIMEOUT")

    def test_special_gate_results(self):
        for kind, good in (("sweep", "RESULT: PASS\n"),
                           ("installer", "PASS: 24 installer checks (disposable fixtures only)\n"),
                           ("projectiles", "Projectile checks passed for 3 games\n")):
            gate = qa_all.Gate("test", (), 3, result=kind)
            self.assertEqual(qa_all.score(gate, 0, good), "PASS")
            self.assertEqual(qa_all.score(gate, 0, ""), "FAIL")
        self.assertEqual(qa_all.score(qa_all.CAMPAIGN, 0, "RESULT: FAIL\nRESULT: PASS\n"), "FAIL")

    def test_campaign_manifest(self):
        points = (ROOT / "tools/checkpoints.txt").read_text().split()
        self.assertEqual(len(points), 96)
        self.assertEqual(len(set(points)), 96)
        self.assertLess(points.index("3-16"), points.index("3-7"))

    def test_full_profile_includes_every_game_suite(self):
        covered = {g.command[1] for g in qa_all.PROFILES["full"] if g.command[0] == "bash"}
        suites = {str(p.relative_to(ROOT)) for p in (ROOT / "tools").glob("qa*.sh")}
        self.assertFalse(suites - covered, f"unregistered QA suites: {suites - covered}")

    def test_timeout_kills_harness(self):
        with tempfile.TemporaryDirectory() as folder:
            _, timed_out = qa_all.execute((sys.executable, "-c", "import time; time.sleep(60)"),
                                           None, Path(folder) / "output", 0.05)
            self.assertTrue(timed_out)

    def test_runner_releases_lease_even_when_artifact_collection_fails(self):
        gate = qa_all.Gate("fake-game", ("unused",), 2, result="counts")
        def run(command, **kwargs):
            # No existing lease; provisioning and release succeed.
            return subprocess.CompletedProcess(command, 1 if command[1] == "env" else 0)
        def execute(command, env, logfile, timeout):
            logfile.write_text("1 passed, 0 failed\n")
            return 0, False
        with tempfile.TemporaryDirectory() as folder, \
             mock.patch.object(qa_all, "ROOT", Path(folder)), \
             mock.patch.object(qa_all, "PROFILES", {"test": [gate]}), \
             mock.patch.object(sys, "argv", ["qa_all.py", "test"]), \
             mock.patch.object(qa_all.subprocess, "run", side_effect=run) as calls, \
             mock.patch.object(qa_all.subprocess, "call", return_value=0), \
             mock.patch.object(qa_all, "lab_env", return_value={}), \
             mock.patch.object(qa_all, "execute", side_effect=execute), \
             mock.patch.object(qa_all, "collect_games", side_effect=OSError("artifact failure")), \
             contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(qa_all.main(), 1)
            self.assertTrue(any(call.args[0] == ["lab", "release"] for call in calls.call_args_list))
            report = next(Path(folder).glob("local/regression/*/results.json"))
            self.assertIn('"result": "FAIL"', report.read_text())

    def test_campaign_shards_allocate_and_collect_every_copy(self):
        calls = []
        def execute(command, env, logfile, timeout):
            calls.append(env.copy())
            logfile.write_text("RESULT: PASS\n")
            return 0, False
        def run(command, **kwargs):
            return subprocess.CompletedProcess(command, 1 if command[1] == "env" else 0)
        with tempfile.TemporaryDirectory() as folder, \
             mock.patch.object(qa_all, "ROOT", Path(folder)), \
             mock.patch.object(qa_all, "PROFILES", {"test": [qa_all.CAMPAIGN]}), \
             mock.patch.object(sys, "argv", ["qa_all.py", "test", "--campaign-shards", "4"]), \
             mock.patch.object(qa_all.subprocess, "run", side_effect=run), \
             mock.patch.object(qa_all.subprocess, "call", return_value=0) as acquire, \
             mock.patch.object(qa_all, "lab_env", return_value={}), \
             mock.patch.object(qa_all, "execute", side_effect=execute), \
             mock.patch.object(qa_all, "collect_games") as collect, \
             contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(qa_all.main(), 0)
            self.assertIn("8", acquire.call_args.args[0])
            self.assertEqual(calls[0]["MDK2_N"], "2")
            self.assertEqual(calls[0]["MDK2_SHARDS"], "4")
            self.assertEqual(calls[1]["MDK2_N"], "8")
            self.assertEqual(collect.call_args.args[1], 8)


if __name__ == "__main__":
    unittest.main()
