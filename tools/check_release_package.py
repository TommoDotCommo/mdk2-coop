#!/usr/bin/env python3
"""Game-free release fixture: checksum, inventory and platform headers.

Pass the .zip path; the unpacked folder of the same name (version dots included) is accepted too.
"""
import hashlib
from pathlib import Path
import sys
import zipfile

EXPECTED = ['MDK2-Coop-Setup.exe', 'dinput8.dll', 'coop/coop.lua', 'mdk2-coop.ps1',
            'installer-gui.ps1', 'install-state.ps1', 'install-policy.exe', 'Couch.cmd', 'mdk2-couch.ps1',
            'README.txt', 'LICENSE.txt']


def resolve_archive(arg):
    """The zip for an argument that is either the zip or its same-named folder.

    Appends instead of replacing a suffix: a version such as v0.3.2-alpha has dots of its own."""
    path = Path(arg)
    return path if path.name.endswith('.zip') else path.with_name(path.name + '.zip')


def check(archive, policy_source=None):
    archive = resolve_archive(archive)
    checksum, name = archive.with_name(archive.name + '.sha256').read_text().split()
    assert name == archive.name, 'checksum names a different asset'
    assert hashlib.sha256(archive.read_bytes()).hexdigest() == checksum, 'checksum mismatch'
    root = archive.name[:-len('.zip')] + '/'
    with zipfile.ZipFile(archive) as package:
        assert set(package.namelist()) == {root + f for f in EXPECTED}, 'unexpected release inventory'
        for payload in ['MDK2-Coop-Setup.exe', 'dinput8.dll', 'install-policy.exe']:
            assert package.read(root + payload)[:2] == b'MZ', 'not a Windows PE'
        source = policy_source or Path(__file__).resolve().parents[1] / 'dist/install-state.ps1'
        assert package.read(root + 'install-state.ps1').decode().replace('\r\n', '\n') == Path(source).read_text(), 'stale policy'
    return 'PASS: release checksum, exact eleven-file inventory, policy payload and Windows PE headers'


if __name__ == '__main__':
    print(check(sys.argv[1]))
