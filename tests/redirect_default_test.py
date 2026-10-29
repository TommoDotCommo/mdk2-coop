"""The per-instance user data redirect is a test-copy feature: a player's install never turns it on.

Off is the absence of [test] save_dir in coop.ini (bridge/redirect.c reads an empty default; the
native test in bridge/tests/redirect_test.c covers "off" in the DLL). Nothing a player installs may
write that key: not the example configuration, not the installers, not the release payload."""
import os
from pathlib import Path
import re
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
KEY = re.compile(rb'save_dir', re.I)


class RedirectOffByDefault(unittest.TestCase):
    def test_dll_default_is_empty(self):
        src = (ROOT / 'bridge' / 'redirect.c').read_text()
        self.assertRegex(src, r'GetPrivateProfileStringA\("test", "save_dir", "",')

    def test_player_files_never_mention_the_key(self):
        offenders = []
        for path in sorted((ROOT / 'dist').rglob('*')):
            if path.is_file() and KEY.search(path.read_bytes()):
                offenders.append(str(path.relative_to(ROOT)))
        self.assertEqual(offenders, [])

    def test_example_configuration_has_no_test_section(self):
        text = (ROOT / 'dist' / 'coop.ini.example').read_text()
        self.assertNotRegex(text, r'(?im)^\s*\[test\]')

    def test_only_the_test_installer_writes_it_and_only_when_isolating(self):
        writers = []
        for path in sorted((ROOT / 'tools').rglob('*.sh')):
            if re.search(r'save_dir=', path.read_text(errors='replace')):
                writers.append(path.name)
        self.assertNotIn('release.sh', writers)
        install = (ROOT / 'tools' / 'install.sh').read_text()
        self.assertRegex(install, r'if mdk_isolated; then sd=\$\'save_dir=profile')   # the one writer, gated
        release = (ROOT / 'tools' / 'release.sh').read_text()
        self.assertNotIn('install.sh', release)          # the player package is not built from the test installer
        self.assertNotRegex(release, r'save_dir')

    def test_built_package_payload_is_free_of_the_key(self):
        """With MDK2_RELEASE_ZIP=<zip> (set by the release check): no text member names the key, and the
        installers' own embedded configuration text does not either."""
        archive = os.environ.get('MDK2_RELEASE_ZIP')
        if not archive:
            self.skipTest('no MDK2_RELEASE_ZIP')
        with zipfile.ZipFile(archive) as z:
            bad = []
            for name in z.namelist():
                if name.endswith(('.dll',)):
                    continue                       # the proxy reads the key; that is the off switch
                data = z.read(name)
                if KEY.search(data) or KEY.search(data.decode('utf-16-le', 'ignore').encode()):
                    bad.append(name)
        self.assertEqual(bad, [])


if __name__ == '__main__':
    unittest.main()
