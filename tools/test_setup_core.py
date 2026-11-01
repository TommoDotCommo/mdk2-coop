#!/usr/bin/env python3
"""Native tests of the portable cores behind the one-file Setup, with mutants that must fail:
  dist/setup-core.h      payload container, names inside it, "started from inside a zip"
  bridge/build_sigs.h    game-build recognition from an exe on disk (via the install-policy helper's `build` command)
No Windows and no game: synthetic PE fixtures from tools/fixtures/make_build_fixtures.py."""
import ctypes
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
sys.path.insert(0, str(ROOT / 'tools/fixtures'))
import make_build_fixtures as fx  # noqa: E402
import payload  # noqa: E402

CORE = (ROOT / 'dist/setup-core.h').read_text()
SIGS = (ROOT / 'bridge/build_sigs.h').read_text()
WRAPPER = '''#include "setup-core.h"
int name_ok(const char *s) { return sc_name_ok(s, strlen(s)); }
int in_zip(const wchar_t *e, const wchar_t *t) { return sc_run_from_zip(e, t); }
int parse(const unsigned char *b, size_t n) { struct sc_file f[SC_MAX_FILES]; return sc_parse(b, n, f); }
'''

CORE_MUTANTS = {
    'name-dotdot': [("name[start + n - 1] == '.'", '0')],
    'name-empty-component': [('if (!n || ', 'if (')],
    'name-backslash': [("|| c == '.' || c == '_' || c == '-'))", "|| c == '.' || c == '_' || c == '-' || c == '\\\\' || c == ':'))")],
    'parse-trailing-bytes': [('return pos == len ? (int)count : -1;', 'return (int)count;')],
    'parse-duplicates': [('if (sc_ieq(out[j].name, out[i].name)) return -1;', '')],
    'parse-no-bounds': [('|| pos + size > len) return -1;', ') return -1;'), ('return pos == len ? (int)count : -1;', 'return (int)count;')],
    'parse-name-check': [('if (!sc_name_ok((const char *)blob + pos, nlen)) return -1;', '')],
    'parse-magic': [('memcmp(blob, "MCP1", 4) != 0', '0')],
    'zip-component': [('sc_fold(exe[end - 1]) == L\'p\') return 1;', 'sc_fold(exe[end - 1]) == L\'p\') return 0;')],
    'zip-case': [('sc_fold(exe[end - 4]) == L\'.\'', 'exe[end - 4] == L\'.\' && exe[end - 3] == L\'z\'')],
}


def build_lib(tmp, header_text):
    tmp = Path(tmp)
    (tmp / 'setup-core.h').write_text(header_text)
    (tmp / 'w.c').write_text(WRAPPER)
    subprocess.run(['gcc', '-shared', '-fPIC', '-O1', '-o', str(tmp / 'w.so'), str(tmp / 'w.c')], check=True)
    lib = ctypes.CDLL(str(tmp / 'w.so'))
    lib.in_zip.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p]
    lib.name_ok.argtypes = [ctypes.c_char_p]
    lib.parse.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
    return lib


def core_failures(lib):
    bad = []

    def expect(cond, what):
        if not cond:
            bad.append(what)
    for good in ['dinput8.dll', 'coop/coop.lua', 'a-b_c.1/x.y', 'Couch.cmd']:
        expect(lib.name_ok(good.encode()), 'accepts ' + good)
    for evil in ['', '../x', 'a/../b', '/abs', 'a//b', 'a\\b', 'C:x', 'a/./b', 'x.', 'a b', 'ü', '.', '..', 'a/', './a', 'x' * 96]:
        expect(not lib.name_ok(evil.encode()), 'rejects ' + repr(evil))
    ok = payload.pack([('a.txt', b'one'), ('coop/b.lua', b'two' * 100), ('empty', b'')])
    expect(lib.parse(ok, len(ok)) == 3, 'parses a good container')
    for name, blob in {
        'bad magic': b'XXXX' + ok[4:], 'trailing byte': ok + b'x', 'truncated': ok[:-1], 'empty': b'', 'zero files': b'MCP1\0\0\0\0',
        'huge count': b'MCP1\xff\xff\xff\xff' + ok[8:],
        'dotdot name': payload.pack([('a', b'x')]).replace(b'\x01\x00a', b'\x02\x00..'),
        'size past end': ok[:8] + ok[8:].replace(b'\x03\x00\x00\x00one', b'\xff\xff\x00\x00one', 1),
    }.items():
        expect(lib.parse(blob, len(blob)) == -1, 'rejects ' + name)
    dup = b'MCP1' + (2).to_bytes(4, 'little') + b''.join(
        len(n).to_bytes(2, 'little') + n + (1).to_bytes(4, 'little') + b'x' for n in (b'Abc', b'aBC'))
    expect(lib.parse(dup, len(dup)) == -1, 'rejects names that differ only in case')
    temp = 'C:\\Users\\p\\AppData\\Local\\Temp'
    cases = [
        (temp + '\\Temp1_mdk2-coop-v1.zip\\mdk2-coop-v1\\MDK2-Coop-Setup.exe', 1, 'Explorer opening the zip'),
        (temp + '\\Temp1_X.ZIP\\Setup.exe', 1, 'upper-case .ZIP'),
        (temp.lower() + '\\temp1_x.zip\\setup.exe', 1, 'case-insensitive temp prefix'),
        (temp + '\\x.zip\\a\\b\\Setup.exe', 1, 'zip folder higher up'),
        ('C:\\Users\\p\\Downloads\\mdk2-coop-v1\\MDK2-Coop-Setup.exe', 0, 'extracted folder'),
        ('C:\\Users\\p\\Downloads\\x.zip\\Setup.exe', 0, 'a .zip folder outside temp is not Explorer\'s'),
        (temp + '\\unpacked\\Setup.exe', 0, 'temp but no zip component'),
        (temp + '\\Setup.zip', 0, 'the exe itself named .zip is not a zip folder'),
        ('C:\\Users\\p\\AppData\\Local\\Temp2\\x.zip\\Setup.exe', 0, 'temp prefix must end at a separator'),
    ]
    for exe, want, what in cases:
        expect(lib.in_zip(exe, temp) == want, 'run-from-zip: ' + what)
        expect(lib.in_zip(exe, temp + '\\') == want, 'run-from-zip, temp with trailing slash: ' + what)
    return bad


