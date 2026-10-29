#!/usr/bin/env python3
"""Small Windows minidump reader for crash dumps written by MiniDumpWriteDump.

Reads the exception record, the faulting thread's x86 register context, the module
list and the memory ranges that are present (the faulting stack in particular).
No debugger is needed. The format is documented by Microsoft (MINIDUMP_HEADER,
MINIDUMP_DIRECTORY, MINIDUMP_EXCEPTION_STREAM, MINIDUMP_MODULE).

    python3 tools/minidump.py dump0000.dmp [--stack N] [--bytes ADDR LEN]
    python3 tools/minidump.py --summary dump*.dmp
"""
import struct
import sys

STREAM_THREADS, STREAM_MODULES, STREAM_MEMORY, STREAM_EXCEPTION = 3, 4, 5, 6
STREAM_SYSTEM, STREAM_MEMORY64 = 7, 9
EXCEPTION_NAMES = {
    0xC0000005: 'ACCESS_VIOLATION', 0xC000001D: 'ILLEGAL_INSTRUCTION',
    0xC0000094: 'INT_DIVIDE_BY_ZERO', 0xC00000FD: 'STACK_OVERFLOW',
    0xC0000409: 'STACK_BUFFER_OVERRUN', 0x80000003: 'BREAKPOINT',
    0xC0000374: 'HEAP_CORRUPTION', 0xE06D7363: 'C++_EXCEPTION',
}
X86_REGS = ('edi', 'esi', 'ebx', 'edx', 'ecx', 'eax', 'ebp', 'eip', 'cs', 'eflags', 'esp', 'ss')


class MinidumpError(ValueError):
    pass


class Minidump:
    def __init__(self, data):
        self.data = data
        if len(data) < 32 or data[:4] != b'MDMP':
            raise MinidumpError('not a minidump')
        _sig, _ver, count, dir_rva, _crc, self.timestamp, _flags = struct.unpack_from('<4sIIIIIQ', data, 0)
        self.streams = {}
        for i in range(count):
            kind, size, rva = struct.unpack_from('<III', data, dir_rva + i * 12)
            if rva + size > len(data) and kind != STREAM_MEMORY64:
                raise MinidumpError('stream %d beyond end of file' % kind)
            self.streams[kind] = (rva, size)
        self.ranges = []        # (start address, file offset, length)
        self._read_memory()
        self.modules = self._read_modules()
        self.exception = self._read_exception()

    def _read_memory(self):
        if STREAM_MEMORY in self.streams:
            rva, _ = self.streams[STREAM_MEMORY]
            n = struct.unpack_from('<I', self.data, rva)[0]
            for i in range(n):
                start, size, off = struct.unpack_from('<QII', self.data, rva + 4 + i * 16)
                self.ranges.append((start, off, size))
        if STREAM_MEMORY64 in self.streams:
            rva, _ = self.streams[STREAM_MEMORY64]
            n, base = struct.unpack_from('<QQ', self.data, rva)
            pos = base
            for i in range(n):
                start, size = struct.unpack_from('<QQ', self.data, rva + 16 + i * 16)
                self.ranges.append((start, pos, size))
                pos += size

    def _string(self, rva):
        n = struct.unpack_from('<I', self.data, rva)[0]
        return self.data[rva + 4:rva + 4 + n].decode('utf-16-le', 'replace')

    def _read_modules(self):
        if STREAM_MODULES not in self.streams:
            return []
        rva, _ = self.streams[STREAM_MODULES]
        n = struct.unpack_from('<I', self.data, rva)[0]
        mods = []
        for i in range(n):
            base, size, _chk, stamp, name = struct.unpack_from('<QIIII', self.data, rva + 4 + i * 108)
            mods.append({'base': base, 'size': size, 'timestamp': stamp, 'name': self._string(name)})
        return mods

    def _read_exception(self):
        if STREAM_EXCEPTION not in self.streams:
            return None
        rva, _ = self.streams[STREAM_EXCEPTION]
        tid, _pad = struct.unpack_from('<II', self.data, rva)
        code, flags, _chain, address, nparams = struct.unpack_from('<IIQQI', self.data, rva + 8)
        info = struct.unpack_from('<15Q', self.data, rva + 8 + 32)
        ctx_size, ctx_rva = struct.unpack_from('<II', self.data, rva + 8 + 32 + 120)
        regs = {}
        if ctx_size >= 0xCC:
            # x86 CONTEXT: flags, dr0-3, dr6, dr7, FloatSave (112 bytes), gs fs es ds, then the integer set.
            seg = struct.unpack_from('<4I', self.data, ctx_rva + 0x8C)
            vals = struct.unpack_from('<12I', self.data, ctx_rva + 0x9C)
            regs = dict(zip(X86_REGS, vals))
            regs.update(zip(('gs', 'fs', 'es', 'ds'), seg))
        return {'thread': tid, 'code': code, 'flags': flags, 'address': address,
                'info': info[:min(nparams, 15)], 'regs': regs}

    def read(self, address, length):
        """Bytes at a virtual address, or None when any part is not in the dump."""
        out = b''
        while length > 0:
            for start, off, size in self.ranges:
                if start <= address < start + size:
                    take = min(length, start + size - address)
                    out += self.data[off + address - start:off + address - start + take]
                    address += take
                    length -= take
                    break
            else:
                return None
        return out

    def u32(self, address):
        raw = self.read(address, 4)
        return None if raw is None else struct.unpack('<I', raw)[0]

    def module_for(self, address):
        for m in self.modules:
            if m['base'] <= address < m['base'] + m['size']:
                return m
        return None

    def locate(self, address):
        m = self.module_for(address)
        if not m:
            return '0x%08x' % address
        return '%s+0x%x' % (m['name'].replace('\\', '/').rsplit('/', 1)[-1], address - m['base'])

    def stack_words(self, count=16):
        regs = self.exception['regs'] if self.exception else {}
        esp = regs.get('esp')
        words = []
        for i in range(count):
            w = self.u32(esp + i * 4) if esp is not None else None
            if w is None:
                break
            words.append(w)
        return words


