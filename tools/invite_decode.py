#!/usr/bin/env python3
"""An independent decoder for invite code v1 (docs/DISCOVERY.md), written from the format, not from bridge/coop_invite.h.
  tools/invite_decode.py <text|->      the clipboard text (the whole message or just the code); prints JSON, exit 1 when invalid
  tools/invite_decode.py --selftest    a known code decodes, a damaged one is refused"""
import json
import re
import sys

ALPHABET = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"


def crc8(data):
    crc = 0
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def decode(text, game="MDK2"):
    m = re.search(r"\b(%s(?:-[0-9A-Za-z]+)+)" % game, text, re.I)
    if not m:
        raise ValueError("no %s code in the text" % game)
    body = m.group(1)[len(game) + 1:].replace("-", "").upper().replace("I", "1").replace("L", "1").replace("O", "0")
    bits = 0
    acc = 0
    data = bytearray()
    for ch in body:
        if ch not in ALPHABET:
            raise ValueError("bad character %r" % ch)
        acc = (acc << 5) | ALPHABET.index(ch)
        bits += 5
        if bits >= 8:
            data.append((acc >> (bits - 8)) & 0xFF)
            bits -= 8
            acc &= (1 << bits) - 1
    if bits >= 5 or acc:
        raise ValueError("padding is not canonical")
    if len(data) < 9 or crc8(data[:-1]) != data[-1]:
        raise ValueError("incomplete: length or CRC-8 wrong")
    version, flags = data[0] >> 4, data[0] & 15
    if version != 1:
        raise ValueError("version %d" % version)
    secret = int.from_bytes(data[1:5], "little")
    port = int.from_bytes(data[5:7], "little")
    n = data[7]
    steam = bool(flags & 1)
    if n > 4 or not port or len(data) != 8 + 5 * n + (8 if steam else 0) + 1:
        raise ValueError("incomplete: route count or length wrong")
    routes = []
    for i in range(n):
        kind, ip = data[8 + 5 * i], data[9 + 5 * i:13 + 5 * i]
        if kind not in (1, 2, 3):
            raise ValueError("route kind %d" % kind)
        routes.append({"kind": {1: "lan", 2: "vpn", 3: "internet"}[kind], "ip": ".".join(str(b) for b in ip)})
    out = {"game": game, "invite_only": bool(flags & 2), "secret": secret, "port": port, "routes": routes, "steam": steam}
    link = re.search(r"https://\S+/#\S+", text)
    out["message"] = bool(re.search(r"\S+ is hosting \S+\. Join: https://", text))
    out["link"] = bool(link)
    return out


def selftest():
    # 2 routes (LAN 192.168.1.20, internet 8.8.4.4), port 27900, secret 0x12345678, invite only: built here, bytewise
    payload = bytearray([0x12]) + (0x12345678).to_bytes(4, "little") + (27900).to_bytes(2, "little") + bytes([2, 1, 192, 168, 1, 20, 3, 8, 8, 4, 4])
    payload.append(crc8(payload))
    bits, acc, chars = 0, 0, ""
    for b in payload:
        acc = (acc << 8) | b
        bits += 8
        while bits >= 5:
            chars += ALPHABET[(acc >> (bits - 5)) & 31]
            bits -= 5
            acc &= (1 << bits) - 1
    if bits:
        chars += ALPHABET[(acc << (5 - bits)) & 31]
    code = "MDK2-" + "-".join(chars[i:i + 5] for i in range(0, len(chars), 5))
    got = decode("Fox is hosting MDK2. Join: https://example.invalid/join#" + code)
    assert got["port"] == 27900 and got["secret"] == 0x12345678 and got["invite_only"] and len(got["routes"]) == 2, got
    bad = code[:-1] + ("0" if code[-1] != "0" else "1")
    try:
        decode(bad)
    except ValueError:
        return 0
    raise SystemExit("selftest: a damaged code was accepted")


if __name__ == "__main__":
    if sys.argv[1:] == ["--selftest"]:
        sys.exit(selftest())
    text = sys.stdin.read() if sys.argv[1:] == ["-"] else " ".join(sys.argv[1:])
    try:
        print(json.dumps(decode(text)))
    except ValueError as e:
        print("invalid invite: %s" % e, file=sys.stderr)
        sys.exit(1)
