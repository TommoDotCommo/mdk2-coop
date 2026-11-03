#!/usr/bin/env python3
"""Drive already leased, registered background launchers; assert process snapshots.

Used by qa_lobby.sh when MDK2_EXTENDED=1. No desktop input or process launching.
Each command is consumed by the production launcher's existing test interface.
"""
import argparse
import configparser
from pathlib import Path
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]


class Launchers:
    def __init__(self, directories, output):
        self.dirs = [Path(d) / 'coop' for d in directories]
        self.output = Path(output)
        self.output.mkdir(parents=True, exist_ok=True)
        self.serial = 0

    def command(self, player, *lines):
        target = self.dirs[player - 1] / 'launcher.txt'
        stage = target.with_suffix('.tmp')
        stage.write_text('\n'.join(lines) + '\n')
        stage.replace(target)
        deadline = time.monotonic() + 10
        while target.exists():
            if time.monotonic() > deadline:
                raise AssertionError(f'launcher {player} did not consume {lines!r}')
            time.sleep(.02)

    def state(self, player):
        self.command(player, 'dump')
        raw = (self.dirs[player - 1] / 'launcher-state.ini').read_text()
        self.serial += 1
        (self.output / f'{self.serial:04d}-player{player}.ini').write_text(raw)
        state = configparser.ConfigParser(interpolation=None)
        state.read_string(raw)
        return state

    def expect(self, player, label, timeout=5, **fields):
        deadline = time.monotonic() + timeout
        while True:
            state = self.state(player)
            actual = {key: state.get(*key.split('.', 1), fallback='<missing>') for key in fields}
            if all(actual[key] == str(value) for key, value in fields.items()):
                print(f'PASS: {label}: {actual}', flush=True)
                return state
            if time.monotonic() >= deadline:
                raise AssertionError(f'{label}: expected {fields}, got {actual}')
            time.sleep(.1)

    def shot(self, player, name):
        self.command(player, 'dump', f'shot {name}')
        (self.output / (name + '.ini')).write_bytes((self.dirs[player - 1] / 'launcher-state.ini').read_bytes())
        subprocess.run(['python3', str(ROOT / 'tools/tga2png.py'),
                        str(self.dirs[player - 1] / (name + '.tga')),
                        str(self.output / (name + '.png'))], check=True)

    def field(self, player, x, y, text):
        self.command(player, f'click {x} {y}', *(['key 8'] * 160), f'text {text}')

    def recents(self):
        # The saved-addresses store became Recent hosts (B4): the lobby's Save address pins the host.
        before = self.state(2).getint('recents', 'count')
        self.command(2, 'page 0', 'click 1180 510')
        state = self.expect(2, 'Save address keeps the lobby', **{'launcher.lobby': 1, 'launcher.connected': 1})
        count = state.getint('recents', 'count')
        assert count in (before, before + 1), (before, count)
        pinned = [i for i in range(count) if state.get(f'recent{i}', 'pinned') == '1' and
                  state.get(f'recent{i}', 'port') == str(self.port)]
        assert pinned, 'the saved address is not a pinned recent host'
        self.command(2, 'click 1180 510')
        self.expect(2, 'saving the same address twice does not duplicate it', **{'recents.count': count})
        assert b'COOP-RECENTS\t1\n' in (self.dirs[1] / 'recents.tsv').read_bytes()
        print('PASS: Save address pinned the host in recents.tsv', flush=True)

    @staticmethod
    def invite(port, routes, secret=0x12345678):
        """An invite code v1 (docs/DISCOVERY.md): routes are (kind, 'a.b.c.d'), kind 1 LAN 2 VPN 3 internet."""
        body = bytes([0x10]) + secret.to_bytes(4, 'little') + int(port).to_bytes(2, 'little') + bytes([len(routes)])
        for kind, ip in routes:
            body += bytes([kind]) + bytes(int(x) for x in ip.split('.'))
        crc = 0
        for byte in body:
            crc ^= byte
            for _ in range(8):
                crc = ((crc << 1) ^ 7) & 255 if crc & 128 else (crc << 1) & 255
        body += bytes([crc])
        bits = ''.join(f'{b:08b}' for b in body)
        bits += '0' * (-len(bits) % 5)
        chars = ''.join('0123456789ABCDEFGHJKMNPQRSTVWXYZ'[int(bits[i:i + 5], 2)] for i in range(0, len(bits), 5))
        return 'MDK2-' + '-'.join(chars[i:i + 5] for i in range(0, len(chars), 5))

    def find_row(self, player, name, timeout=12):
        """The Join list row for the host called name on our port, read from the launcher's own state."""
        deadline = time.monotonic() + timeout
        while True:
            state = self.state(player)
            rows = state.getint('join', 'rows', fallback=0)
            for i in range(rows):
                row = state[f'row{i}']
                if row.get('name') == name and row.get('port') == str(self.port):
                    return state, i, row
            if time.monotonic() >= deadline:
                raise AssertionError(f'{name} on port {self.port} not in the Join list: {rows} rows, '
                                     f'sources {[state.get("join", f"source{k}", fallback="?") for k in range(4)]}')
            time.sleep(.3)

    def join_list(self):
        """B finds A with no address typed, joins from the list, from a pasted invite, recents and Rejoin."""
        host = 'Hosty'
        self.expect(1, 'host lobby open', **{'launcher.lobby': 1})
        self.expect(2, 'joiner on the Join list, no lobby', **{'launcher.lobby': 0, 'launcher.page': 0, 'join.shown': 1})
        state, i, row = self.find_row(2, host)
        assert row.get('route') == 'LAN' and row.get('state') == '0' and row.get('mismatch') == '0', dict(row)
        assert row.get('ip') not in ('', '0.0.0.0', '255.255.255.255'), dict(row)
        assert state.getint('join', 'adapters') >= 1 and state.getint('join', 'candidates') >= 4
        print(f'PASS: {host} listed with no address typed: {dict(row)}', flush=True)
        self.shot(2, 'join_list')
        at = int(row['shown_at'])
        assert at < 3, f'{host} is row {at} on screen; only three rows are shown'
        self.command(2, f'click 1000 {430 + 44 * at + 20}')
        self.expect(2, 'row selected, primary says who', **{'join.selected': i, 'join.primary': f'Join {host}'})
        self.command(2, 'click 1000 660')
        self.expect(2, 'joined from the list', timeout=8, **{'launcher.lobby': 1, 'launcher.connected': 1})
        self.expect(1, 'host sees the joiner', timeout=6, **{'launcher.players': 2})
        state = self.expect(2, 'recent host remembered after the join', **{'recents.count': 1, 'recent0.name': host,
                                                                          'recent0.route': 'lan', 'recent0.port': self.port})
        listed_ip = row['ip']
        assert state.get('recent0', 'host') == listed_ip, (state.get('recent0', 'host'), listed_ip)
        print('PASS: recents.tsv:', (self.dirs[1] / 'recents.tsv').read_text().strip().splitlines()[1:], flush=True)
        self.command(2, 'key 27')
        self.expect(2, 'joiner left', **{'launcher.lobby': 0})
        self.expect(1, 'host lost the joiner', timeout=7, **{'launcher.players': 1})
        # two routes answer (127.0.0.2 reaches the same host): the LAN one is kept (C4)
        code = self.invite(self.port, [(3, '127.0.0.2'), (1, '127.0.0.1')])
        self.command(2, 'click 1000 366', f'text {code}')
        state = self.expect(2, 'pasted invite recognised before Join', **{'join.paste_kind': 1, 'join.primary': 'Join invite'})
        assert state.get('join', 'recognised').startswith('Invite code'), state.get('join', 'recognised')
        self.command(2, 'click 1000 660')
        self.expect(2, 'joined from a pasted invite', timeout=8, **{'launcher.lobby': 1, 'launcher.connected': 1})
        self.expect(1, 'host sees the invited joiner', timeout=6, **{'launcher.players': 2})
        state = self.expect(2, 'the invite route is a second recent host', **{'recents.count': 2})
        hosts = {state.get(f'recent{k}', 'host'): state.get(f'recent{k}', 'route') for k in range(2)}
        assert hosts.get('127.0.0.1') == 'lan', hosts
        self.command(2, 'key 27')
        self.expect(2, 'joiner left again', **{'launcher.lobby': 0})
        self.command(2, 'click 1000 185')    # Host a game: Play without the list probes only the last host
        self.expect(2, 'Rejoin <name> when the last host answers', timeout=10, **{'join.shown': 0, 'join.rejoin': f'Rejoin {host}'})
        self.shot(2, 'rejoin')
        self.command(2, f'clipboard {host} is hosting MDK2. Join: https://tommodotcommo.github.io/coop-mod-kit/join/#{code}')
        self.expect(2, 'clipboard invite offered', **{'join.offer': 1, 'join.offer_name': host, 'join.primary': f'Join {host}'})
        self.shot(2, 'clipboard_offer')
        self.command(2, 'click 1000 511')     # Not now
        self.expect(2, 'Not now dismisses the offer', **{'join.offer': 0})
        self.command(2, f'clipboard {code}')
        self.expect(2, 'a dismissed invite is not offered again', **{'join.offer': 0})
        self.command(2, 'page 5')
        self.expect(2, 'Recent hosts page scans', **{'launcher.page': 5, 'join.shown': 1, 'recents.count': 2})
        self.shot(2, 'recent_hosts')
        self.command(2, 'page 2', *['click 1220 115'] * 4)     # Settings, the section picker to Network
        self.expect(2, 'Settings > Network: find settings on by default', **{'join.find_lan': 1, 'join.find_tailscale': 1})
        self.shot(2, 'settings_network')
        self.command(2, 'page 0')

    def full(self):
        self.expect(1, 'real lobby filled by synthetic protocol members', **{'launcher.players': 16})
        self.shot(1, 'lobby_full')

    def lifecycle(self):
        for page in range(1, 6):
            self.command(2, f'page {page}')
            self.expect(2, f'page {page} retains connected lobby', **{'launcher.page': page, 'launcher.connected': 1, 'launcher.phase': 2})
            self.shot(2, f'lobby_bar_page{page}')
        self.command(1, 'page 0', 'key 27')  # Leave closes host socket, process stays alive
        self.expect(2, 'host loss while browsing', timeout=7, **{'launcher.phase': 4, 'launcher.connected': 0})
        self.shot(2, 'lobby_host_lost')
        self.command(1, 'click 1000 660')
        self.expect(2, 'reconnect without leaving other page', timeout=7, **{'launcher.phase': 2, 'launcher.connected': 1, 'launcher.page': 5})
        self.expect(1, 'host sees real clients reconnect', **{'launcher.players': 3})
        self.shot(2, 'lobby_reconnected')

    def mismatch(self):
        self.command(1, 'page 0', 'key 13')
        time.sleep(4)
        self.expect(1, 'mismatched roster refuses host start', **{'launcher.phase': 2, 'launcher.countdown': 0})
        self.expect(2, 'mismatched client remains in lobby', **{'launcher.phase': 2, 'launcher.countdown': 0})
        self.shot(2, 'version_mismatch_refused')

    def chat(self):
        # Per-member windows are independent. A single command file floods in one frame.
        base = self.state(1).getint('launcher', 'chat_total')
        self.command(1, 'say Host to clients')
        for p in (2, 3):
            self.expect(p, 'host chat received', **{f'chat{base}.text': 'Host to clients'})
        self.command(2, 'say Client to host')
        for p in (1, 3):
            self.expect(p, 'client chat relayed', **{f'chat{base + 1}.text': 'Client to host'})
        # A reconnect may reorder Alice and Bob. The sender's chat color is its
        # roster index in this three-member fixture, rather than a fixed slot.
        sender_index = self.state(1).getint(f'chat{base + 1}', 'color')
        assert sender_index in (1, 2)
        time.sleep(4.1)
        before = self.state(1).getint('launcher', 'chat_total')
        hostile = 'evil|[launcher]\t\x01\x7f=100%'
        self.command(2, 'say ' + hostile, *[f'say flood{i}' for i in range(7)])
        self.expect(1, 'host accepts four-message burst only', **{'launcher.chat_total': before + 4})
        time.sleep(.5)
        for p in (1, 2, 3):
            state = self.state(p)
            assert state.getint('launcher', 'chat_total') == before + 4
            assert state[f'chat{before}']['text'] == 'evil_[launcher]=100%'
        print('PASS: hostile text sanitized and flood limit enforced on all real peers', flush=True)
        time.sleep(4.1)
        self.command(2, 'say After flood window')
        self.expect(1, 'chat resumes after rate-limit window', **{'launcher.chat_total': before + 5})
        self.command(2, 'voice 3000')
        self.expect(2, 'synthetic local speaking indicator on', **{'launcher.local_talking': 1})
        for p in (1, 3):
            self.expect(p, 'synthetic remote speaking indicator on', **{f'player{sender_index}.talking': 1})
        self.shot(1, 'chat_and_speaking')
        self.expect(1, 'remote speaking indicator expires', timeout=5, **{f'player{sender_index}.talking': 0})
        self.expect(2, 'local speaking indicator expires', **{'launcher.local_talking': 0})
        self.command(1, 'voice 2000')
        self.expect(2, 'host synthetic voice reaches client indicator', **{'player0.talking': 1})
        self.expect(2, 'host indicator expires', timeout=4, **{'player0.talking': 0})
        self.command(2, 'page 0')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=('recents', 'full', 'lifecycle', 'mismatch', 'chat', 'join_list'))
    parser.add_argument('--dirs', nargs='+', required=True)
    parser.add_argument('--output', required=True)
    parser.add_argument('--port', required=True)
    parser.add_argument('--version', required=True)
    args = parser.parse_args()
    required = 2 if args.mode == 'join_list' else 3
    if len(args.dirs) != required:
        parser.error(f'{args.mode} requires {required} game directories')
    live = Launchers(args.dirs, Path(args.output) / args.mode)
    live.port, live.version = args.port, args.version
    getattr(live, args.mode)()