def describe(dump, stack=16):
    ex = dump.exception
    lines = []
    if not ex:
        return ['no exception stream']
    name = EXCEPTION_NAMES.get(ex['code'], 'unknown')
    lines.append('exception 0x%08x %s at %s thread %d' % (ex['code'], name, dump.locate(ex['address']), ex['thread']))
    if ex['code'] == 0xC0000005 and len(ex['info']) >= 2:
        lines.append('  %s address 0x%x' % ({0: 'reading', 1: 'writing', 8: 'executing'}.get(ex['info'][0], 'accessing'), ex['info'][1]))
    r = ex['regs']
    if r:
        lines.append('  ' + ' '.join('%s=%08x' % (k, r[k]) for k in ('eax', 'ebx', 'ecx', 'edx', 'esi', 'edi', 'ebp', 'esp', 'eip', 'eflags')))
    for i, w in enumerate(dump.stack_words(stack)):
        lines.append('  stack +%02x %08x %s' % (i * 4, w, dump.locate(w) if dump.module_for(w) else ''))
    return lines


def main(argv):
    args = [a for a in argv if not a.startswith('--')]
    summary = '--summary' in argv
    stack = 16
    if '--stack' in argv:
        i = argv.index('--stack')
        stack = int(argv[i + 1])
        args = [a for a in args if a != argv[i + 1]]
    for path in args:
        with open(path, 'rb') as f:
            dump = Minidump(f.read())
        print(path)
        for line in describe(dump, 0 if summary else stack):
            print(line)
        if not summary:
            for m in dump.modules:
                print('  module %08x %8x %s' % (m['base'], m['size'], m['name']))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
