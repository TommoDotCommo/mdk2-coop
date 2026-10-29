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

    def profiles(self):
        self.command(2, 'page 5', 'click 980 452')  # Add
        self.field(2, 900, 185, 'Live host')
        self.field(2, 900, 258, '127.0.0.1')
        self.field(2, 1180, 258, self.port)
        self.field(2, 900, 330, 'Original notes')
        self.command(2, 'click 800 395')
        self.expect(2, 'profile added through UI', **{'profiles.count': 1, 'profile0.name': 'Live host', 'profile0.port': self.port})
        state = self.expect(2, 'real host probe reports lobby and matching version', timeout=8,
                            **{'profile0.status': 3, 'profile0.version': self.version})
        assert 0 <= state.getint('profile0', 'rtt') < 1200
        print('PASS: measured real-host profile RTT', state['profile0']['rtt'], flush=True)
        self.command(2, 'click 1050 452')  # Edit
        self.field(2, 900, 330, 'Edited notes')
        self.command(2, 'click 800 395', 'click 1030 494')  # Save, Export
        export = self.dirs[1] / 'profiles-export.tsv'
        saved = (self.dirs[1] / 'profiles.tsv').read_bytes()
        assert export.read_bytes() == saved and b'Edited notes' in saved
        self.command(2, 'click 1050 452', 'click 1140 395')  # Edit, Delete
        self.expect(2, 'profile deleted through UI', **{'profiles.count': 0})
        (self.dirs[1] / 'profiles-import.tsv').write_bytes(export.read_bytes())
        self.command(2, 'click 1150 494')  # Import
        self.expect(2, 'export imported through UI', **{'profiles.count': 1, 'profile0.notes': 'Edited notes'})
        self.expect(2, 'imported profile probes real host', timeout=8, **{'profile0.status': 3})
        self.shot(2, 'profiles_live')

    def saved_join(self):
        self.command(2, 'page 5')
        state = self.state(2)
        count = state.getint('profiles', 'count')
        existing = any(state.get(f'profile{i}', 'host') == '127.0.0.1' and
                       state.get(f'profile{i}', 'port') == str(self.port) for i in range(count))
        expected = count if existing else count + 1
        self.command(2, 'page 0', 'click 1180 510')
        self.expect(2, 'save host address without leaving lobby',
                    **{'launcher.lobby': 1, 'launcher.connected': 1, 'profiles.count': expected})
        saved = (self.dirs[1] / 'profiles.tsv').read_bytes()
        assert ('\t127.0.0.1\t' + str(self.port) + '\t').encode() in saved
        self.command(2, 'click 1180 510')
        self.expect(2, 'saving the same address twice does not duplicate it', **{'profiles.count': expected})
        assert (self.dirs[1] / 'profiles.tsv').read_bytes() == saved
        self.command(2, 'click 1000 660')
        self.expect(2, 'leave before joining a saved address', **{'launcher.lobby': 0})
        self.expect(1, 'host removes departed client', timeout=6, **{'launcher.players': 1})
        self.command(2, 'page 5', 'click 800 494')
        self.expect(2, 'saved Join opens the connected lobby on Play',
                    **{'launcher.page': 0, 'launcher.lobby': 1, 'launcher.connected': 1, 'launcher.players': 2})
        self.expect(1, 'saved Join reaches the real host', **{'launcher.players': 2})
        self.shot(2, 'saved_address_joined_lobby')

    def full(self):
        self.expect(1, 'real lobby filled by synthetic protocol members', **{'launcher.players': 16})
        # Start a new probe only after all synthetic hellos have been consumed.
        # Otherwise a valid early three-player reply is cached for 15 seconds.
        time.sleep(.2)
        self.command(2, 'page 5', 'click 1150 494')  # Import resets probe generation
        self.expect(2, 'profile reports real host full', timeout=8, **{'profile0.status': 6})
        self.shot(2, 'profiles_full')

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
        self.command(2, 'page 5', 'click 1150 494')
        self.expect(2, 'profile reports different real host version', timeout=8, **{'profile0.status': 5})
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
    parser.add_argument('mode', choices=('profiles', 'full', 'lifecycle', 'mismatch', 'chat', 'saved_join'))
    parser.add_argument('--dirs', nargs='+', required=True)
    parser.add_argument('--output', required=True)
    parser.add_argument('--port', required=True)
    parser.add_argument('--version', required=True)
    args = parser.parse_args()
    required = 2 if args.mode == 'saved_join' else 3
    if len(args.dirs) != required:
        parser.error(f'{args.mode} requires {required} game directories')
    live = Launchers(args.dirs, Path(args.output) / args.mode)
    live.port, live.version = args.port, args.version
    getattr(live, args.mode)()
