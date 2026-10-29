"""Crash watch: a dump, a crash marker or a dialog appearing during a run fails it; others' dumps do not."""
import contextlib
import io
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import crashwatch
import minidump_fixture


class CrashWatchTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        self.crash = root / 'crash'
        self.crash.mkdir()
        self.mine = root / 'copy1'
        (self.mine / 'coop' / 'logs').mkdir(parents=True)
        self.other = root / 'other'
        self.state = str(root / 'state.json')
        os.environ['MDK2_CRASH_DIALOGS'] = '0'

    def tearDown(self):
        self.tmp.cleanup()

    def begin(self):
        crashwatch.begin(self.state, str(self.crash), [str(self.mine)])

    def dump(self, name, directory):
        image = str(directory).replace('/', '\\') + '\\mdk2hd.exe'
        (self.crash / name).write_bytes(minidump_fixture.build([(0x400000, 0x208000, image)]))

    def test_quiet_run_passes(self):
        self.dump('dump0000.dmp', self.mine)             # older than the run: not ours to fail
        self.begin()
        self.assertEqual(crashwatch.verdict(self.state), (True, []))

    def test_new_dump_of_our_copy_fails_with_its_path_and_site(self):
        self.begin()
        self.dump('dump0001.dmp', self.mine)
        ok, lines = crashwatch.verdict(self.state)
        self.assertFalse(ok)
        self.assertIn('dump0001.dmp', lines[0])
        self.assertIn('mdk2hd.exe+0x672ed', lines[0])

    def test_dump_of_another_copy_is_only_noted(self):
        self.begin()
        self.dump('dump0001.dmp', self.other)
        ok, lines = crashwatch.verdict(self.state)
        self.assertTrue(ok)
        self.assertIn('other copy', lines[0])

    def test_unreadable_new_dump_fails_rather_than_hiding(self):
        self.begin()
        (self.crash / 'dump0002.dmp').write_bytes(b'MDMP-but-broken' * 4)
        self.assertFalse(crashwatch.verdict(self.state)[0])

    def test_crash_marker_in_session_logs_fails(self):
        self.begin()
        (self.mine / 'coop' / 'logs' / '2026-10-29T01-50-24Z_p9032.crash').write_bytes(b'')
        ok, lines = crashwatch.verdict(self.state)
        self.assertFalse(ok)
        self.assertIn('crash-marker', lines[0])

    def test_dialog_fails_even_when_it_was_closed(self):
        self.begin()
        crashwatch.record_dialogs(self.state, True, ['9032|66|MDK2|Jinkies! There was an error.'])
        ok, lines = crashwatch.verdict(self.state)
        self.assertFalse(ok)
        self.assertIn('Jinkies', lines[0])

    def test_recorded_exit_fails(self):
        self.begin()
        crashwatch.main(['record', '--state', self.state, 'exit', 'game 2 is gone'])
        self.assertFalse(crashwatch.verdict(self.state)[0])

    def test_deliberate_crash_is_allowed_once_per_kind(self):
        self.begin()
        crashwatch.main(['expect', '--state', self.state, '1'])
        self.dump('dump0003.dmp', self.mine)
        (self.mine / 'coop' / 'logs' / 'a.crash').write_bytes(b'')
        self.assertTrue(crashwatch.verdict(self.state)[0])
        self.dump('dump0004.dmp', self.mine)             # a second, unplanned crash still fails
        self.assertFalse(crashwatch.verdict(self.state)[0])

    def test_expected_findings_are_labelled_and_collected(self):
        self.begin()
        crashwatch.main(['expect', '--state', self.state, '1'])
        self.dump('dump0003.dmp', self.mine)
        self.dump('dump0004.dmp', self.other)
        ok, lines = crashwatch.verdict(self.state)
        self.assertTrue(ok)
        self.assertEqual(sorted(l.split(' ')[0] for l in lines), ['expected', 'note'])
        target = Path(self.tmp.name) / 'evidence'
        moved = crashwatch.collect(self.state, str(target))
        self.assertEqual([Path(m).name for m in moved], ['dump0003.dmp'])
        self.assertTrue((self.crash / 'dump0004.dmp').exists())        # another copy's dump stays put
        self.assertFalse((self.crash / 'dump0003.dmp').exists())

    def test_check_command_exit_status(self):
        self.begin()
        run = lambda: subprocess.run([sys.executable, '-I', str(ROOT / 'tools' / 'crashwatch.py'), 'check',
                                      '--state', self.state, '--no-dialogs'], capture_output=True, text=True)
        self.assertEqual(run().returncode, 0)
        self.dump('dump0001.dmp', self.mine)
        r = run()
        self.assertEqual(r.returncode, 1)
        self.assertIn('CRASH dump', r.stdout)

    def test_mutant_that_ignores_new_dumps_is_caught(self):
        """The detector must be able to fail: with the dump scan disabled this very scenario passes."""
        self.begin()
        self.dump('dump0001.dmp', self.mine)
        real = crashwatch.list_dumps
        crashwatch.list_dumps = lambda d: {}
        try:
            self.assertTrue(crashwatch.verdict(self.state)[0])     # blind detector: the mutant
        finally:
            crashwatch.list_dumps = real
        self.assertFalse(crashwatch.verdict(self.state)[0])

    def test_redirected_crash_folder_of_our_copy_is_watched(self):
        inst = self.mine / 'profile' / 'MDK2HD' / 'crash'
        inst.mkdir(parents=True)
        crashwatch.begin(self.state, [str(self.crash), str(inst)], [str(self.mine)])
        image = str(self.mine).replace('/', '\\') + '\\mdk2hd.exe'
        (inst / 'dump0000.dmp').write_bytes(minidump_fixture.build([(0x400000, 0x208000, image)]))
        ok, lines = crashwatch.verdict(self.state)
        self.assertFalse(ok)
        self.assertIn(str(inst), lines[0])

    def test_dump_in_a_copys_own_folder_is_ours_even_with_a_foreign_image_path(self):
        inst = self.mine / 'profile' / 'MDK2HD' / 'crash'
        inst.mkdir(parents=True)
        crashwatch.begin(self.state, [str(self.crash), str(inst)], [str(self.mine)])
        (inst / 'dump0000.dmp').write_bytes(minidump_fixture.build([(0x400000, 0x208000, 'C:\\elsewhere\\mdk2hd.exe')]))
        self.assertFalse(crashwatch.verdict(self.state)[0])

    def test_real_folder_still_attributes_by_image_when_instance_folders_are_watched(self):
        inst = self.mine / 'profile' / 'MDK2HD' / 'crash'
        inst.mkdir(parents=True)
        crashwatch.begin(self.state, [str(self.crash), str(inst)], [str(self.mine)])
        self.dump('dump0001.dmp', self.other)
        ok, lines = crashwatch.verdict(self.state)
        self.assertTrue(ok)
        self.assertIn('other copy', lines[0])
        self.dump('dump0002.dmp', self.mine)
        self.assertFalse(crashwatch.verdict(self.state)[0])

    def test_dump_older_than_the_run_in_an_instance_folder_is_ignored(self):
        inst = self.mine / 'profile' / 'MDK2HD' / 'crash'
        inst.mkdir(parents=True)
        (inst / 'dump0000.dmp').write_bytes(b'old')
        crashwatch.begin(self.state, [str(self.crash), str(inst)], [str(self.mine)])
        self.assertEqual(crashwatch.verdict(self.state), (True, []))

    def test_collect_keeps_same_named_dumps_of_two_folders(self):
        inst = self.mine / 'profile' / 'MDK2HD' / 'crash'
        inst.mkdir(parents=True)
        crashwatch.begin(self.state, [str(self.crash), str(inst)], [str(self.mine)])
        image = str(self.mine).replace('/', '\\') + '\\mdk2hd.exe'
        for folder in (self.crash, inst):
            (folder / 'dump0000.dmp').write_bytes(minidump_fixture.build([(0x400000, 0x208000, image)]))
        moved = crashwatch.collect(self.state, str(Path(self.tmp.name) / 'evidence'))
        self.assertEqual(len(moved), 2)
        self.assertEqual(len(set(moved)), 2)


