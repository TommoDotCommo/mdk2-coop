#!/usr/bin/env python3
"""Game-free release fixture: checksum, inventory and platform headers."""
import hashlib
from pathlib import Path
import sys
import zipfile

archive = Path(sys.argv[1])
checksum, name = archive.with_suffix('.zip.sha256').read_text().split()
assert name == archive.name, 'checksum names a different asset'
assert hashlib.sha256(archive.read_bytes()).hexdigest() == checksum, 'checksum mismatch'
root = archive.stem + '/'
expected = ['MDK2-Coop-Setup.exe', 'dinput8.dll', 'coop/coop.lua', 'mdk2-coop.ps1',
            'installer-gui.ps1', 'install-state.ps1', 'Couch.cmd', 'mdk2-couch.ps1',
            'README.txt', 'LICENSE.txt']
with zipfile.ZipFile(archive) as package:
    assert set(package.namelist()) == {root + f for f in expected}, 'unexpected release inventory'
    for payload in ['MDK2-Coop-Setup.exe', 'dinput8.dll']:
        assert package.read(root + payload)[:2] == b'MZ', 'not a Windows PE'
    source = Path(__file__).resolve().parents[1] / 'dist/install-state.ps1'
    assert package.read(root + 'install-state.ps1').decode().replace('\r\n', '\n') == source.read_text(), 'stale policy'
print('PASS: release checksum, exact ten-file inventory, policy payload and Windows PE headers')
