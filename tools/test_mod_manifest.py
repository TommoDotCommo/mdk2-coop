#!/usr/bin/env python3
"""coop-mod.json (tools/mod_manifest.py) against a real release build, and mutants the manifest check (run by check_release_package.py) must reject:
  wrong-hash     the manifest's hash of dinput8.dll does not match the zip
  wrong-size     the manifest's size of coop/coop.lua does not match
  root-differs   the copy at the zip root differs from the one in the top folder
  no-root-copy   the root copy is missing
Game-free. Usage: test_mod_manifest.py [release zip]   (without one, builds a release into a temporary folder)"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import check_release_package  # noqa: E402
import mod_manifest  # noqa: E402

TAG = 'v9.8.6-manifesttest'


def rewrite(src, dst, edit):
    """Copy the release folder's zip with edit(name, bytes) -> bytes|None applied to each member; refresh the sidecar."""
    for f in ('MDK2-Coop-Setup.exe', 'MDK2-Coop-Setup.exe.sha256'):
        shutil.copyfile(src.with_name(f), dst.with_name(f))
    with zipfile.ZipFile(src) as a, zipfile.ZipFile(dst, 'w', zipfile.ZIP_DEFLATED) as b:
        for info in a.infolist():
            data = edit(info.filename, a.read(info))
            if data is not None:
                b.writestr(info, data)
    dst.with_name(dst.name + '.sha256').write_text('%s  %s\n' % (hashlib.sha256(dst.read_bytes()).hexdigest(), dst.name))


def main():
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        if len(sys.argv) > 1:
            zip_path = Path(sys.argv[1])
        else:
            env = dict(os.environ, RELEASE_OUT=str(tmp / 'release'), RELEASE_REUSE_DLL='1')
            env.pop('SIGN_CMD', None)
            if not (ROOT / 'bridge/dinput8.dll').exists():
                subprocess.run(['make', '-s', '-C', str(ROOT / 'bridge')], check=True)
            subprocess.run(['bash', str(ROOT / 'tools/release.sh'), TAG], check=True, env=env, capture_output=True)
            zip_path = tmp / 'release' / ('mdk2-coop-%s.zip' % TAG)
        failures = []
        try:
            check_release_package.check(zip_path)
        except AssertionError as e:
            failures.append('real release rejected: %s' % e)
        top = zip_path.name[:-len('.zip')] + '/'

        def manifest_edit(fn, both=True):
            def edit(name, data):
                if name == 'coop-mod.json' or (both and name == top + 'coop-mod.json'):
                    m = json.loads(data)
                    fn(m)
                    return json.dumps(m, indent=2).encode() + b'\n'
                return data
            return edit

        def bad_hash(m):
            m['files'][0]['sha256'] = '0' * 64

        def bad_size(m):
            m['files'][1]['size'] += 1

        def other_version(m):
            m['homepage'] = 'https://example.invalid/'
        mutants = {
            'wrong-hash': manifest_edit(bad_hash),
            'wrong-size': manifest_edit(bad_size),
            'root-differs': manifest_edit(other_version, both=False),
            'no-root-copy': lambda name, data: None if name == 'coop-mod.json' else data,
        }
        for label, edit in mutants.items():
            d = tmp / label
            d.mkdir()
            bad = d / zip_path.name
            rewrite(zip_path, bad, edit)
            try:       # the manifest check itself (check_release_package.check calls it), so no other check hides a survivor
                mod_manifest.check(bad)
                failures.append('mutant %s survived' % label)
            except (AssertionError, KeyError):
                pass
    if failures:
        print('\n'.join('FAIL: ' + f for f in failures))
        return 1
    print('PASS: coop-mod.json matches the release zip; %d manifest mutants rejected' % len(mutants))
    return 0


if __name__ == '__main__':
    sys.exit(main())
