#!/usr/bin/env python3
"""The in-game updater (bridge/launcher_update.h) against fixture folders, game-free (co-op mod standard J4, J6, J7):

  setup      files with Setup's receipts: a start, an update, a rollback and the next start leave the game folder's file list
             and hashes as they were, apart from the files the package ships (and their receipts); every backup and the
             update state are in the data folder
  legacy     the dinput8.dll.previous, coop/coop.lua.previous and coop/update-state.ini an older updater (v0.3.5) wrote into
             the game folder move to the data folder at the first start, verified
  copy       no receipt (a mod manager or a copy by hand): the updater says so and replaces nothing
  hardlink   no receipt and hard-linked files (Vortex): reported as such, nothing replaced, the manager's files untouched
  linux      install-linux.sh's record counts as Setup; its hashes follow the update and the rollback

Mutants that must fail:
  backup-beside-dll    the updater keeps its backups and state in the game folder (the old layout)
  ignore-owner         the updater replaces files Setup did not install
  keep-update-old      the next start does not remove the DLL renamed aside during the update
  no-legacy-move       old .previous files stay in the game folder
  no-record-sync       install-linux.sh's record keeps the old hash after an update"""
import hashlib
import io
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / 'bridge/launcher_update.h').read_text()
TAG = 'v0.4.0'
PACKAGE = {'dinput8.dll', 'coop/coop.lua', 'dinput8.dll.coop-receipt.json', 'coop/coop.lua.coop-receipt.json'}

MUTANTS = {
    'backup-beside-dll': ('lu_update_paths(data_dir, state_path, dll_prev, lua_prev);', 'lu_update_paths(game_dir, state_path, dll_prev, lua_prev);'),
    'adopted-blocked': ('return LU_OWNER_SETUP;', 'return LU_OWNER_COPY;'),
    'ignore-owner': ('if (lu_install_owner(game_dir) != LU_OWNER_SETUP) return LU_ERR_NOT_OWNED;', ''),
    'keep-update-old': ('if (lu_link_count(old) && remove(old) != 0) left++;', ''),
    'no-legacy-move': ('        remove(from);\n', '        (void)from;\n'),
    'no-record-sync': ('fprintf(f, "%s%s\\n", new_hash, sp1);', 'fprintf(f, "%s\\n", line);'),
}


def pe(fill):
    b = bytearray([fill] * 512)
    b[0:2] = b'MZ'
    b[60:64] = (64).to_bytes(4, 'little')
    b[64:68] = b'PE\x00\x00'
    b[68:70] = (0x014c).to_bytes(2, 'little')
    return bytes(b)


OLD_DLL, OLD_LUA, NEW_DLL, NEW_LUA = pe(0x11), b'-- coop.lua v0.3.5\n', pe(0x22), b'-- coop.lua v0.4.0\n'


def sha(data):
    return hashlib.sha256(data).hexdigest()


def snapshot(folder):
    return {p.relative_to(folder).as_posix(): sha(p.read_bytes()) for p in sorted(folder.rglob('*')) if p.is_file()}


def receipt(data):
    return '{"schema":1,"original":null,"installed":"%s","pending":null}' % sha(data)


def make_game(base, name, how):
    game = base / name
    (game / 'coop').mkdir(parents=True)
    (game / 'mdk2hd.exe').write_bytes(b'GAME-EXE')
    (game / 'readme-of-the-game.txt').write_bytes(b'game file')
    if how == 'hardlink':
        staging = base / (name + '-staging')
        (staging / 'coop').mkdir(parents=True)
        (staging / 'dinput8.dll').write_bytes(OLD_DLL)
        (staging / 'coop/coop.lua').write_bytes(OLD_LUA)
        os.link(staging / 'dinput8.dll', game / 'dinput8.dll')
        os.link(staging / 'coop/coop.lua', game / 'coop/coop.lua')
    else:
        (game / 'dinput8.dll').write_bytes(OLD_DLL)
        (game / 'coop/coop.lua').write_bytes(OLD_LUA)
    if how in ('setup', 'legacy'):
        (game / 'dinput8.dll.coop-receipt.json').write_text(receipt(OLD_DLL))
        (game / 'coop/coop.lua.coop-receipt.json').write_text(receipt(OLD_LUA))
    if how == 'linux':
        (game / 'coop/install-linux.record').write_text('%s new dinput8.dll\n%s new coop/coop.lua\n' % (sha(OLD_DLL), sha(OLD_LUA)))
    return game


