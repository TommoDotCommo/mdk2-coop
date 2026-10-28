#!/usr/bin/env python3
"""Update a disposable, leased game and verify its launcher after a real restart.

The caller supplies start/stop commands which register and stop only its own game.
Requires a stopped copy configured with [test] background=1 and launcher=1.
Only the local fake release server is contacted. No public release is downloaded.
"""
import argparse
import configparser
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import io
import json
from pathlib import Path
import subprocess
import threading
import time
import zipfile

from check_lobby_live import Launchers


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game-dir', type=Path, required=True)
    parser.add_argument('--old-package', type=Path, required=True)
    parser.add_argument('--new-package', type=Path, required=True)
    parser.add_argument('--old-version', required=True)
    parser.add_argument('--new-version', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--start-command', nargs='+', required=True)
    parser.add_argument('--stop-command', nargs='+', required=True)
    args = parser.parse_args()
    config = configparser.ConfigParser(interpolation=None)
    ini = args.game_dir / 'coop/coop.ini'
    config.read(ini)
    assert config.getint('test', 'background') == 1
    assert config.getint('test', 'launcher') == 1
    assert (args.game_dir / 'mdk2hd.exe').is_file()
    live = Launchers([args.game_dir] * 3, args.output)
    archive = args.new_package.read_bytes()
    name = args.new_package.name
    sidecar = f'{hashlib.sha256(archive).hexdigest()}  {name}\n'.encode()
    requests = []

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_GET(self):
            requests.append(self.path)
            if self.path == '/latest':
                body = json.dumps({'tag_name': args.new_version, 'body': 'Local live verification',
                                   'assets': [{'name': name, 'size': len(archive), 'browser_download_url': base + '/asset.zip'},
                                              {'name': name + '.sha256', 'size': len(sidecar), 'browser_download_url': base + '/asset.sha256'}]}).encode()
            elif self.path == '/asset.sha256':
                body = sidecar
            elif self.path == '/asset.zip':
                body = archive
            else:
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    with ThreadingHTTPServer(('127.0.0.1', 0), Handler) as server:
        base = f'http://127.0.0.1:{server.server_port}'
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        started = False

        def start():
            nonlocal started
            subprocess.run(args.start_command, check=True)
            started = True
            # The caller returns a registered PID; allow the real launcher to boot.
            log = args.game_dir / 'coop/bridge.log'
            deadline = time.monotonic() + 60
            while not log.exists() or 'launcher: shown' not in log.read_text(errors='replace'):
                assert time.monotonic() < deadline, 'launcher did not start'
                time.sleep(.1)

        try:
            for target, entry in [('dinput8.dll', 'dinput8.dll'), ('coop/coop.lua', 'coop/coop.lua')]:
                with zipfile.ZipFile(args.old_package) as z:
                    payload = next(n for n in z.namelist() if n.endswith('/' + entry))
                    path = args.game_dir / target
                    path.unlink(missing_ok=True)  # never write through a game-copy hard link
                    path.write_bytes(z.read(payload))
            config['update'] = {'auto_check': '0', 'api_url': base + '/latest', 'last_check': '0', 'retry_after': '0'}
            stream = io.StringIO()
            config.write(stream, space_around_delimiters=False)
            ini.write_text(stream.getvalue())
            (args.game_dir / 'coop/bridge.log').unlink(missing_ok=True)
            start()
            live.expect(1, 'old real game launcher version', **{'launcher.version': args.old_version})
            live.command(1, 'page 3', 'update check')
            live.expect(1, 'real launcher discovers local release', timeout=30,
                        **{'launcher.update_state': 3, 'launcher.update_tag': args.new_version})
            live.command(1, 'update apply')
            live.expect(1, 'real launcher applies verified update', timeout=30,
                        **{'launcher.update_state': 5, 'launcher.update_err': 0, 'launcher.version': args.old_version})
            live.shot(1, 'update_applied_before_restart')
            with zipfile.ZipFile(args.new_package) as z:
                dll = next(n for n in z.namelist() if n.endswith('/dinput8.dll'))
                assert (args.game_dir / 'dinput8.dll').read_bytes() == z.read(dll)
            subprocess.run(args.stop_command, check=True)
            started = False
            # Remove the old log, so boot detection cannot read the previous run.
            (args.game_dir / 'coop/bridge.log').unlink(missing_ok=True)
            start()
            live.expect(1, 'restarted real game uses new launcher version', timeout=15,
                        **{'launcher.version': args.new_version})
            live.command(1, 'page 3')
            live.shot(1, 'updated_about_after_restart')
            assert '/latest' in requests and '/asset.zip' in requests and '/asset.sha256' in requests
            (args.output / 'requests.json').write_text(json.dumps(requests, indent=2))
            print('PASS: real game DLL update, restart and About version display', flush=True)
        finally:
            if started:
                subprocess.run(args.stop_command, check=True)
            server.shutdown()
            worker.join()


if __name__ == '__main__':
    main()
