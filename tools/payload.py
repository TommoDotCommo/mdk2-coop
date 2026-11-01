#!/usr/bin/env python3
"""The one-file Setup's payload: pack the release files into the container dist/setup-core.h reads, write the
resource script that embeds it, and read a payload back out of a built exe (for checks and tests).

  payload.py pack <outdir> <version> <name=path>...   writes payload.bin, payload.sha256, version.txt, setup.rc
  payload.py list <exe>                               lists the files embedded in a Setup exe
  payload.py extract <exe> <dir>                      writes them (dir must not exist)
"""
import hashlib
import os
import re
import struct
import sys
from pathlib import Path

RES_PAYLOAD, RES_VERSION, RES_SHA256 = 10, 11, 12
NAME = re.compile(r'^[A-Za-z0-9._-]+(/[A-Za-z0-9._-]+)*$')


def pack(files):
    """files: ordered list of (name, bytes) -> container blob."""
    out = [b'MCP1', struct.pack('<I', len(files))]
    seen = set()
    for name, data in files:
        if not NAME.match(name) or any(p in ('.', '..') for p in name.split('/')) or name.endswith('.') or len(name) >= 96:
            raise ValueError('bad payload name: %r' % name)
        if name.lower() in seen:
            raise ValueError('duplicate payload name: %r' % name)
        seen.add(name.lower())
        out += [struct.pack('<H', len(name)), name.encode(), struct.pack('<I', len(data)), data]
    return b''.join(out)


def unpack(blob):
    if blob[:4] != b'MCP1':
        raise ValueError('not a payload')
    count = struct.unpack_from('<I', blob, 4)[0]
    pos, files = 8, []
    for _ in range(count):
        n = struct.unpack_from('<H', blob, pos)[0]
        name = blob[pos + 2:pos + 2 + n].decode()
        size = struct.unpack_from('<I', blob, pos + 2 + n)[0]
        start = pos + 6 + n
        files.append((name, blob[start:start + size]))
        pos = start + size
    if pos != len(blob):
        raise ValueError('trailing bytes in payload')
    return files


def resources(exe_bytes):
    """{id: bytes} of the RCDATA (type 10) resources with numeric ids in a PE file."""
    b = exe_bytes
    pe = struct.unpack_from('<I', b, 0x3c)[0]
    nsec = struct.unpack_from('<H', b, pe + 6)[0]
    opt = struct.unpack_from('<H', b, pe + 20)[0]
    rsrc_rva = struct.unpack_from('<I', b, pe + 24 + 96 + 2 * 8)[0]
    if not rsrc_rva:
        return {}
    secs = []
    for i in range(nsec):
        s = pe + 24 + opt + i * 40
        vsize, va, rawsize, raw = struct.unpack_from('<IIII', b, s + 8)
        secs.append((va, max(vsize, rawsize), raw))

    def off(rva):
        for va, size, raw in secs:
            if va <= rva < va + size:
                return raw + rva - va
        raise ValueError('rva outside sections')
    base = off(rsrc_rva)

    def entries(o):
        named, ids = struct.unpack_from('<HH', b, o + 12)
        return [struct.unpack_from('<II', b, o + 16 + 8 * i) for i in range(named + ids)]
    found = {}
    for tid, tsub in entries(base):
        if tid != 10 or not tsub & 0x80000000:
            continue
        for rid, rsub in entries(base + (tsub & 0x7fffffff)):
            if not rsub & 0x80000000:
                continue
            for _lang, leaf in entries(base + (rsub & 0x7fffffff)):
                rva, size = struct.unpack_from('<II', b, base + leaf)
                found[rid] = b[off(rva):off(rva) + size]
    return found


def read_exe(path):
    """(version, sha256 hex from the exe, blob) of a Setup exe, or None when it carries no payload."""
    res = resources(Path(path).read_bytes())
    if RES_PAYLOAD not in res:
        return None
    return res[RES_VERSION].split(b'\0')[0].decode(), res[RES_SHA256].decode(), res[RES_PAYLOAD]


def main(argv):
    if len(argv) >= 5 and argv[1] == 'pack':
        outdir, version = Path(argv[2]), argv[3]
        files = []
        for spec in argv[4:]:
            name, _, path = spec.partition('=')
            files.append((name, Path(path).read_bytes()))
        blob = pack(files)
        outdir.mkdir(parents=True, exist_ok=True)
        (outdir / 'payload.bin').write_bytes(blob)
        (outdir / 'payload.sha256').write_text(hashlib.sha256(blob).hexdigest())
        (outdir / 'version.txt').write_text(version)
        dist = Path(__file__).resolve().parents[1] / 'dist'
        rc = ['1 24 "%s"' % (dist / 'installer.manifest').as_posix(), '1 ICON "%s"' % (dist / 'installer.ico').as_posix()]
        for rid, f in ((RES_PAYLOAD, 'payload.bin'), (RES_VERSION, 'version.txt'), (RES_SHA256, 'payload.sha256')):
            rc.append('%d RCDATA "%s"' % (rid, (outdir / f).resolve().as_posix()))
        (outdir / 'setup.rc').write_text('\n'.join(rc) + '\n')
        return 0
    if len(argv) == 3 and argv[1] == 'list':
        got = read_exe(argv[2])
        if not got:
            print('no payload')
            return 1
        for name, data in unpack(got[2]):
            print('%8d  %s' % (len(data), name))
        return 0
    if len(argv) == 4 and argv[1] == 'extract':
        got = read_exe(argv[2])
        os.mkdir(argv[3])
        for name, data in unpack(got[2]):
            target = Path(argv[3]) / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
        return 0
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv))