def scenarios(binary, base, zip_path):
    """Returns the failed expectations."""
    bad = []

    def run(*args):
        r = subprocess.run([str(binary)] + [str(a) for a in args], capture_output=True, text=True)
        if r.returncode:
            raise RuntimeError('%s: exit %d' % (args, r.returncode))
        return int(r.stdout.strip())

    def expect(cond, what):
        if not cond:
            bad.append(what)

    # setup: start, update, rollback, start again
    game, data = make_game(base, 'setup', 'setup'), base / 'setup-data'
    before = snapshot(game)
    expect(run('owner', game) == 0, 'setup: receipts not recognised as Setup')
    run('startup', game, data)
    expect(run('apply', game, data, zip_path, TAG) == 0, 'setup: update did not apply')
    expect((game / 'dinput8.dll').read_bytes() == NEW_DLL, 'setup: dinput8.dll not updated')
    expect(sorted(p.name for p in data.iterdir()) == ['coop.lua.previous', 'dinput8.dll.previous', 'update-state.ini'],
           'setup: data folder does not hold the backups and state: %s' % sorted(p.name for p in data.iterdir()))
    expect(run('startup', game, data) == 0, 'setup: leftovers in the game folder after the next start')
    mid = snapshot(game)
    changed = {k for k in set(before) | set(mid) if before.get(k) != mid.get(k)}
    expect(changed <= PACKAGE, 'setup: the update changed game-folder files the package does not ship: %s' % sorted(changed - PACKAGE))
    expect(run('rollback', game, data) == 0, 'setup: rollback failed')
    run('startup', game, data)
    after = snapshot(game)
    expect(after == before, 'setup: game folder differs after update + rollback + start: %s'
           % sorted(k for k in set(before) | set(after) if before.get(k) != after.get(k)))

    # legacy: files of the v0.3.5 updater move to the data folder at the first start
    game, data = make_game(base, 'legacy', 'legacy'), base / 'legacy-data'
    (game / 'dinput8.dll.previous').write_bytes(pe(0x05))
    (game / 'coop/coop.lua.previous').write_bytes(b'-- older\n')
    (game / 'coop/update-state.ini').write_text('[update]\nstatus=applied\nprevious_version=v0.3.3-alpha\n')
    run('startup', game, data)
    expect(not (game / 'dinput8.dll.previous').exists() and not (game / 'coop/coop.lua.previous').exists()
           and not (game / 'coop/update-state.ini').exists(), 'legacy: old updater files still in the game folder')
    expect((data / 'dinput8.dll.previous').exists() and (data / 'dinput8.dll.previous').read_bytes() == pe(0x05)
           and (data / 'update-state.ini').exists(), 'legacy: old updater files did not arrive in the data folder')

    # copy and hardlink: nothing replaced
    for how, owner in (('copy', 1), ('hardlink', 2)):
        game, data = make_game(base, how, how), base / (how + '-data')
        before = snapshot(game)
        expect(run('owner', game) == owner, '%s: owner not %d' % (how, owner))
        run('startup', game, data)
        expect(run('apply', game, data, zip_path, TAG) == 17, '%s: the updater did not refuse (LU_ERR_NOT_OWNED)' % how)
        expect(snapshot(game) == before, '%s: the updater changed the game folder' % how)
        if how == 'hardlink':
            staging = base / 'hardlink-staging'
            expect((staging / 'dinput8.dll').read_bytes() == OLD_DLL, 'hardlink: the manager\'s staging file changed')
            expect(os.stat(game / 'dinput8.dll').st_nlink == 2, 'hardlink: the link was broken')

    # A manual install after Setup adoption has receipts and updates normally.
    game, data = make_game(base, 'adopted', 'copy'), base / 'adopted-data'
    (game / 'dinput8.dll.coop-receipt.json').write_text(receipt(OLD_DLL))
    (game / 'coop/coop.lua.coop-receipt.json').write_text(receipt(OLD_LUA))
    expect(run('owner', game) == 0, 'adopted: Setup receipts not recognised')
    expect(run('apply', game, data, zip_path, TAG) == 0, 'adopted: manual install still blocked')
    expect((game / 'dinput8.dll').read_bytes() == NEW_DLL, 'adopted: DLL not updated')
    expect(run('rollback', game, data) == 0, 'adopted: rollback failed')

    # linux: the record follows the files
    game, data = make_game(base, 'linux', 'linux'), base / 'linux-data'
    expect(run('owner', game) == 0, 'linux: install-linux.sh record not recognised')
    expect(run('apply', game, data, zip_path, TAG) == 0, 'linux: update did not apply')
    rec = (game / 'coop/install-linux.record').read_text()
    expect(rec == '%s new dinput8.dll\n%s new coop/coop.lua\n' % (sha(NEW_DLL), sha(NEW_LUA)), 'linux: record not updated: ' + rec)
    expect(run('rollback', game, data) == 0, 'linux: rollback failed')
    rec = (game / 'coop/install-linux.record').read_text()
    expect(rec == '%s new dinput8.dll\n%s new coop/coop.lua\n' % (sha(OLD_DLL), sha(OLD_LUA)), 'linux: record not rolled back')
    return bad


