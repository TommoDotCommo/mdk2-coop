#!/usr/bin/env python3
"""Update a disposable, leased game and verify its launcher after a real restart.

The caller supplies start/stop commands which register and stop only its own game.
Requires a stopped copy configured with [test] background=1 and launcher=1.
Only the local fake release server is contacted. No public release is downloaded.
--rollback: after the restart on the new version, roll back from the About page, restart, and check the old version runs.
--unchanged: the game folder's files (names and hashes) end as they began, nothing the updater keeps (backups, state, the
renamed DLL, stage files) is ever in the game folder, and its backups are in <user data>/mdk2-coop/update. This copy's own
data (coop/ beside coop.lua, as a test copy's coop.ini asks, and its [test] save_dir) is left out of the comparison.
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
    parser.add_argument('--rollback', action='store_true')
    parser.add_argument('--unchanged', action='store_true')
    args = parser.parse_args()
    config = configparser.ConfigParser(interpolation=None)
    ini = args.game_dir / 'coop/coop.ini'
    config.read(ini)
    assert config.getint('test', 'background') == 1
    assert config.getint('test', 'launcher') == 1
    assert (args.game_dir / 'mdk2hd.exe').is_file()
    live = Launchers([args.game_dir] * 3, args.output)
    save_dir = config.get('test', 'save_dir', fallback='')
    data_base = (Path(save_dir) if save_dir[1:2] == ':' else args.game_dir / save_dir) if save_dir else None
    package = {'dinput8.dll', 'coop/coop.lua', 'dinput8.dll.coop-receipt.json', 'coop/coop.lua.coop-receipt.json'}
    updater_files = ('.previous', 'update-state.ini', '.update-old', '-stage')

    def snapshot():
        """{relative path: sha256} of the game folder without this test copy's own data."""
        out = {}
        for f in sorted(args.game_dir.rglob('*')):
            rel = f.relative_to(args.game_dir).as_posix()
            if not f.is_file() or (save_dir and rel.startswith(save_dir.replace('\\', '/').rstrip('/') + '/')):
                continue
            if rel.startswith('coop/') and rel not in package and '/' not in rel[5:]:
                continue        # the test copy's data folder (coop.ini [test]): logs, state dumps, its coop.ini
            out[rel] = hashlib.sha256(f.read_bytes()).hexdigest()
        return out

    def updater_leftovers():
        return [f.relative_to(args.game_dir).as_posix() for f in args.game_dir.rglob('*')
                if f.is_file() and any(t in f.name for t in updater_files)
                and not (save_dir and f.relative_to(args.game_dir).as_posix().startswith(save_dir.rstrip('/') + '/'))]
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
            before = snapshot()
            (args.output / 'game-folder-before.json').write_text(json.dumps(before, indent=2))
            before_receipts = {k: (args.game_dir / k).read_text() for k in before if k.endswith('.coop-receipt.json')}
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
            if args.unchanged:
                assert data_base, 'the copy needs [test] save_dir: the updater keeps its backups under it'
                kept = sorted(p.name for p in (data_base / 'mdk2-coop/update').iterdir())
                assert kept == ['coop.lua.previous', 'dinput8.dll.previous', 'update-state.ini'], 'update folder: %s' % kept
                left = [f for f in updater_leftovers() if not f.startswith('dinput8.dll.update-old')]
                assert not left, 'updater files in the game folder after the update: %s' % left
            subprocess.run(args.stop_command, check=True)
            started = False
            # Remove the old log, so boot detection cannot read the previous run.
            (args.game_dir / 'coop/bridge.log').unlink(missing_ok=True)
            start()
            live.expect(1, 'restarted real game uses new launcher version', timeout=15,
                        **{'launcher.version': args.new_version})
            live.command(1, 'page 3')
            live.shot(1, 'updated_about_after_restart')
            if args.unchanged:
                assert not updater_leftovers(), 'updater files in the game folder after the restart: %s' % updater_leftovers()
            if args.rollback:
                live.command(1, 'update rollback')
                live.expect(1, 'real launcher rolls back to the verified backups', timeout=30,
                            **{'launcher.update_state': 6, 'launcher.update_err': 0})
                subprocess.run(args.stop_command, check=True)
                started = False
                (args.game_dir / 'coop/bridge.log').unlink(missing_ok=True)
                start()
                live.expect(1, 'restarted real game runs the rolled-back version', timeout=15,
                            **{'launcher.version': args.old_version})
                subprocess.run(args.stop_command, check=True)
                started = False
                time.sleep(2)
            if args.unchanged:
                after = snapshot()
                (args.output / 'game-folder-after.json').write_text(json.dumps(after, indent=2))
                changed = sorted(k for k in set(before) | set(after) if before.get(k) != after.get(k))
                if args.rollback:
                    receipts = [k for k in changed if k.endswith('.coop-receipt.json')
                                and json.loads((args.game_dir / k).read_text()) == json.loads(before_receipts[k])]
                    changed = [k for k in changed if k not in receipts]
                    assert not changed, 'game folder differs after update + rollback: %s' % changed
                else:
                    assert set(changed) <= package, 'the update changed game-folder files it does not ship: %s' % changed
            assert '/latest' in requests and '/asset.zip' in requests and '/asset.sha256' in requests
            (args.output / 'requests.json').write_text(json.dumps(requests, indent=2))
            print('PASS: real game DLL update, restart and About version display'
                  + (', rollback and restart' if args.rollback else '')
                  + (', game folder unchanged (updater files only in the user data folder)' if args.unchanged else ''), flush=True)
        finally:
            if started:
                subprocess.run(args.stop_command, check=True)
            server.shutdown()
            worker.join()


if __name__ == '__main__':
    main()
