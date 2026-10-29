"""Builds small synthetic minidumps for the parser and crash-watch tests."""
import struct


def build(modules, code=0xC0000005, address=0x4672ed, regs=None, access=(0, 0x10), stack=(), esp=0x1afbe4, tid=7):
    """modules: [(base, size, 'C:\\\\path\\\\x.exe')]. Returns the bytes of a minidump."""
    base = dict(eax=1, ebx=2, ecx=3, edx=4, esi=5, edi=6, ebp=7, eip=address, esp=esp, eflags=0x246)
    base.update(regs or {})
    regs = base
    body = bytearray()
    pos = 32 + 4 * 12                       # header + 4 directory entries
    def put(blob):
        nonlocal pos
        rva = pos
        body.extend(blob)
        pos += len(blob)
        return rva
    names = []
    for _, _, name in modules:
        raw = name.encode('utf-16-le')
        names.append(put(struct.pack('<I', len(raw)) + raw + b'\0\0'))
    mods = struct.pack('<I', len(modules))
    for (base, size, _), nrva in zip(modules, names):
        mods += struct.pack('<QIIII', base, size, 0, 0, nrva) + bytes(108 - 24)
    mod_rva = put(mods)
    context = bytearray(716)
    struct.pack_into('<I', context, 0, 0x1003f)
    order = ('edi', 'esi', 'ebx', 'edx', 'ecx', 'eax', 'ebp', 'eip', 'cs', 'eflags', 'esp', 'ss')
    values = [regs.get(k, 0x23) for k in order]
    struct.pack_into('<12I', context, 0x9C, *values)
    ctx_rva = put(bytes(context))
    info = list(access) + [0] * 13
    exc = struct.pack('<II', tid, 0) + struct.pack('<IIQQI', code, 0, 0, address, len(access)) + b'\0\0\0\0' \
        + struct.pack('<15Q', *info) + struct.pack('<II', len(context), ctx_rva)
    exc_rva = put(exc)
    words = b''.join(struct.pack('<I', w) for w in stack)
    data_rva = put(words)
    mem = struct.pack('<I', 1) + struct.pack('<QII', esp, len(words), data_rva)
    mem_rva = put(mem)
    directory = struct.pack('<III', 4, len(mods), mod_rva) + struct.pack('<III', 6, len(exc), exc_rva) \
        + struct.pack('<III', 5, len(mem), mem_rva) + struct.pack('<III', 0, 0, 0)
    header = struct.pack('<4sIIIIIQ', b'MDMP', 0xA793, 4, 32, 0, 1793000000, 0)
    return header + directory + bytes(body)