def build(folder, source):
    (folder / 'launcher_update.h').write_text(source)
    binary = folder / 'owner_test'
    subprocess.run(['cc', '-std=c99', '-O1', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-Wno-unused-parameter',
                    '-I', str(folder), str(ROOT / 'bridge/tests/update_owner_test.c'), '-o', str(binary)], check=True)
    return binary


def main():
    zip_bytes = io.BytesIO()
    with zipfile.ZipFile(zip_bytes, 'w', zipfile.ZIP_DEFLATED) as z:
        z.writestr('mdk2-coop-%s/dinput8.dll' % TAG, NEW_DLL)
        z.writestr('mdk2-coop-%s/coop/coop.lua' % TAG, NEW_LUA)
        z.writestr('coop-mod.json', '{}')
    failures = []
    for label, (before, after) in [('real', (None, None))] + list(MUTANTS.items()):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            (tmp / 'src').mkdir()
            if before is not None:
                assert SOURCE.count(before) >= 1, 'mutant %s does not apply' % label
            binary = build(tmp / 'src', SOURCE if before is None else SOURCE.replace(before, after))
            zip_path = tmp / 'release.zip'
            zip_path.write_bytes(zip_bytes.getvalue())
            (tmp / 'games').mkdir()
            try:
                bad = scenarios(binary, tmp / 'games', zip_path)
            except RuntimeError as e:
                bad = [str(e)]
            if label == 'real':
                failures += bad
            elif not bad:
                failures.append('mutant %s survived' % label)
            else:
                print('KILLED: %s (%s)' % (label, bad[0]))
    if failures:
        print('\n'.join('FAIL: ' + f for f in failures))
        return 1
    print('PASS: updater leaves the game folder as it was (start, update, rollback), moves old backups out, replaces only '
          "Setup's files (copy and hard-linked refused), keeps install-linux.sh's record; %d mutants killed" % len(MUTANTS))
    return 0


if __name__ == '__main__':
    sys.exit(main())
