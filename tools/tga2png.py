#!/usr/bin/env python3
"""tga2png.py <in.tga> <out.png>: the game's screenshots (uncompressed or RLE, 24/32-bit) as PNG."""
import struct, sys, zlib

def read_tga(path):
    data = open(path, "rb").read()
    idlen, cmtype, itype = data[0], data[1], data[2]
    w, h, bpp, desc = struct.unpack_from("<HHBB", data, 12)
    px = bpp // 8
    pos = 18 + idlen + (struct.unpack_from("<H", data, 5)[0] * (data[7] // 8) if cmtype else 0)
    if itype == 2:
        raw = data[pos:pos + w * h * px]
    elif itype == 10:
        out = bytearray()
        while len(out) < w * h * px:
            c = data[pos]; pos += 1
            n = (c & 0x7f) + 1
            if c & 0x80:
                out += data[pos:pos + px] * n; pos += px
            else:
                out += data[pos:pos + n * px]; pos += n * px
        raw = bytes(out)
    else:
        sys.exit(f"unsupported TGA type {itype}")
    rows = [raw[y * w * px:(y + 1) * w * px] for y in range(h)]
    if not desc & 0x20:             # origin bottom left
        rows.reverse()
    rgb = []
    for r in rows:
        line = bytearray(w * 3)
        line[0::3], line[1::3], line[2::3] = r[2::px], r[1::px], r[0::px]
        rgb.append(bytes(line))
    return w, h, rgb

def write_png(path, w, h, rows):
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    body = zlib.compress(b"".join(b"\0" + r for r in rows), 6)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", body) + chunk(b"IEND", b""))

w, h, rows = read_tga(sys.argv[1])
write_png(sys.argv[2], w, h, rows)
