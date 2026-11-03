#!/usr/bin/env python3
"""VERSIONINFO resources for the files the release ships (dinput8.dll, install-policy.exe, MDK2-Coop-Setup.exe), so Windows'
file properties, mod managers and diagnostics see the mod version.

  versioninfo.py <version> <file name> <description> <out.rc>   writes the resource script (only when it changed)
  versioninfo.py --read <pe file>                               prints the ProductVersion string of a built file
"""
import re
import sys
from pathlib import Path

PRODUCT = 'MDK2 Co-op'


def numbers(version):
    """v1.2.3[-x] -> (1, 2, 3, 0); anything else (a git hash) -> zeros. The string keeps the exact version."""
    m = re.match(r'^v?(\d+)\.(\d+)\.(\d+)', version)
    return tuple(min(int(x), 65535) for x in m.groups()) + (0,) if m else (0, 0, 0, 0)


def script(version, name, description):
    def q(s):
        return s.replace('"', '""')
    n = ','.join(str(x) for x in numbers(version))
    rows = [('CompanyName', 'TommoDotCommo'), ('FileDescription', description), ('FileVersion', version),
            ('InternalName', name), ('LegalCopyright', 'MIT licence'), ('OriginalFilename', name),
            ('ProductName', PRODUCT), ('ProductVersion', version)]
    values = '\n'.join('      VALUE "%s", "%s\\0"' % (k, q(v)) for k, v in rows)
    return ('1 VERSIONINFO\nFILEVERSION %s\nPRODUCTVERSION %s\nFILEOS 0x40004\nFILETYPE %s\nBEGIN\n'
            '  BLOCK "StringFileInfo"\n  BEGIN\n    BLOCK "040904B0"\n    BEGIN\n%s\n    END\n  END\n'
            '  BLOCK "VarFileInfo"\n  BEGIN\n    VALUE "Translation", 0x409, 1200\n  END\nEND\n'
            % (n, n, '2' if name.lower().endswith('.dll') else '1', values))


def read(data, field="ProductVersion"):
    """A string field of a PE file's VERSIONINFO, or None."""
    key = (field + '\0').encode('utf-16-le')
    i = data.find(key)
    if i < 0:
        return None
    i += len(key)
    while data[i:i + 2] == b'\0\0':     # padding to the 32-bit boundary where the value starts
        i += 2
    end = i
    while end + 1 < len(data) and data[end:end + 2] != b'\0\0':
        end += 2
    return data[i:end].decode('utf-16-le', 'replace')


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == '--read':
        v = read(Path(sys.argv[2]).read_bytes())
        print(v if v is not None else '')
        sys.exit(0 if v else 1)
    if len(sys.argv) != 5:
        print(__doc__, file=sys.stderr)
        sys.exit(2)
    out = Path(sys.argv[4])
    text = script(sys.argv[1], sys.argv[2], sys.argv[3])
    if not out.exists() or out.read_text() != text:
        out.write_text(text)
