#!/usr/bin/env python3
"""Game-free auto-update unit tests, source mutants and local fake release server tests."""
import hashlib
import io
import json
from pathlib import Path
import socket
import subprocess
import tempfile
import threading
import zipfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / 'bridge/launcher_update.h').read_text()

MUTANTS = {
    'ver-major': ('if (cand.major != cur.major) return cand.major > cur.major ? 1 : 0;', ''),
    'ver-minor': ('if (cand.minor != cur.minor) return cand.minor > cur.minor ? 1 : 0;', ''),
    'ver-patch': ('if (cand.patch != cur.patch) return cand.patch > cur.patch ? 1 : 0;', ''),
    'ver-prerelease': ('if (!cur.pre[0] && cand.pre[0]) return 0;', 'if (!cur.pre[0] && cand.pre[0]) return 1;'),
    'ver-dev': ('if (!current || !strcmp(current, "dev")) return 1;', 'if (!current || !strcmp(current, "dev")) return 0;'),
    'throttle-interval': ('if (s->last_check && now < s->last_check + LU_INTERVAL_SEC) return 0;', ''),
    'throttle-ratelimit': ('if (now < s->retry_after) return 0;', ''),
    'throttle-auto-flag': ('if (!s->auto_enabled) return 0;', ''),
    'http-ratelimit-429': ('status_code == 429 || ', ''),
    'http-ratelimit-403': ('(status_code == 403 && ratelimit_remaining == 0)', '0'),
    'json-nul-check': ('|| memchr(json, 0, len)', ''),
    'url-scheme-check': ('if (!lu_url_valid(url))', 'if (0)'),
    'asset-both-required': ('if (!out->zip_url[0] || !out->zip_size || !out->sha_url[0] || !out->sha_size)', 'if (!out->zip_url[0])'),
    'sidecar-filename-match': ('|| strncmp(text + name_start, expected_zip_name, name_len)', ''),
    'download-size-check': ('actual_size != declared_size || ', ''),
    'download-sha256-check': ('if (strcmp(actual_hash, expected_sha256) != 0)', 'if (0)'),
    'zip-crc32-check': ('if (lu_crc32(buf, uncomp_sz) != crc)', 'if (0)'),
    'pe-magic-check': ("bytes[0] != 'M' || bytes[1] != 'Z'", '0'),
    'pe-arch-check': ('if (lu_u16(bytes + pe_off + 4) != 0x014cu) return 0;', ''),
    'rollback-backup-verify': ('if (!lu_file_sha256(dll_prev, check_dll) || strcmp(check_dll, prev_dll_hash) != 0) return LU_ERR_ROLLBACK;', ''),
    'apply-rollback-on-fail': ('if (fail_after_dll || !lu_rename_replace(lua_stage, lua_path)', 'if (0'),
}

# Adjust url-scheme-check mutant to match exact substring in launcher_update.h
assert '&& lu_url_valid(url))' in SOURCE
MUTANTS['url-scheme-check'] = ('&& lu_url_valid(url))', '&& 1)')

with tempfile.TemporaryDirectory() as temp:
    directory = Path(temp)

    def run_unit(src_text):
        (directory / 'launcher_update.h').write_text(src_text)
        binary = directory / 'unit_test'
        subprocess.run(['cc', '-std=c99', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-parameter', '-Wno-unused-but-set-variable', '-Wno-unused-function',
                        '-I', str(directory), str(ROOT / 'bridge/tests/launcher_update_test.c'),
                        '-o', str(binary)], check=True, capture_output=True)
        game_dir = directory / 'game'
        (game_dir / 'coop').mkdir(parents=True, exist_ok=True)
        return subprocess.run([str(binary), str(game_dir)], capture_output=True)

    res = run_unit(SOURCE)
    assert res.returncode == 0, res.stderr.decode()
    print(res.stdout.decode().strip())

    for name, (before, after) in MUTANTS.items():
        assert SOURCE.count(before) == 1, (name, SOURCE.count(before))
        res = run_unit(SOURCE.replace(before, after))
        assert res.returncode != 0, 'SURVIVED: ' + name
        print('KILLED:', name)

# Local fake release server tests (ZIP_DEFLATED archive, sidecar, truncation, checksum, rate-limit, rollback)
def make_pe_bytes(fill_byte):
    b = bytearray([fill_byte] * 512)
    b[0:2] = b'MZ'
    b[60:64] = (64).to_bytes(4, 'little')
    b[64:68] = b'PE\x00\x00'
    b[68:70] = (0x014c).to_bytes(2, 'little')
    return bytes(b)

dll_v1 = make_pe_bytes(0x11)
lua_v1 = b'-- coop.lua v0.3.1\n'
dll_v2 = make_pe_bytes(0x22)
lua_v2 = b'-- coop.lua v0.4.0\n'

zip_buffer = io.BytesIO()
with zipfile.ZipFile(zip_buffer, 'w', zipfile.ZIP_DEFLATED) as zf:
    zf.writestr('mdk2-coop-v0.4.0/dinput8.dll', dll_v2)
    zf.writestr('mdk2-coop-v0.4.0/coop/coop.lua', lua_v2)
zip_bytes = zip_buffer.getvalue()
zip_sha = hashlib.sha256(zip_bytes).hexdigest()
sidecar_bytes = f'{zip_sha}  mdk2-coop-v0.4.0.zip\n'.encode()

