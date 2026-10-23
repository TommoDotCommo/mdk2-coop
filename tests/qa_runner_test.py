"""Exercise lane overlap, barriers and allocation rollback with real subprocesses."""
import contextlib
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import qa_all


ALLOCATOR = '''#!/usr/bin/env python3
import fcntl, json, os, sys
from pathlib import Path
root = Path(os.environ['FAKE_RESOURCES'])
with (root/'allocation.lock').open('a') as lock:
    fcntl.flock(lock, fcntl.LOCK_EX)
    with (root/'allocations').open('a') as log:
        log.write(sys.argv[1]+' '+os.environ['MDK2_QA_OWNER']+'\\n')
    if sys.argv[1] == 'release':
        raise SystemExit(0)
    counter = root/'counter'
    n = int(counter.read_text())+1 if counter.exists() else 1
    counter.write_text(str(n))
    if os.environ.get('ALLOCATOR_MODE') == 'busy' and n == 2:
        raise SystemExit(3)
    if os.environ.get('ALLOCATOR_MODE') == 'malformed':
        print('[]')
        raise SystemExit(0)
    if os.environ.get('ALLOCATOR_MODE') == 'collision': n = 1
    values = {f'MDK2_DIR{i}':f'D:/test/lane{n}/copy{i}' for i in range(1,int(sys.argv[2])+1)}
    values.update(MDK2_PORT=str(28000+n*100), MDK2_TOOLS=f'D:/test/lane{n}/tools',
                  MDK2_PIDS=str(root/f'pids{n}'), MDK2_RUN=str(root))
    print(json.dumps(values))
'''

GATE = '''import json, os, sys, time
from pathlib import Path
root = Path(os.environ['FAKE_RESOURCES'])
name = sys.argv[1]
start = time.monotonic()
time.sleep(0.25)
(root/(name+'.json')).write_text(json.dumps({'start':start,'end':time.monotonic(),
    'path':os.environ.get('MDK2_DIR1'), 'shards':os.environ.get('MDK2_SHARDS'),
    'players':os.environ.get('MDK2_N')}))
print('1 passed, 0 failed')
'''


class LaneRunner(unittest.TestCase):
    def run_profile(self, folder, mode='', names=None):
        root = Path(folder)
        allocator = root/'allocator'
        allocator.write_text(ALLOCATOR)
        allocator.chmod(0o755)
        gates = [qa_all.Gate('build', (sys.executable, '-c', GATE, 'build'))]
        for name in names or ['a', 'b', 'launcher', 'character-motion', 'c', 'd', 'installer', 'campaign']:
            gates.append(qa_all.Gate(name, (sys.executable, '-c', GATE, name), 2,
                                     result='counts', shards=2 if name == 'campaign' else 1))
        original_execute = qa_all.execute
        def execute(command, env, logfile, timeout):
            if command[0] == 'bash':  # no real Windows games exist in this test
                logfile.write_text('synthetic game cleanup\n')
                return 0, False
            return original_execute(command, env, logfile, timeout)
        with mock.patch.object(qa_all, 'ROOT', root), \
             mock.patch.object(qa_all, 'PROFILES', {'test': gates}), \
             mock.patch.dict(os.environ, {'FAKE_RESOURCES': folder, 'ALLOCATOR_MODE': mode}), \
             mock.patch.object(sys, 'argv', ['qa_all.py', 'test', '--lanes', '2', '--resources', str(allocator)]), \
             mock.patch.object(qa_all, 'collect_games'), \
             mock.patch.object(qa_all, 'execute', side_effect=execute), \
             contextlib.redirect_stdout(io.StringIO()):
            code = qa_all.main()
        report = json.loads(next(root.glob('local/regression/*/results.json')).read_text())
        return code, report

    def test_parallel_games_and_exclusive_barriers(self):
        with tempfile.TemporaryDirectory() as folder:
            code, report = self.run_profile(folder)
            self.assertEqual(code, 0, report)
            self.assertEqual([r['name'] for r in report['gates']],
                             ['build', 'a', 'b', 'launcher', 'character-motion', 'c', 'd', 'installer', 'campaign'])
            root = Path(folder)
            events = {name: json.loads((root/(name+'.json')).read_text())
                      for name in ['build','a','b','launcher','character-motion','c','d','installer','campaign']}
            self.assertLess(max(events[n]['start'] for n in ['a','b']),
                            min(events[n]['end'] for n in ['a','b']))
            self.assertNotEqual(events['a']['path'], events['b']['path'])
            for first, second in [('build','a'),('a','launcher'),('b','launcher'),
                                  ('launcher','character-motion'),('character-motion','c'),('character-motion','d'),('c','installer'),
                                  ('d','installer'),('installer','campaign')]:
                self.assertLessEqual(events[first]['end'], events[second]['start'])
            lines = (root/'allocations').read_text().splitlines()
            self.assertEqual(sorted(l[8:] for l in lines if l.startswith('acquire ')),
                             sorted(l[8:] for l in lines if l.startswith('release ')))

    def test_busy_rolls_back_other_lanes_before_starting_games(self):
        with tempfile.TemporaryDirectory() as folder:
            code, report = self.run_profile(folder, 'busy', ['a','b'])
            self.assertEqual(code, 3, report)
            self.assertEqual(report['result'], 'BUSY')
            self.assertFalse((Path(folder)/'a.json').exists())
            self.assertFalse((Path(folder)/'b.json').exists())
            lines = (Path(folder)/'allocations').read_text().splitlines()
            self.assertEqual(sum(l.startswith('release ') for l in lines), 1)

    def test_malformed_success_is_released(self):
        with tempfile.TemporaryDirectory() as folder:
            code, report = self.run_profile(folder, 'malformed', ['a'])
            self.assertEqual(code, 1, report)
            self.assertIn('JSON object', report['gates'][-1]['error'])
            self.assertIn('release ', (Path(folder)/'allocations').read_text())

    def test_overlapping_resources_fail_before_games(self):
        with tempfile.TemporaryDirectory() as folder:
            code, report = self.run_profile(folder, 'collision', ['a','b'])
            self.assertEqual(code, 1, report)
            self.assertIn('overlapping', report['gates'][-1]['error'])
            self.assertFalse((Path(folder)/'a.json').exists())
            lines = (Path(folder)/'allocations').read_text().splitlines()
            self.assertEqual(sum(l.startswith('release ') for l in lines), 2)

    def test_cancellation_stops_harness(self):
        import threading
        import time
        qa_all.CANCEL.clear()
        with tempfile.TemporaryDirectory() as folder:
            timer = threading.Timer(0.2, qa_all.CANCEL.set)
            timer.start()
            start = time.monotonic()
            try:
                with self.assertRaises(InterruptedError):
                    qa_all.execute((sys.executable, '-c', 'import time; time.sleep(60)'),
                                   None, Path(folder)/'log', 120)
                self.assertLess(time.monotonic()-start, 5)
            finally:
                timer.cancel()
                qa_all.CANCEL.clear()


