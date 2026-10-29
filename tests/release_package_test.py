"""The release checker accepts the zip or its same-named folder when the version has dots."""
import hashlib
from pathlib import Path
import sys
import tempfile
import unittest
import zipfile
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import check_release_package as checker

NAME = 'mdk2-coop-v0.3.2-alpha'


def build(folder, policy='policy\n', extra=None):
    archive = folder / (NAME + '.zip')
    with zipfile.ZipFile(archive, 'w') as z:
        for f in checker.EXPECTED:
            data = b'MZ' if f.endswith(('.exe', '.dll')) else (policy.replace('\n', '\r\n').encode() if f == 'install-state.ps1' else b'x')
            z.writestr(NAME + '/' + f, data)
        for f in extra or []:
            z.writestr(NAME + '/' + f, b'x')
    (folder / (archive.name + '.sha256')).write_text(hashlib.sha256(archive.read_bytes()).hexdigest() + '  ' + archive.name + '\n')
    return archive


class ReleasePackage(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.folder = Path(self.tmp.name)
        self.policy = self.folder / 'install-state.ps1'
        self.policy.write_text('policy\n')

    def tearDown(self):
        self.tmp.cleanup()

    def test_zip_path(self):
        archive = build(self.folder)
        self.assertIn('PASS', checker.check(archive, self.policy))

    def test_folder_path_with_version_dots(self):
        build(self.folder)
        staged = self.folder / NAME
        staged.mkdir()
        self.assertEqual(checker.resolve_archive(staged), self.folder / (NAME + '.zip'))
        self.assertIn('PASS', checker.check(staged, self.policy))

    def test_extra_file_stale_policy_and_bad_checksum_fail(self):
        build(self.folder, extra=['stray.txt'])
        with self.assertRaises(AssertionError):
            checker.check(self.folder / (NAME + '.zip'), self.policy)
        build(self.folder, policy='old\n')
        with self.assertRaises(AssertionError):
            checker.check(self.folder / (NAME + '.zip'), self.policy)
        archive = build(self.folder)
        (self.folder / (archive.name + '.sha256')).write_text('0' * 64 + '  ' + archive.name + '\n')
        with self.assertRaises(AssertionError):
            checker.check(archive, self.policy)


if __name__ == '__main__':
    unittest.main()
