#!/usr/bin/env python3
"""Test stand-in for a code signer: gives a PE file a certificate table (what pe_signed() looks for), in place. Not a signature."""
import struct
import sys
from pathlib import Path

path = Path(sys.argv[1])
data = bytearray(path.read_bytes())
pe = struct.unpack_from('<I', data, 0x3c)[0]
magic = struct.unpack_from('<H', data, pe + 24)[0]
directory = pe + 24 + (96 if magic == 0x10b else 112) + 4 * 8
while len(data) % 8:
    data.append(0)
cert = struct.pack('<IHH', 16, 0x200, 2) + b'FAKE-CERT-FOR-TESTS'
cert += b'\0' * (-len(cert) % 8)
struct.pack_into('<II', data, directory, len(data), len(cert))
data += cert
path.write_bytes(data)
