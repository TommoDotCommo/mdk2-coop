#!/usr/bin/env python3
"""Exercise the exact native setup decisions used by PowerShell, without Windows."""
import ctypes
import itertools
from pathlib import Path
import random
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / 'dist/install-policy.h').read_text()
WRAPPER = '''#include "install-policy.h"
int file(const char *e,const char *o,int s,const char *i,const char *r,const char *p,const char *b,int u) { return ci_file(e,o,s,i,r,p,b,u); }
int actions(unsigned s,int o) { return ci_actions(s,o); }
int loader(const char *t) { return ci_loader(t); }
'''
MUTANTS = {
    'unreadable': ('unknown || !*expected', '!*expected'),
    'no-expected': ('unknown || !*expected', 'unknown'),
    'receipt-schema': ('schema != 1 || !*installed', '!*installed'),
    'empty-installed': ('schema != 1 || !*installed', 'schema != 1'),
    'backup-integrity': ('*original && strcmp(backup, original)', '0'),
    'unexpected-backup': ('!*original && *backup', '0'),
    'orphan-backup': ('if (*backup) return CI_CONFLICT;', 'if (0) return CI_CONFLICT;'),
    'foreign-modification': ('return CI_MODIFIED;', 'return CI_OUTDATED;'),
    'unowned-equality': ('return CI_UNMANAGED;', 'return CI_CURRENT;'),
    'pending-recovery': ('(*pending && !strcmp(observed, pending))', '0'),
    'empty-action-input': ('!states || ', ''),
    'unknown-action-input': ('(states & ~127u) || ', ''),
    'blocked-actions': ('return 32;', 'return 16;'),
    'update-action': ('action = 2;', 'action = 3;'),
    'repair-action': ('action = 3;', 'action = 1;'),
    'ownership': ('(owned ? 16 : 0)', '16'),
    'loader-prefix': ("a != text && a[-1] != '\\n'", '0'),
    'loader-suffix': ("*p && *p != '\\n'", '0'),
    'loader-duplicates': ('strstr(a + strlen(begin), begin)', '0'),
    'loader-end-duplicates': ('strstr(b + strlen(end), end)', '0'),
    'loader-body': ('strncmp(p, body, strlen(body))', '0'),
}

def oracle(e, o, s, i, r, p, b, u):
    if u or not e: return 5
    if s:
        if s != 1 or not i: return 5
        if (r and b != r) or (not r and b): return 4
        if not o: return 1
        if o == e: return 0
        if o == i or (p and o == p): return 2
        if r and o == r: return 1
        return 3
    if b: return 4
    return 1 if not o else 6


def verify(lib):
    lib.file.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int] + [ctypes.c_char_p]*4 + [ctypes.c_int]
    lib.loader.argtypes = [ctypes.c_char_p]
    checks = 0
    for e,o,s,i,r,p,b,u in itertools.product([b'',b'new'], [b'',b'new',b'old',b'orig',b'pending',b'foreign'], [0,1,2], [b'',b'old'], [b'',b'orig'], [b'',b'pending'], [b'',b'orig',b'bad'], [0,1]):
        assert lib.file(e,o,s,i,r,p,b,u) == oracle(e,o,s,i,r,p,b,u), (e,o,s,i,r,p,b,u)
        checks += 1
    for mask, owned in itertools.product(range(256), range(2)):
        blocked = not mask or mask & ~127 or mask & ((1<<3)|(1<<4)|(1<<5))
        action = 0
        if not blocked:
            if not owned and mask & ((1<<1)|(1<<6)): action = 1
            elif mask & (1<<2): action = 2
            elif mask & ((1<<1)|(1<<6)): action = 3
        expected = 32 if blocked else action | (16 if owned else 0)
        assert lib.actions(mask, owned) == expected, (mask,owned)
        checks += 1
    block = b'-- >>> mdk2-coop >>>\nif (coop_init) then coop_init() end\n-- <<< mdk2-coop <<<'
    for newline in (b'\n',b'\r\n'):
        exact = block.replace(b'\n',newline)
        for text in (exact, exact+newline, b'option=1'+newline+exact+newline+b'option=2'):
            assert lib.loader(text) == 0, text
        for text in (b'prefix'+exact,exact+b'evil()',exact+exact,exact+newline+b'-- >>> mdk2-coop >>>',exact+newline+b'-- <<< mdk2-coop <<<',exact.replace(b'coop_init()',b'noop_init()'),exact.split(newline)[0],exact.split(newline)[-1]):
            assert lib.loader(text) == 4, text
    assert lib.loader(b'options=1\n') == 1
    # Seeded arbitrary input: no crashes, classifications are always defined.
    rng = random.Random(43)
    for _ in range(10000):
        text = bytes(rng.randrange(1,256) for _ in range(rng.randrange(256)))
        assert lib.loader(text) in (0,1,4)
    return checks

with tempfile.TemporaryDirectory() as temp:
    directory = Path(temp)
    (directory/'wrapper.c').write_text(WRAPPER)
    def build(source, name):
        (directory/'install-policy.h').write_text(source)
        binary = directory/(name+'.so')
        subprocess.run(['cc','-shared','-fPIC','-std=c99','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-I',str(directory),str(directory/'wrapper.c'),'-o',str(binary)], check=True, capture_output=True)
        return ctypes.CDLL(str(binary))
    print('PASS:',verify(build(SOURCE,'good')),'policy decisions and 10000 fuzz inputs')
    for name,(before,after) in MUTANTS.items():
        assert SOURCE.count(before) == 1, name
        lib = build(SOURCE.replace(before,after), name)
        try: verify(lib)
        except AssertionError: print('KILLED:',name)
        else: raise AssertionError('SURVIVED: '+name)
    subprocess.run(['cc','-std=c99','-Wall','-Wextra','-Werror',str(ROOT/'dist/install-policy.c'),'-o',str(directory/'cli')],check=True)
    def cli(*args):
        return subprocess.run([str(directory/'cli'),*args],capture_output=True)
    assert cli('file','new','old','1','old','-','-','-','0').stdout == b'2\n'
    assert cli('actions','8','1').stdout == b'32\n'
    assert cli('loader',b'-- >>> mdk2-coop >>>\nif (coop_init) then coop_init() end\n-- <<< mdk2-coop <<<'.hex()).stdout == b'0\n'
    for text in ('00','1','zz','ff'*8193):
        assert cli('loader',text).returncode == 2
    print('PASS: command adapter parity and malformed/oversized hex rejection')