class ShellResources(unittest.TestCase):
    def test_helper_install_copies_are_serialized(self):
        import subprocess
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder)
            (root/'wslpath').write_text('#!/bin/sh\nprintf "%s\\n" "$2"\n')
            (root/'cp').write_text("""#!/usr/bin/env python3
import os, time
from pathlib import Path
p=Path(os.environ['COPY_MARKER'])
with p.open('x'):
    time.sleep(0.05)
p.unlink()
""")
            for name in ('wslpath','cp'): (root/name).chmod(0o755)
            env={k:v for k,v in os.environ.items() if not k.startswith('MDK2_')}
            env.update(PATH=folder+os.pathsep+env['PATH'], COPY_MARKER=str(root/'copy-active'),
                       MDK2_TOOLS=str(root/'tools'), MDK2_DIR1='D:/test/copy')
            command=['bash','-c', 'source "$1"; mdk_tools_install', 'bash',
                     str(qa_all.ROOT/'tools/mdk.sh')]
            processes=[subprocess.Popen(command,env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
                       for _ in range(4)]
            results=[(p.communicate(timeout=5),p.returncode) for p in processes]
            self.assertTrue(all(code==0 for _,code in results),results)

    def test_sweep_never_boots_after_failed_install(self):
        import shutil
        import subprocess
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder); (root/'tools').mkdir()
            shutil.copy2(qa_all.ROOT/'tools/sweep.sh',root/'tools/sweep.sh')
            (root/'tools/checkpoints.txt').write_text('1-1\n')
            (root/'tools/install.sh').write_text('#!/bin/sh\nexit 1\n')
            (root/'tools/install.sh').chmod(0o755)
            marker=root/'booted'
            (root/'tools/mdk.sh').write_text("""MDK2_N=2
mdk_seq() { seq 1 2; }
mdk_kill() { :; }
mdk_boot() { touch "$BOOT_MARKER"; }
mdk_log() { echo "$BOOT_MARKER.log"; }
""")
            env={k:v for k,v in os.environ.items() if not k.startswith('MDK2_')}
            result=subprocess.run(['bash',str(root/'tools/sweep.sh'),'1-1','1-1'],
                                  env=env|{'BOOT_MARKER':str(marker)},capture_output=True,text=True,timeout=5)
            self.assertNotEqual(result.returncode,0)
            self.assertIn('fixture installation failed',result.stderr)
            self.assertFalse(marker.exists())


if __name__ == '__main__':
    unittest.main()
