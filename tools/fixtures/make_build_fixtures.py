#!/usr/bin/env python3
"""Synthetic stand-ins for mdk2hd.exe: tiny 32-bit PE files that carry (or lack) exactly the two facts the mod uses to
recognise a game build (bridge/build_sigs.h): the "copytagmethods" string and a table entry that points at it. No game code.

  make_build_fixtures.py <folder>      writes steam.exe, overhaul.exe and the unknown variants
"""
import struct
import sys
from pathlib import Path

BASE = 0x400000
SECTION_VA = 0xD0000            # RVA; covers both builds' table entries and strings
SECTION_SIZE = 0x40000
# (table entry VA, string VA) as in bridge/build_sigs.h
STEAM = (0x506520, 0x4d7d34)
OVERHAUL = (0x505510, 0x4d769c)


def make_exe(entry_va, name_va, text=b'copytagmethods\0', pointer=None, magic=0x10b, truncate=None, declared=SECTION_SIZE):
    """pointer: what the table entry holds (default: the string's address, which is the real thing)."""
    pe = 0x80
    opt_size = 0xE0
    headers = bytearray(0x200)
    headers[0:2] = b'MZ'
    struct.pack_into('<I', headers, 0x3c, pe)
    headers[pe:pe + 4] = b'PE\0\0'
    struct.pack_into('<HHIIIHH', headers, pe + 4, 0x14c, 1, 0, 0, 0, opt_size, 0x102)
    o = pe + 24
    struct.pack_into('<H', headers, o, magic)
    struct.pack_into('<I', headers, o + 28, BASE)
    struct.pack_into('<II', headers, o + 32, 0x1000, 0x200)
    struct.pack_into('<II', headers, o + 56, SECTION_VA + SECTION_SIZE, 0x200)
    struct.pack_into('<I', headers, o + 92, 16)
    s = o + opt_size
    headers[s:s + 8] = b'.data\0\0\0'
    struct.pack_into('<IIII', headers, s + 8, declared, SECTION_VA, declared, 0x200)
    body = bytearray(SECTION_SIZE)
    struct.pack_into('<I', body, entry_va - BASE - SECTION_VA, name_va if pointer is None else pointer)
    body[name_va - BASE - SECTION_VA:name_va - BASE - SECTION_VA + len(text)] = text
    data = bytes(headers) + bytes(body)
    return data[:truncate] if truncate is not None else data


def variants():
    return {
        'steam.exe': make_exe(*STEAM),
        'overhaul.exe': make_exe(*OVERHAUL),
        # the string is there but the table entry does not point at it: an unknown build
        'unknown-pointer.exe': make_exe(*STEAM, pointer=0x1234),
        # the pointer is right but the string is something else
        'unknown-string.exe': make_exe(*OVERHAUL, text=b'copytagmethodz\0'),
        # a different layout altogether (both facts sit at addresses the mod does not know)
        'unknown-layout.exe': make_exe(0x507000, 0x4d8000),
        # the right bytes sit in the file but outside what the section header declares (not part of the image)
        'unknown-outside-section.exe': make_exe(*STEAM, declared=0x8000),
        'unknown-64bit.exe': make_exe(*STEAM, magic=0x20b),
        'unknown-truncated.exe': make_exe(*STEAM, truncate=0x300),
        'unknown-text.exe': b'fixture, never executed',
    }


if __name__ == '__main__':
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    for name, data in variants().items():
        (out / name).write_bytes(data)