class ShellGuardTest(unittest.TestCase):
    """tools/mdk.sh: a script that sources it fails (97) when a dump of its copy appears."""

    def run_script(self, body, guard='1'):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            (tmp / 'game1' / 'coop' / 'logs').mkdir(parents=True)
            (tmp / 'crash').mkdir()
            script = tmp / 'script.sh'
            script.write_text('#!/usr/bin/env bash\nsource "%s"\n%s\n' % (ROOT / 'tools' / 'mdk.sh', body))
            env = dict(os.environ, MDK2_N='1', MDK2_DIR1=str(tmp / 'game1'), MDK2_CRASH_DIR=str(tmp / 'crash'),
                       MDK2_CRASH_DIALOGS='0', MDK2_CRASH_GUARD=guard, MDK2_PIDS=str(tmp / 'pids'))
            env.pop('MDK2_CRASH_STATE', None)
            env['FAKE'] = str(tmp)
            r = subprocess.run(['bash', str(script)], env=env, capture_output=True, text=True)
            return r

    FAKE_DUMP = '''python3 - <<'PY'
import os, sys
sys.path.insert(0, os.environ['TESTS'])
import minidump_fixture
img = os.environ['FAKE'] + '/game1/mdk2hd.exe'
open(os.environ['FAKE'] + '/crash/dump0001.dmp', 'wb').write(minidump_fixture.build([(0x400000, 0x208000, img)]))
PY
'''

    def run_fake(self, body, guard='1'):
        os.environ['TESTS'] = str(Path(__file__).resolve().parent)
        return self.run_script(self.FAKE_DUMP + body, guard)

    def test_clean_run_keeps_its_status(self):
        r = self.run_script('echo fine; exit 0')
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_fake_dump_appearing_during_a_run_fails_it(self):
        r = self.run_fake('exit 0')
        self.assertEqual(r.returncode, 97, r.stderr)
        self.assertIn('CRASH dump', r.stderr)
        self.assertIn('dump0001.dmp', r.stderr)

    def test_failure_status_of_the_script_is_kept(self):
        self.assertEqual(self.run_fake('exit 5').returncode, 5)

    def test_it_also_works_behind_the_scripts_own_exit_trap(self):
        r = self.run_fake('trap "echo cleanup" EXIT\nexit 0')
        self.assertEqual(r.returncode, 97, r.stderr)
        self.assertIn('cleanup', r.stdout)

    def test_dump_in_the_copys_redirected_crash_folder_fails_the_run(self):
        body = '''mkdir -p "$FAKE/game1/profile/MDK2HD/crash"
python3 - <<'PY'
import os, sys
sys.path.insert(0, os.environ['TESTS'])
import minidump_fixture
img = os.environ['FAKE'] + '/game1/mdk2hd.exe'
open(os.environ['FAKE'] + '/game1/profile/MDK2HD/crash/dump0000.dmp', 'wb').write(minidump_fixture.build([(0x400000, 0x208000, img)]))
PY
exit 0'''
        os.environ['TESTS'] = str(Path(__file__).resolve().parent)
        r = self.run_script(body)
        self.assertEqual(r.returncode, 97, r.stderr)
        self.assertIn('profile/MDK2HD/crash/dump0000.dmp', r.stderr)
        self.assertEqual(self.run_script(body, guard='0').returncode, 0)     # mutant: guard off lets it pass

    def test_mutant_guard_off_lets_the_crash_pass(self):
        self.assertEqual(self.run_fake('exit 0', guard='0').returncode, 0)

    def test_expected_deliberate_crash_passes(self):
        self.assertEqual(self.run_fake('mdk_crash_expect 1\nexit 0').returncode, 0)


if __name__ == '__main__':
    unittest.main()