with tempfile.TemporaryDirectory() as temp:
    directory = Path(temp)
    client_bin = directory / 'client'
    subprocess.run(['cc', '-std=c99', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-I', str(ROOT / 'bridge'), str(ROOT / 'bridge/tests/launcher_update_client.c'),
                    '-o', str(client_bin)], check=True)

    scenarios = [
        ('up-to-date', 'v0.4.0', 1, 0),
        ('update-success', 'v0.3.1', 0, 0),
        ('checksum-mismatch', 'v0.3.1', 12, 0),
        ('interrupted-download', 'v0.3.1', 11, 0),
        ('missing-sidecar', 'v0.3.1', 9, 0),
        ('rate-limited', 'v0.3.1', 5, 0),
        ('rollback-on-failure', 'v0.3.1', 15, 1),
    ]

    for mode, cur_ver, expected_code, fail_step in scenarios:
        game_dir = directory / f'game-{mode}'
        (game_dir / 'coop').mkdir(parents=True, exist_ok=True)
        (game_dir / 'dinput8.dll').write_bytes(dll_v1)
        (game_dir / 'coop/coop.lua').write_bytes(lua_v1)
        receipt = {'schema': 1, 'original': 'a' * 64, 'installed': hashlib.sha256(dll_v1).hexdigest(), 'pending': None}
        (game_dir / 'dinput8.dll.coop-receipt.json').write_text(json.dumps(receipt))

        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as srv:
            srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            srv.bind(('127.0.0.1', 0))
            srv.listen(4)
            srv.settimeout(3)
            port = srv.getsockname()[1]
            errors = []

            def serve():
                try:
                    reqs = 1 if mode in ('up-to-date', 'missing-sidecar', 'rate-limited') else 3
                    for _ in range(reqs):
                        conn, _ = srv.accept()
                        with conn:
                            req = conn.recv(4096).decode(errors='replace')
                            path = req.split(' ')[1]
                            if path == '/releases/latest':
                                if mode == 'rate-limited':
                                    resp = (b'HTTP/1.0 403 Forbidden\r\n'
                                            b'X-RateLimit-Remaining: 0\r\n'
                                            b'Retry-After: 900\r\n\r\n{}')
                                    conn.sendall(resp)
                                    continue
                                assets = [{
                                    'name': 'mdk2-coop-v0.4.0.zip',
                                    'browser_download_url': f'http://127.0.0.1:{port}/download/mdk2-coop-v0.4.0.zip',
                                    'size': len(zip_bytes),
                                }]
                                if mode != 'missing-sidecar':
                                    assets.append({
                                        'name': 'mdk2-coop-v0.4.0.zip.sha256',
                                        'browser_download_url': f'http://127.0.0.1:{port}/download/mdk2-coop-v0.4.0.zip.sha256',
                                        'size': len(sidecar_bytes),
                                    })
                                payload = json.dumps({
                                    'tag_name': 'v0.4.0',
                                    'body': 'Release notes for v0.4.0',
                                    'assets': assets,
                                }).encode()
                                conn.sendall(b'HTTP/1.0 200 OK\r\nContent-Type: application/json\r\n\r\n' + payload)
                            elif path.endswith('.sha256'):
                                body = sidecar_bytes if mode != 'checksum-mismatch' else (b'0' * 64 + b'  mdk2-coop-v0.4.0.zip\n')
                                conn.sendall(b'HTTP/1.0 200 OK\r\n\r\n' + body)
                            elif path.endswith('.zip'):
                                body = zip_bytes[:len(zip_bytes) // 2] if mode == 'interrupted-download' else zip_bytes
                                conn.sendall(b'HTTP/1.0 200 OK\r\n\r\n' + body)
                except Exception as exc:
                    errors.append(exc)

            t = threading.Thread(target=serve)
            t.start()
            res = subprocess.run([str(client_bin), f'http://127.0.0.1:{port}/releases/latest',
                                  str(game_dir), cur_ver, str(expected_code), str(fail_step)],
                                 capture_output=True, timeout=5)
            t.join(3)
            assert not errors, (mode, errors)
            assert res.returncode == 0, (mode, res.stderr.decode())
            if mode == 'update-success':
                assert (game_dir / 'dinput8.dll').read_bytes() == dll_v2
                assert (game_dir / 'coop/coop.lua').read_bytes() == lua_v2
                updated_receipt = json.loads((game_dir / 'dinput8.dll.coop-receipt.json').read_text())
                assert updated_receipt['original'] == 'a' * 64
                assert updated_receipt['installed'] == hashlib.sha256(dll_v2).hexdigest()
            else:
                assert (game_dir / 'dinput8.dll').read_bytes() == dll_v1
                assert (game_dir / 'coop/coop.lua').read_bytes() == lua_v1
            print('PASS: local fake release server', mode)

    # Also test offline (closed port)
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(('127.0.0.1', 0))
        closed_port = s.getsockname()[1]
    res = subprocess.run([str(client_bin), f'http://127.0.0.1:{closed_port}/releases/latest',
                          str(directory), 'v0.3.1', '4', '0'], capture_output=True, timeout=5)
    assert res.returncode == 0, res.stderr.decode()
    print('PASS: local fake release server offline')
