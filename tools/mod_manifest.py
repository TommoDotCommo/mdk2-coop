#!/usr/bin/env python3
"""coop-mod.json: what the package is, for Setup, the updater, diagnostics and mod managers (co-op mod standard J5).

  mod_manifest.py <version> <staged package folder>   prints the manifest of the staged release (tools/release.sh)
  mod_manifest.py --check <zip>                       checks the manifest in a release zip against the zip's files
  mod_manifest.py --nexus-note <zip>                  prints the copy note for Nexus Mods (same zip, same version)

"files" lists what the package puts into the game folder (the paths are relative to it, with '/'), with SHA-256 and size.
A copy of these files is a complete install and deleting them a complete uninstall; Setup's receipts and the updater use the
same list.
"""
import hashlib
import json
from pathlib import Path
import sys
import zipfile

# the game folder's files of the package; everything else in the zip is Setup, documentation or the Linux script
GAME_FILES = ['dinput8.dll', 'coop/coop.lua']
# game builds the mod switches on for (bridge/build_sigs.h decides at run time; these are the exe files seen so far)
BUILDS = [
    {'name': 'Overhaul Games build', 'sha256': '6b8b2400988cdd11ed2c0b701ad382b613e83c88b6d243446f51e0bd541aa956'},
    {'name': 'Steam build 3902', 'sha256': None},
]
KEYS = ['schema', 'id', 'name', 'version', 'game', 'proxy', 'proton', 'files', 'user_data', 'homepage', 'updates']


def manifest(version, files):
    """files: {relative path: bytes} of GAME_FILES."""
    return {
        'schema': 1,
        'id': 'mdk2-coop',
        'name': 'MDK2 Co-op',
        'version': version,
        'game': {'name': 'MDK2 HD', 'steam_appid': 213350, 'exe': ['mdk2hd.exe'], 'builds': BUILDS},
        'proxy': {'file': 'dinput8.dll', 'system_dll': 'dinput8.dll', 'chain_file': 'dinput8.chain.dll'},
        'proton': {'dll_overrides': {'dinput8': 'native,builtin'}},
        'files': [{'path': p, 'sha256': hashlib.sha256(files[p]).hexdigest(), 'size': len(files[p])} for p in GAME_FILES],
        'user_data': '%LOCALAPPDATA%\\mdk2-coop',
        'homepage': 'https://github.com/TommoDotCommo/mdk2-coop',
        'updates': {'github': 'TommoDotCommo/mdk2-coop', 'nexus': None},
    }


def check(zip_path):
    """The manifest at the zip root and in the top folder: identical, schema-complete, and every listed file's hash and size
    equal the zip's member. Returns the manifest; raises AssertionError."""
    zip_path = Path(zip_path)
    root = zip_path.name[:-len('.zip')] + '/'
    with zipfile.ZipFile(zip_path) as z:
        inner = z.read(root + 'coop-mod.json')
        outer = z.read('coop-mod.json')
        assert inner == outer, 'the two coop-mod.json copies differ'
        m = json.loads(inner)
        assert list(m) == KEYS, 'coop-mod.json keys differ from the standard: %s' % list(m)
        assert m['schema'] == 1 and m['id'] == 'mdk2-coop', 'coop-mod.json schema or id'
        assert m['version'] == zip_path.name[len('mdk2-coop-'):-len('.zip')], 'coop-mod.json version differs from the zip name'
        assert sorted(m['game']) == ['builds', 'exe', 'name', 'steam_appid'], 'coop-mod.json game keys'
        assert sorted(m['proxy']) == ['chain_file', 'file', 'system_dll'], 'coop-mod.json proxy keys'
        assert sorted(m['updates']) == ['github', 'nexus'], 'coop-mod.json updates keys'
        assert [f['path'] for f in m['files']] == GAME_FILES, 'coop-mod.json file list'
        for f in m['files']:
            assert sorted(f) == ['path', 'sha256', 'size'], 'coop-mod.json file entry keys'
            data = z.read(root + f['path'])
            assert hashlib.sha256(data).hexdigest() == f['sha256'], 'coop-mod.json hash does not match the zip: ' + f['path']
            assert len(data) == f['size'], 'coop-mod.json size does not match the zip: ' + f['path']
    return m


def nexus_note(zip_path):
    zip_path = Path(zip_path)
    m = check(zip_path)
    digest = hashlib.sha256(zip_path.read_bytes()).hexdigest()
    return ('MDK2 Co-op %(v)s for Nexus Mods (copy note; nothing is uploaded by the release script)\n\n'
            'File to upload: %(zip)s (the same file as on GitHub Releases, unchanged)\n'
            'SHA-256: %(sha)s\n'
            'Version field: %(v)s (exactly this string; the in-game updater and Setup compare it)\n'
            'Category: Miscellaneous. Requirements: MDK2 HD (Steam build 3902 or the Overhaul Games build).\n\n'
            'Mod manager notes: the zip has one top folder, %(top)s/, which holds the game-folder files (dinput8.dll and\n'
            'coop/coop.lua) next to Setup and the README. A mod manager that installs the top folder into the game folder\n'
            'makes a complete install; Setup then shows "Installed by a mod manager" and the in-game updater leaves the\n'
            'files to the manager. coop-mod.json lists the game-folder files with their SHA-256.\n\n'
            'Canonical release: https://github.com/TommoDotCommo/mdk2-coop/releases/tag/%(v)s\n'
            % {'v': m['version'], 'zip': zip_path.name, 'sha': digest, 'top': zip_path.name[:-len('.zip')]})


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == '--check':
        try:
            check(sys.argv[2])
        except (AssertionError, KeyError) as e:
            print('FAIL: %s' % e, file=sys.stderr)
            sys.exit(1)
        print('PASS: coop-mod.json matches the zip')
    elif len(sys.argv) == 3 and sys.argv[1] == '--nexus-note':
        sys.stdout.write(nexus_note(sys.argv[2]))
    elif len(sys.argv) == 3:
        stage = Path(sys.argv[2])
        print(json.dumps(manifest(sys.argv[1], {p: (stage / p).read_bytes() for p in GAME_FILES}), indent=2))
    else:
        print(__doc__, file=sys.stderr)
        sys.exit(2)
