#!/usr/bin/env python3
"""Local stand-in for SignPath's action download, not cryptographic signing.

fake_signpath.py A|B <prepared release directory> <output artifact directory> <version>
Asserts the input at each approval boundary and returns the same flat layout as upload-artifact.
"""
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import check_release_package as checker
import payload

phase, release, output, version = sys.argv[1:]
release, output = Path(release), Path(output)
stage = release / ('mdk2-coop-' + version)
if phase == 'A':
    assert not (stage / 'coop-mod.json').exists(), 'stage A must precede hashing the payload'
    assert not (release / 'MDK2-Coop-Setup.exe').exists(), 'stage A must precede building the outer Setup'
    inputs = [stage / f for f in checker.SHIPPED_PE]
elif phase == 'B':
    setup = release / 'MDK2-Coop-Setup.exe'
    got = payload.read_exe(setup)
    assert got and got[0] == version, 'stage B needs the assembled payload'
    embedded = dict(payload.unpack(got[2]))
    for name in ('dinput8.dll', 'install-policy.exe'):
        assert checker.pe_signed(embedded[name]), 'stage B cannot precede stage A'
        assert embedded[name] == (stage / name).read_bytes(), 'stage A signed bytes must be embedded'
    inputs = [setup]
else:
    raise AssertionError('expected A or B')
output.mkdir(parents=True, exist_ok=True)
for source in inputs:
    assert not checker.pe_signed(source.read_bytes()), 'input already signed'
    checker.check_metadata(source.read_bytes(), version, source.name)
    target = output / source.name
    shutil.copyfile(source, target)
    subprocess.run([sys.executable, str(ROOT / 'tools/fixtures/fake_sign.py'), str(target)], check=True)
print('PASS: fake SignPath stage ' + phase)
