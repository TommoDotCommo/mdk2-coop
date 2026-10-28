#!/usr/bin/env python3
"""Windows WinINet/update adapter against a local server; no game is launched.
Pass a disposable NTFS --test-root and two locally built release packages.
"""
import argparse
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import shutil
import subprocess
import threading
import uuid
import zipfile

ROOT = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--test-root', type=Path, required=True)
p.add_argument('--old-package', type=Path, required=True)
p.add_argument('--new-package', type=Path, required=True)
p.add_argument('--try-real-api', action='store_true')
a = p.parse_args()
a.test_root = a.test_root / ('run-' + uuid.uuid4().hex)
a.test_root.mkdir(parents=True, exist_ok=True)
exe = a.test_root / 'launcher_update_win_test.exe'
# An explicit non-elevating manifest prevents Windows installer-detection
# heuristics for a fixture whose filename contains 'update'.
resource = a.test_root/'manifest.rc'
resource.write_text('1 24 "' + str(ROOT/'bridge/tests/as_invoker.manifest') + '"\n')
resource_obj = a.test_root/'manifest.o'
subprocess.run(['i686-w64-mingw32-windres', str(resource), '-o', str(resource_obj)], check=True)
subprocess.run(['i686-w64-mingw32-gcc', '-O2', '-Wall', '-Wextra', '-Wno-unused-function',
                '-I', str(ROOT/'bridge'), str(ROOT/'bridge/tests/launcher_update_win_test.c'),
                str(resource_obj), '-o', str(exe), '-lwininet'], check=True)
archive = a.new_package.read_bytes()
name = a.new_package.name
sidecar = f'{hashlib.sha256(archive).hexdigest()}  {name}\n'.encode()
profile = a.test_root/'profile'
profile.mkdir(exist_ok=True)
winpath = lambda path: subprocess.check_output(['wslpath', '-w', str(path.resolve())], text=True).strip()
env = dict(os.environ)
env['LOCALAPPDATA'] = winpath(profile)
env['TEMP'] = env['TMP'] = winpath(profile)
env['WSLENV'] = ':'.join(filter(None, [env.get('WSLENV', ''), 'LOCALAPPDATA/w:TEMP/w:TMP/w']))
mode = ''
class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_): pass
    def do_GET(self):
        if self.path.startswith('/latest') or self.path.startswith('/releases'):
            release_name = a.old_package.name if mode == 'current' else name
            assets = [{'name': release_name, 'size': len(archive), 'browser_download_url': base+'/asset.zip'}]
            if mode != 'missing-sidecar':
                assets.append({'name': release_name+'.sha256', 'size': len(sidecar), 'browser_download_url': base+'/asset.sha256'})
            body = json.dumps([{'tag_name': 'v0.3.2-test' if mode == 'current' else 'v0.4.0-test',
                                'draft': False, 'prerelease': False,
                                'body': 'Local verification release', 'assets': assets}]).encode()
        elif self.path.endswith('.sha256'):
            body = b'0'*64 + sidecar[64:] if mode == 'checksum' else sidecar
        else:
            body = archive[:len(archive)//2] if mode == 'truncated' else archive
        self.send_response(200)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)
with ThreadingHTTPServer(('127.0.0.1', 0), Handler) as server:
    base = f'http://127.0.0.1:{server.server_port}'
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    try:
        scenarios = [('current',1), ('missing-sidecar',9), ('checksum',12), ('truncated',11), ('rollback-on-failure',15), ('success',0)]
        if a.try_real_api: scenarios.insert(0, ('real-api',0))
        for mode, expected in scenarios:
            game = a.test_root/mode
            game.mkdir(exist_ok=True)
            with zipfile.ZipFile(a.old_package) as z:
                for entry in z.infolist():
                    rel = Path(*Path(entry.filename).parts[1:])
                    if str(rel) in ('dinput8.dll','coop/coop.lua'):
                        target = game/rel
                        target.parent.mkdir(parents=True, exist_ok=True)
                        target.write_bytes(z.read(entry))
            shutil.copyfile(exe, game/exe.name)
            # Launch via Windows so NTFS fixtures use the same filesystem/process
            # path as the game, independent of WSL executable interop quirks.
            quote = lambda value: "'" + str(value).replace("'", "''") + "'"
            stdout, stderr = game/'stdout.txt', game/'stderr.txt'
            args = f'"{winpath(game)}" "{mode if mode == "real-api" else base+"/releases?per_page=10"}" {expected}'
            command = (f"$ErrorActionPreference='Stop'; $env:LOCALAPPDATA={quote(winpath(profile))}; $env:TEMP=$env:TMP={quote(winpath(profile))}; "
                       f"$p=Start-Process -FilePath {quote(winpath(game/exe.name))} "
                       f"-ArgumentList {quote(args)} -WindowStyle Hidden -Wait -PassThru "
                       f"-RedirectStandardOutput {quote(winpath(stdout))} -RedirectStandardError {quote(winpath(stderr))}; "
                       "exit $p.ExitCode")
            result = subprocess.run(['powershell.exe', '-NoProfile', '-Command', command],
                                    env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
            output = stdout.read_text(errors='replace') + stderr.read_text(errors='replace') if stdout.exists() else result.stdout
            print(mode + ': ' + output.strip(), flush=True)
            if result.returncode or 'PASS:' not in output and mode != 'real-api':
                raise RuntimeError(f'{mode}: fixture exit {result.returncode}: {result.stdout}')
    finally:
        server.shutdown()
        worker.join()
print('PASS: native Windows updater and mapped launcher DLL scenarios')
