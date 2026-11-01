#!/usr/bin/env python3
"""An installed older version updates itself to the release layout tools/release.sh builds now (the zip plus the single-file
Setup.exe and its sidecar as extra assets). Runs the updater's own code (bridge/launcher_update.h, which this layout change does
not touch) over a real release build; game-free. Mutants (layouts the updater cannot use) must fail:
  root-renamed   the zip's top folder is not mdk2-coop-<tag>
  no-sidecar     the zip's .sha256 asset is missing
  setup-first    nothing: the Setup.exe asset listed first must not be picked (proves the test sees the asset choice)"""
import hashlib
import json
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
TAG = 'v9.8.7-layouttest'


def build_release(out):
    env = dict(os.environ, RELEASE_OUT=str(out), RELEASE_REUSE_DLL='1')
    if not (ROOT / 'bridge/dinput8.dll').exists():
        subprocess.run(['make', '-s', '-C', str(ROOT / 'bridge')], check=True)
    env.pop('SIGN_CMD', None)
    subprocess.run(['bash', str(ROOT / 'tools/release.sh'), TAG], check=True, env=env, capture_output=True)


def release_json(out, zip_name, drop_sidecar=False, setup_first=False):
    base = 'https://github.com/TommoDotCommo/mdk2-coop/releases/download/%s/' % TAG

    def asset(name):
        return {'name': name, 'size': (out / name).stat().st_size, 'browser_download_url': base + name}
    assets = [asset('MDK2-Coop-Setup.exe'), asset('MDK2-Coop-Setup.exe.sha256'), asset(zip_name)]
    if not drop_sidecar:
        assets.append(asset(zip_name + '.sha256'))
    if not setup_first:
        assets.reverse()
    return json.dumps([{'tag_name': TAG, 'draft': False, 'prerelease': False, 'body': 'notes', 'assets': assets}])


def attempt(binary, out, label, zip_path, json_text):
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        (tmp / 'releases.json').write_text(json_text)
        game = tmp / 'game'
        (game / 'coop').mkdir(parents=True)
        (game / 'dinput8.dll').write_bytes(b'OLD-DLL-FILE')
        (game / 'coop' / 'coop.lua').write_bytes(b'-- old lua')
        r = subprocess.run([str(binary), str(tmp / 'releases.json'), str(zip_path), str(game), 'v0.2.0-alpha'], capture_output=True, text=True)
        return r.returncode, r.stdout.strip()


def main():
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        out = tmp / 'release'
        build_release(out)
        zip_path = out / ('mdk2-coop-%s.zip' % TAG)
        binary = tmp / 'layout_test'
        subprocess.run(['cc', '-std=c99', '-O1', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-Wno-unused-parameter',
                        '-I', str(ROOT / 'bridge'), str(ROOT / 'bridge/tests/update_layout_test.c'), '-o', str(binary)], check=True)
        failures = []
        code, text = attempt(binary, out, 'real', zip_path, release_json(out, zip_path.name))
        if code != 0:
            failures.append('real layout: ' + text)
        code, text = attempt(binary, out, 'setup-first', zip_path, release_json(out, zip_path.name, setup_first=True))
        if code != 0:
            failures.append('Setup.exe listed first confuses the updater: ' + text)
        code, _ = attempt(binary, out, 'no-sidecar', zip_path, release_json(out, zip_path.name, drop_sidecar=True))
        if code == 0:
            failures.append('mutant no-sidecar survived')
        # a zip whose top folder is wrong: rewrite the real zip under another root
        renamed = out / 'renamed'
        renamed.mkdir()
        with zipfile.ZipFile(zip_path) as src, zipfile.ZipFile(renamed / zip_path.name, 'w', zipfile.ZIP_DEFLATED) as dst:
            for info in src.infolist():
                dst.writestr(info.filename.replace('mdk2-coop-%s/' % TAG, 'setup/', 1), src.read(info))
        digest = hashlib.sha256((renamed / zip_path.name).read_bytes()).hexdigest()
        (renamed / (zip_path.name + '.sha256')).write_text('%s  %s\n' % (digest, zip_path.name))
        for f in ('MDK2-Coop-Setup.exe', 'MDK2-Coop-Setup.exe.sha256'):
            (renamed / f).write_bytes((out / f).read_bytes())
        code, _ = attempt(binary, renamed, 'root-renamed', renamed / zip_path.name, release_json(renamed, zip_path.name))
        if code == 0:
            failures.append('mutant root-renamed survived')
    if failures:
        print('\n'.join('FAIL: ' + f for f in failures))
        return 1
    print('PASS: an installed older updater finds, verifies, applies and rolls back the new release layout (3 mutants)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