def policy_build(tmp, sigs_text, exe_path):
    tmp = Path(tmp)
    (tmp / 'dist').mkdir(exist_ok=True)
    (tmp / 'bridge').mkdir(exist_ok=True)
    shutil.copy(ROOT / 'dist/install-policy.c', tmp / 'dist')
    shutil.copy(ROOT / 'dist/install-policy.h', tmp / 'dist')
    (tmp / 'bridge/build_sigs.h').write_text(sigs_text)
    subprocess.run(['gcc', '-O1', '-o', str(tmp / 'policy'), str(tmp / 'dist/install-policy.c')], check=True)
    return tmp / 'policy'


def identify(policy, path):
    r = subprocess.run([str(policy), 'build', str(path)], capture_output=True, text=True)
    return r.returncode, r.stdout.split()


SIG_MUTANTS = {
    'pointer-not-checked': ('cb_u32(f + e) == CB_SIGS[i].name_va &&', ''),
    'string-not-checked': ('memcmp(f + n, "copytagmethods", 15) == 0', '1'),
    'string-prefix-only': ('memcmp(f + n, "copytagmethods", 15) == 0', 'memcmp(f + n, "copytagmethods", 8) == 0'),
    'any-pe-magic': ('cb_u16(f + pe + 24) != 0x10b', '0'),
    'section-bounds': ('rva - vaddr < span &&', ''),
    'steam-address': ('0x506520', '0x506524'),
    'overhaul-address': ('0x4d769c', '0x4d769d'),
}


def sig_failures(policy, files):
    bad = []
    want = {'steam.exe': (['0', 'Steam', 'build', '3902']), 'overhaul.exe': ['1', 'Overhaul', 'Games', 'build']}
    for name, path in files.items():
        code, out = identify(policy, path)
        if name in want:
            if code != 0 or out != want[name]:
                bad.append('%s is not recognised: %s %s' % (name, code, out))
        elif code != 0 or out[:1] != ['-1']:
            bad.append('%s must be unknown, got %s %s' % (name, code, out))
    if identify(policy, files['steam.exe'].parent / 'no-such-file')[0] != 2:
        bad.append('an unreadable file must exit 2')
    return bad


def main():
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        for i, text in enumerate([CORE] + [None] * len(CORE_MUTANTS)):
            d = tmp / ('core%d' % i)
            d.mkdir()
            if text is None:
                name, edits = list(CORE_MUTANTS.items())[i - 1]
                mutated = CORE
                for old, new in edits:
                    assert old in mutated, 'mutant %s does not match setup-core.h' % name
                    mutated = mutated.replace(old, new, 1)
                bad = core_failures(build_lib(d, mutated))
                if not bad:
                    print('FAIL: mutant %s survived' % name)
                    failures += 1
            else:
                bad = core_failures(build_lib(d, text))
                for b in bad:
                    print('FAIL:', b)
                failures += len(bad)
        files = {}
        fdir = tmp / 'fx'
        fdir.mkdir()
        for name, data in fx.variants().items():
            (fdir / name).write_bytes(data)
            files[name] = fdir / name
        bad = sig_failures(policy_build(_mk(tmp / 'p0'), SIGS, None), files)
        for b in bad:
            print('FAIL:', b)
        failures += len(bad)
        for i, (name, (old, new)) in enumerate(SIG_MUTANTS.items()):
            assert old in SIGS, 'mutant %s does not match build_sigs.h' % name
            policy = policy_build(_mk(tmp / ('p%d' % (i + 1))), SIGS.replace(old, new, 1), None)
            if not sig_failures(policy, files):
                print('FAIL: build-signature mutant %s survived' % name)
                failures += 1
        # one source of truth: the DLL's table is built from the same macros
        dll = (ROOT / 'bridge/dinput8.c').read_text()
        if 'COOP_BUILD_STEAM' not in dll or 'COOP_BUILD_OVERHAUL' not in dll or re.search(r'0x506520|0x4d7d34|0x505510|0x4d769c', dll):
            print('FAIL: bridge/dinput8.c must take the build signatures from build_sigs.h')
            failures += 1
    if failures:
        print('%d failure(s)' % failures)
        return 1
    print('PASS: setup core (names, container, run-from-zip) and build signatures with %d + %d mutants' % (len(CORE_MUTANTS), len(SIG_MUTANTS)))
    return 0


def _mk(path):
    path.mkdir(parents=True)
    return path


if __name__ == '__main__':
    sys.exit(main())
