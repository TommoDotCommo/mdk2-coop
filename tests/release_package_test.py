"""The release checker accepts the zip or its same-named folder when the version has dots."""
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
import zipfile
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import check_release_package as checker

NAME = 'mdk2-coop-v0.3.2-alpha'
# a stub PE with the VERSIONINFO string the checker reads
PE = b'MZ' + 'ProductName\0MDK2 Co-op\0'.encode('utf-16-le') + 'ProductVersion\0'.encode('utf-16-le') + b'\0\0' + 'v0.3.2-alpha'.encode('utf-16-le') + b'\0\0'


def build(folder, policy='policy\n', extra=None, pe_edits=None):
    archive = folder / (NAME + '.zip')
    members = {}
    with zipfile.ZipFile(archive, 'w') as z:
        for f in checker.EXPECTED:
            data = (pe_edits or {}).get(f, PE) if f.endswith(('.exe', '.dll')) else (policy.replace('\n', '\r\n').encode() if f == 'install-state.ps1' else b'x')
            if f == 'coop-mod.json':
                data = json.dumps(checker.mod_manifest.manifest(NAME[len('mdk2-coop-'):], members)).encode()
                z.writestr('coop-mod.json', data)
            members[f] = data
            info = zipfile.ZipInfo(NAME + '/' + f)
            info.external_attr = (0o755 if f.endswith('.sh') else 0o644) << 16
            z.writestr(info, data)
        for f in extra or []:
            z.writestr(NAME + '/' + f, b'x')
    (folder / (archive.name + '.sha256')).write_text(hashlib.sha256(archive.read_bytes()).hexdigest() + '  ' + archive.name + '\n')
    # the single-file Setup beside it; its payload is read through payload.read_exe, replaced here by a stub
    setup = folder / 'MDK2-Coop-Setup.exe'
    setup.write_bytes((pe_edits or {}).get('single-file Setup', PE))
    (folder / 'MDK2-Coop-Setup.exe.sha256').write_text(hashlib.sha256(setup.read_bytes()).hexdigest() + '  MDK2-Coop-Setup.exe\n')
    blob = checker.payload.pack([(f, members[f]) for f in checker.PAYLOAD])
    checker.payload.read_exe = lambda path: (NAME[len('mdk2-coop-'):], hashlib.sha256(blob).hexdigest(), blob)
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

    def test_embedded_payload_must_equal_the_zip_and_be_intact(self):
        archive = build(self.folder)
        payload = checker.payload
        good = payload.read_exe(None)
        files = dict(payload.unpack(good[2]))
        files['dinput8.dll'] = b'MZ-different'
        blob = payload.pack(sorted(files.items()))
        payload.read_exe = lambda path: (good[0], hashlib.sha256(blob).hexdigest(), blob)
        with self.assertRaises(AssertionError):
            checker.check(archive, self.policy)       # a payload signed or built after the zip's files
        payload.read_exe = lambda path: (good[0], '0' * 64, good[2])
        with self.assertRaises(AssertionError):
            checker.check(archive, self.policy)       # payload does not match its own hash
        payload.read_exe = lambda path: ('v9', good[1], good[2])
        with self.assertRaises(AssertionError):
            checker.check(archive, self.policy)       # version differs from the zip

    def test_metadata_mutants(self):
        # Each shipped binary uses the same check, including the outer Setup.
        for field, good, bad in [('ProductName', 'MDK2 Co-op', 'Other Thing'),
                                 ('ProductVersion', 'v0.3.2-alpha', 'v9.9.9-alpha')]:
            for name in checker.SHIPPED_PE + ['single-file Setup']:
                with self.subTest(field=field, binary=name):
                    data = PE.replace(good.encode('utf-16-le'), bad.encode('utf-16-le'))
                    archive = build(self.folder, pe_edits={name: data})
                    with self.assertRaisesRegex(AssertionError, field):
                        checker.check(archive, self.policy)

    def test_signature_detection(self):
        pe = bytearray(0x400)
        pe[0:2] = b'MZ'
        pe[0x3c] = 0x80
        pe[0x80:0x84] = b'PE\0\0'
        pe[0x98] = 0x0b
        pe[0x99] = 0x01
        self.assertFalse(checker.pe_signed(bytes(pe)))
        import struct
        struct.pack_into('<II', pe, 0x80 + 24 + 96 + 32, 0x300, 0x40)
        self.assertTrue(checker.pe_signed(bytes(pe)))
        struct.pack_into('<II', pe, 0x80 + 24 + 96 + 32, 0x300, 0x400)    # table past the end of the file
        self.assertFalse(checker.pe_signed(bytes(pe)))


if __name__ == '__main__':
    unittest.main()
