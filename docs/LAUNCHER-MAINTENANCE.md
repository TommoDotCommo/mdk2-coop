# Launcher and installer maintenance

This revision adds installer ownership/status support and lets an existing UDP
pregame session survive launcher page navigation. It does **not** implement the
planned release downloader/self-updater, connection profiles, or pregame chat and
voice controls. Existing in-game voice remains unchanged. Steam lobby behavior
is unchanged. Do not advertise the five-feature launcher roadmap as complete.

## Installer inventory and ownership

`dist/install-state.ps1` separates pure classification (`Get-CoopFileState`,
`Get-CoopActions`, `Get-CoopLoaderState`) from read-only observations and writes.
Setup refreshes its inventory on Refresh, folder changes and window activation.
The action button reads Install, Update or Repair when appropriate; current
installs need no replacement. Unknown, modified and conflicting observations
block replacement. Uninstall may still be offered for an owned installation,
but refuses to remove modified payloads.

For a script-readable snapshot, from an unpacked package:

```powershell
powershell -NoProfile -File .\mdk2-coop.ps1 -GameDir '<game dir>' -StatusOnly -Yes
```

Each deployed DLL, Lua file and per-user marker has a sibling
`.coop-receipt.json` recording the original and installed SHA-256 and any pending
replacement. Replaced originals are kept in `.coop-original`. Copies are staged
and checked before replacing a target. Updates retain the first original;
uninstall checks that backup and restores it. Receipts contain hashes, not paths.
A missing payload can be repaired or uninstalled. An interrupted write can be
retried when the observed hash matches the installed, pending or original hash.
A missing/damaged backup or malformed receipt blocks destructive operations.
An interruption before the first receipt, or during receipt retirement, can leave
an orphan backup; resolve that conflict manually, retaining both copies.

Legacy `.coop-backup` payload backups are preserved and block replacement until
reviewed. Unreceipted payloads are kept by uninstall. An identical package payload
can be adopted by reinstalling. An older unreceipted payload is backed up as the
baseline, so uninstall restores that older payload; review legacy installs before
migrating them. Ownership cannot be inferred reliably from a DLL marker string.

Only the exact settings.lua loader block is removed. Current player options are
kept; the first settings snapshot is retained for manual recovery. A changed or
partial loader block is a conflict. Existing co-op configuration and runtime data
are kept. GUI installs copy missing older runtime files into the user's runtime
folder without overwriting existing files. No registry entries or shortcuts are
created, and the game executable is not patched. Explicit command-line network
options still edit the existing `[net]` keys and `[launcher] mode`.

Each file has an independent recovery journal. This is **not** an atomic
transaction across the game directory and user profile; an error can leave a
partial install requiring repair. Review all inventory conflicts before proceeding.
Windows UI, ACL/elevation, filesystem recovery and DPI checks must pass before
shipping this revision. The inventory does not yet expand every migrated runtime
file or individual command-line INI key into its own row.

## Pregame lifecycle

`bridge/launcher_session.h` is a portable lifecycle policy shared by the native
launcher and its simulator. States are off, waiting, lobby, countdown, lost-host
and start. Navigating pages leaves the session and its socket intact. The Play
page shows the roster; other pages show the lobby status, player count, Ready or
Start and Leave. Esc leaves; Enter activates focused page controls while browsing.
The Play page retains Enter for Ready/Start. Returning to Play does not rejoin.

A host sends a three-second countdown. Clients accept rosters only from the
configured host endpoint, require their own member ID and the declared roster
count, and reject an incompatible countdown. Countdown heartbeats do not extend
the deadline. Host loss clears the countdown and roster; hellos continue so the
client can reconnect. A client requires a host heartbeat within one second of
the deadline before entering the game. This can cancel a start during prolonged
loss; leave and rejoin if the host has already entered the game.

The launcher closes its UDP socket before the game starts. The game uses its
existing configuration and performs a fresh game handshake on that port. Lobby
membership is not transferred as an authenticated game session. UDP endpoint
matching is not cryptographic authentication; Internet transport hardening and
full parser fuzzing remain follow-up work. The existing original WinMM/IMA ADPCM
in-game audio stack should be reused for pregame voice rather than adding a codec
dependency. It is not yet connected to the launcher lifecycle.

Hidden launcher tests support `page 0..4` and `dump` in `launcher.txt`. Dump writes
`launcher-state.ini` from process state (page, lifecycle, roster count, ready,
connected and countdown, plus each player's ready state). It contains no addresses
or player names. These commands are enabled only by the existing test-launcher path.

## Release checksum convention

`tools/release.sh <version>` produces `mdk2-coop-<version>.zip` and
`mdk2-coop-<version>.zip.sha256`, in standard `sha256sum` format with the exact ZIP
basename. Publish both assets together when a maintainer separately authorizes a
release. Neither asset is published by this script. Future update clients must
select the exact versioned ZIP and matching checksum, validate both declared size
and SHA-256 before extraction, and keep the previous package for rollback.
A checksum served by the same release origin detects corruption; it is not an
independent publisher signature. Downloading, safe extraction, version comparison,
rate limiting, prompts and safe replacement of the loaded launcher DLL are still
unimplemented.

## Ordered verification

Game-free commands on a development host:

```sh
python3 tools/test_launcher_session.py
make -C bridge
bash -n tools/release.sh tools/qa_installer.sh tools/qa_lobby.sh
python3 -m py_compile tools/test_launcher_session.py tools/check_launcher_state.py tools/qa_all.py
git diff --check
```

The simulator must pass 100,000 generated sessions and kill all 12 source mutants.
The build must produce the Windows DLL without warnings. To validate a locally
built package without publishing:

```sh
bash tools/release.sh v0.3.2-test
python3 tools/check_release_package.py local/release/mdk2-coop-v0.3.2-test.zip
```

The package check verifies the published checksum convention, exact inventory,
Windows binary headers and the included policy source. The complete offline
profile contains other existing Windows executables and is not Linux-only.

On Windows, before any game tests:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/test_install_state.ps1 -Mutants
```

This must pass the pure policy matrix and disposable filesystem checks, then kill
all ten installer mutants. Fixtures cover original restoration, update retaining
originals, missing payloads, foreign modifications, corrupt receipts/backups,
identical-payload adoption and unchanged sibling saves. This test has not been
run on a host without PowerShell. Reparse-point rejection still needs dedicated
junction fixtures/mutants.

Then run the existing installer gate on configured disposable copies:

```sh
MDK2_BACKGROUND=1 tools/qa_installer.sh
```

It launches no game, reads a source executable, runs the new policy/mutant suite,
then exercises the actual installer. It must report nonzero installer checks and
zero failures, including exact DLL/Lua original restoration, save preservation,
new settings surviving uninstall and setup control bounds/non-overlap after
scaling at 100%, 125%, 150% and 200%. Check actual screen fit on smaller work areas
separately; control geometry alone cannot establish text legibility.

Finally, with three configured disposable game copies:

```sh
MDK2_BACKGROUND=1 tools/qa_lobby.sh
MDK2_BACKGROUND=1 MUTANT=nostart tools/qa_lobby.sh
```

The normal run must show page 1, 2, 3 and 4 with lobby=1, connected=1, players=3,
phase=2 in the client's process-written dump, and the host must retain all three
members. Ready must appear in both process dumps. The original start/connection
checks must then pass. The `nostart` run must fail start checks. Older checks in
this harness still use logs; only the new browse/ready assertions use process
state. Host-loss/reconnect and endpoint filtering still require live state-based
adapter scenarios before this revision is release-ready.
