#!/usr/bin/env python3
"""Game-free release fixture: checksum, inventory, platform headers, the single-file Setup's payload and signatures.

  check_release_package.py <zip> [--require-signed]   the zip (or its same-named folder) and the MDK2-Coop-Setup.exe beside it
  check_release_package.py --signed <pe file>...      exit 1 unless each PE file carries an Authenticode certificate
"""
import hashlib
from pathlib import Path
import struct
import sys
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import mod_manifest  # noqa: E402
import payload  # noqa: E402
import versioninfo  # noqa: E402

EXPECTED = ['MDK2-Coop-Setup.exe', 'dinput8.dll', 'coop/coop.lua', 'mdk2-coop.ps1',
            'installer-gui.ps1', 'install-state.ps1', 'install-policy.exe', 'Couch.cmd', 'mdk2-couch.ps1',
            'install-linux.sh', 'README.txt', 'LICENSE.txt', 'coop-mod.json']
ROOT_FILES = ['coop-mod.json']      # beside the top folder, for mod managers (the updater never looks there)
# the single-file Setup carries the zip's files except its own (payload-less) Setup.exe and the Linux script
PAYLOAD = sorted(set(EXPECTED) - {'MDK2-Coop-Setup.exe', 'install-linux.sh'})
SHIPPED_PE = ['MDK2-Coop-Setup.exe', 'dinput8.dll', 'install-policy.exe']


def resolve_archive(arg):
    """The zip for an argument that is either the zip or its same-named folder.

    Appends instead of replacing a suffix: a version such as v0.3.2-alpha has dots of its own."""
    path = Path(arg)
    return path if path.name.endswith('.zip') else path.with_name(path.name + '.zip')


def pe_signed(data):
    """True when the PE file has a non-empty certificate table (IMAGE_DIRECTORY_ENTRY_SECURITY)."""
    if data[:2] != b'MZ':
        return False
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    magic = struct.unpack_from('<H', data, pe + 24)[0]
    dirs = pe + 24 + (96 if magic == 0x10b else 112)
    offset, size = struct.unpack_from('<II', data, dirs + 4 * 8)
    return offset != 0 and size != 0 and offset + size <= len(data)


def check(archive, policy_source=None, require_signed=False):
    archive = resolve_archive(archive)
    checksum, name = archive.with_name(archive.name + '.sha256').read_text().split()
    assert name == archive.name, 'checksum names a different asset'
    assert hashlib.sha256(archive.read_bytes()).hexdigest() == checksum, 'checksum mismatch'
    root = archive.name[:-len('.zip')] + '/'
    with zipfile.ZipFile(archive) as package:
        assert set(package.namelist()) == {root + f for f in EXPECTED} | set(ROOT_FILES), 'unexpected release inventory'
        version = archive.name[len('mdk2-coop-'):-len('.zip')]
        for pe in SHIPPED_PE:
            assert package.read(root + pe)[:2] == b'MZ', 'not a Windows PE'
            assert versioninfo.read(package.read(root + pe)) == version, 'VERSIONINFO of %s is not %s' % (pe, version)
        source = policy_source or Path(__file__).resolve().parents[1] / 'dist/install-state.ps1'
        assert package.read(root + 'install-state.ps1').decode().replace('\r\n', '\n') == Path(source).read_text(), 'stale policy'
        sh = package.getinfo(root + 'install-linux.sh')
        assert (sh.external_attr >> 16) & 0o111, 'install-linux.sh lost its executable bit'
        assert b'\r' not in package.read(root + 'install-linux.sh'), 'install-linux.sh has CRLF line endings'
        if require_signed:
            for pe in SHIPPED_PE:
                assert pe_signed(package.read(root + pe)), 'zip member is not signed: ' + pe
        members = {f: package.read(root + f) for f in PAYLOAD}
    # the single-file Setup beside the zip
    setup = archive.with_name('MDK2-Coop-Setup.exe')
    checksum, name = setup.with_name(setup.name + '.sha256').read_text().split()
    assert name == setup.name, 'Setup checksum names a different asset'
    assert hashlib.sha256(setup.read_bytes()).hexdigest() == checksum, 'Setup checksum mismatch'
    assert setup.read_bytes()[:2] == b'MZ', 'Setup is not a Windows PE'
    got = payload.read_exe(setup)
    assert got, 'Setup.exe carries no payload'
    version, want_hash, blob = got
    assert hashlib.sha256(blob).hexdigest() == want_hash, 'embedded payload does not match its SHA-256'
    assert version == archive.name[len('mdk2-coop-'):-len('.zip')], 'embedded version differs from the zip'
    assert versioninfo.read(setup.read_bytes()) == version, 'VERSIONINFO of the single-file Setup is not ' + version
    mod_manifest.check(archive)
    embedded = dict(payload.unpack(blob))
    assert sorted(embedded) == PAYLOAD, 'embedded payload inventory differs'
    for f in PAYLOAD:
        assert embedded[f] == members[f], 'embedded payload differs from the zip (signed before embedding?): ' + f
    if require_signed:
        assert pe_signed(setup.read_bytes()), 'single-file Setup is not signed'
    return ('PASS: release checksums, exact inventory, coop-mod.json against the zip, VERSIONINFO, policy payload, Windows PE '
            'headers, and the single-file Setup whose embedded payload equals the zip' + (' (all executables signed)' if require_signed else ''))


if __name__ == '__main__':
    if len(sys.argv) >= 3 and sys.argv[1] == '--signed':
        bad = [f for f in sys.argv[2:] if not pe_signed(Path(f).read_bytes())]
        if bad:
            print('not signed: ' + ', '.join(bad), file=sys.stderr)
            sys.exit(1)
        sys.exit(0)
    print(check(sys.argv[1], require_signed='--require-signed' in sys.argv[2:]))
