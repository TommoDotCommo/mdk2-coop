# Launcher and installer maintenance

This document covers the launcher and installer architecture for MDK2 Co-op,
including installer status verification, background GitHub update checking and rollback,
saved-IP profile management, persistent lobby session browsing, and pregame lobby
text and voice communication.

## 1. Installer inventory and ownership

`dist/install-state.ps1` separates pure classification (`Get-CoopFileState`,
`Get-CoopActions`, `Get-CoopLoaderState`) from read-only observations and writes.
`dist/install-policy.h` provides the pure portable policy engine (`ci_file`, `ci_actions`,
`ci_loader`), compiled to `dist/install-policy.exe` via `tools/build_install_policy.sh`.

Setup refreshes its inventory on demand ("Refresh" button), when the game path changes,
and whenever the window regains focus. The action button dynamically reflects system state:
- "Install" appears when unmanaged or missing payloads require deployment.
- "Update" appears when an existing installation has outdated files matching the release.
- "Repair" appears when expected managed files are missing or modified.
- "Needs attention" appears (disabled) if foreign modifications, unreadable files, or conflicts exist.
- "Up to date" appears (disabled) when all files match current release hashes.
- "Uninstall" enables only when an owned installation exists, restoring original files from
  verified `.coop-original` backups while strictly preserving user saves and settings.

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

Legacy `.coop-backup` payload backups are preserved and block replacement until
reviewed. Unreceipted payloads are kept by uninstall. An identical package payload
can be adopted by reinstalling.

Only the exact settings.lua loader block is removed. Current player options are
kept; the first settings snapshot is retained for manual recovery. Existing co-op
configuration and runtime data are kept. GUI installs copy missing older runtime
files into `%LOCALAPPDATA%\mdk2-coop\coop` without overwriting existing files.
No registry entries or shortcuts are created, and the game executable is not patched.

## 2. GitHub auto-update and rollback

`bridge/launcher_update.h` and `bridge/launcher_update_win.h` implement GitHub release
discovery and safe self-updating:
- Queries the unauthenticated public GitHub API (`/repos/TommoDotCommo/mdk2-coop/releases?per_page=10`),
  with configurable `[update] api_url` for testing with a local fake server.
- Enforces an 86,400s (24h) auto-check interval (`lu_can_check`), honors HTTP 429 and `Retry-After` /
  `X-RateLimit-Remaining` rate limits, and tolerates offline networks gracefully.
- Compares semantic versions (`lu_version_is_newer`) and extracts release notes.
- Locates the release ZIP and its matching `.zip.sha256` sidecar asset.
- Downloads assets in memory up to 32 MB, verifying declared size and SHA-256 hash.
- Safely extracts files with DEFLATE inflation (`lu_inflate`), checking CRC-32 and PE32 headers.
- Replaces running files atomically (`lu_apply_package`): moves current `dinput8.dll` to
  `dinput8.dll.previous`, writes new files, updates `update-state.ini`, and synchronizes
  installer receipts.
- Automatic rollback (`lu_rollback`): if any step fails or an interrupted update is detected on
  startup (`lu_recover_interrupted`), the verified `.previous` backups are restored.
- The user retains full control via the "Auto-check" toggle on the About page and manual "Check now" /
  "Install update" / "Roll back" buttons.

### Release layout and the updater

`tools/release.sh <version>` produces, in `local/release/`:

| Asset | Used by |
|---|---|
| `MDK2-Coop-Setup.exe` + `MDK2-Coop-Setup.exe.sha256` | players: the main download. The zip's files are embedded as a payload (`tools/payload.py`, read by `dist/installer.c` and `dist/setup-core.h`), checked against a SHA-256 stored in the exe, and unpacked into a fresh `%LOCALAPPDATA%\mdk2-coop\setup\<version>[-n]` folder (exclusive create, never through a link) before `installer-gui.ps1` runs from there. `Setup.exe /extract <new folder>` only unpacks. |
| `mdk2-coop-<tag>.zip` + `mdk2-coop-<tag>.zip.sha256` | the in-game updater, manual installs, Nexus. Its `MDK2-Coop-Setup.exe` carries no payload and starts the scripts beside it; run from inside a zip (scripts missing, or a path under `%TEMP%` with a `*.zip` folder) it says "Extract the whole zip first, or download the single Setup.exe". |

The updater (`bridge/launcher_update.h`) is unchanged by the single-file Setup and needs exactly the old things: the
asset `mdk2-coop-<tag>.zip` with its `.zip.sha256`, and the members `mdk2-coop-<tag>/dinput8.dll` and
`mdk2-coop-<tag>/coop/coop.lua`. The Setup.exe assets are ignored by its asset matching (it matches names exactly).
**Keep it that way**: do not rename the zip or its top folder, and keep both files in it. `tools/test_update_layout.py`
checks this against a real `release.sh` build (release JSON with all four assets, apply, rollback; mutants for a wrong
root folder and a missing sidecar). The payload zip stays far below the updater's 32 MB limit (the Setup.exe is never
downloaded by it).

Windows-side checks for the Setup exe (`tools/test_setup_exe.py`: extraction, damaged payload, junctions, zip message,
mutants) and for the plain build messages (`tools/test_installer_builds.ps1`, `tools/qa_build_message.sh`) run in
`tools/qa_installer.sh` and `tools/qa_build_message.sh`; `tools/test_setup_core.py`, `tools/test_install_linux.sh`,
`tools/test_release_signing.sh` and `tools/test_update_layout.py` are game-free offline gates.

`tools/test_launcher_update_win.py` loads an old and a new DLL in one process and unloads the old one. Build both packages
from the same tree (as the commands above do): an old DLL from a different source version leaks its exception handler,
which then points into the new DLL's code. The current DLL removes its handler when it is unloaded.

## Code signing

Windows SmartScreen and antivirus heuristics treat an unsigned exe that starts PowerShell with
`-ExecutionPolicy Bypass` with suspicion. A code-signing certificate fixes the first and helps with the second.
`tools/release.sh` is ready for one; nothing in the repository holds a certificate.

```sh
# a command template that signs one file in place; {file} is replaced by the quoted path
SIGN_CMD='osslsigncode sign -pkcs12 "$HOME/sign.p12" -pass "$SIGN_PASS" -h sha256 -t http://timestamp.digicert.com -in {file} -out {file}.signed && mv {file}.signed {file}' \
SIGN_VERIFY_CMD='osslsigncode verify -in {file}' \
  bash tools/release.sh v0.x.y
# signtool from a Windows shell works too: SIGN_CMD='signtool.exe sign /fd sha256 /tr http://timestamp.digicert.com /td sha256 /a {file}'
python3 tools/check_release_package.py local/release/mdk2-coop-v0.x.y.zip --require-signed
```

What the script does when `SIGN_CMD` is set: signs `dinput8.dll`, `install-policy.exe` and the zip's `MDK2-Coop-Setup.exe`
**before** the single-file Setup's payload is hashed and embedded (so the embedded copies are the signed ones, and the
receipts and the updater's hashes cover signed bytes); builds and signs the single-file `MDK2-Coop-Setup.exe` last; after
each signature it checks that the file now carries a certificate table, plus `SIGN_VERIFY_CMD` if you set one; any failure
aborts the release. Unset `SIGN_CMD`, and it builds unsigned with a warning. `--require-signed` fails unless every shipped
exe and dll (and the embedded copies, which must equal the zip's) is signed. The scripts (`*.ps1`) are not signed: a signed
Setup that starts them with Bypass is the common installer pattern, and the zip's scripts are plain text.
`tools/test_release_signing.sh` proves the hook with a fake signer, including a `RELEASE_MUTANT=late-sign` build that the
checker must reject.

Routes for the certificate (the owner chooses):

- **SignPath Foundation**: free for open-source projects; a hosted signer with an approval workflow, so `SIGN_CMD` would call
  their CLI/API with the built file (their rules: public repository, OSI licence, a build you can show; apply on their site).
- **Certum "Open Source Code Signing"**: a cheap yearly certificate on a smart card or a cloud token; it works with
  `osslsigncode`/`signtool`.
- **Azure Trusted Signing** (about USD 10 a month): a Microsoft-run service; check its current eligibility for individual
  developers in your country. It signs with `signtool` plus its dlib, which fits `SIGN_CMD`.

A new certificate has no SmartScreen reputation at first; the warning fades after enough downloads (an EV certificate skips
that wait but costs much more). Signing alone does not stop an antivirus false positive on the first release.

## Submitting a false positive to Microsoft Defender (wdsi)

If Defender or SmartScreen flags a release (Setup.exe, the zip or `dinput8.dll`):

1. Go to https://www.microsoft.com/en-us/wdsi/filesubmission and sign in with a Microsoft account (the portal's wording and
   options change from time to time: follow what the page offers, the steps below are the usual path).
2. Choose **Software developer**, then **Submit as: I believe this file is incorrectly detected** (not "malware").
3. Upload the flagged file (a zip works; use the password `infected` only if the form asks for one) and fill in the detection name if
   Windows showed it (Security > Protection history).
4. In the additional information, say what it is: "MDK2 Co-op, an open-source co-op mod for the game MDK2 HD
   (https://github.com/TommoDotCommo/mdk2-coop, MIT). `dinput8.dll` is a DirectInput proxy that the game loads; Setup starts
   PowerShell scripts that are included in readable form. Source, SHA-256 of the release files and the build script are public."
   Give the release URL and the file's SHA-256.
5. Submit one file per request, note the submission ID, and check the result in the portal (usually within a few days). Resubmit
   each new release's flagged files, or sign them (above) and say so.
6. Other vendors have their own forms (VirusTotal lists who flags a file: submit to each flagging vendor through its
   "report a false positive" page).

## 3. Saved-IP profiles

`bridge/launcher_profiles.h` and `bridge/launcher_profiles_win.h` manage connection profiles:
- Profiles store name, host/IP, port, notes, and last-used timestamp (`struct lp_profile`).
- Up to 32 profiles stored in `coop/profiles.tsv` via transactional encode/decode (`lp_encode`, `lp_decode`).
- Supports automatic v0-to-v1 format migration and tab-separated export/import (`profiles-export.tsv`,
  `profiles-import.tsv`).
- Background UDP probing (`lp_probe`, 15-second interval, 1200ms timeout) queries server status
  using fixed-width binary packets (`CPQ1` query, `CPS1` response).
- Connection status shown live in the UI without freezing: Checking, No answer (offline), Reachable,
  In lobby, In game, Version mismatch, Full, alongside round-trip time (RTT in ms) and last-used time.
- Selecting a profile enables one-click "Join" or "Host", automatically populating connection settings.
- Profile addresses are kept private and never leaked or transmitted to third parties.

## 4. Persistent lobby session and page browsing

`bridge/launcher_session.h` provides an independent session state machine:
- Session states: `LS_OFF`, `LS_WAITING`, `LS_LOBBY`, `LS_COUNTDOWN`, `LS_LOST`, `LS_START`.
- Joining or hosting creates a persistent session that remains active while the user navigates
  freely between launcher tabs (Play, Saves, Settings, About, Profiles, Help).
- On non-Play pages, a persistent bottom lobby bar displays current status (Hosting/Connected/Reconnecting),
  player count, speaking indicator, chat count, and Ready/Start/Leave buttons.
- Host disappearance transitions clients to `LS_LOST`, retaining reconnection attempts while
  updating the UI.
- On countdown completion (`LS_START`), the launcher exits cleanly, releases its UDP socket, writes
  effective connection parameters to `coop.ini`, and hands off execution to the game client.

## 5. Pregame lobby text and voice chat

Pregame communication is integrated directly into the lobby lifecycle:
- **Text chat**:
  - Encoded with wire protocol `MDKL C1` (client to host) and `MDKL M1` (host relay).
  - Ring buffer of 16 messages (`LS_CHAT_HISTORY`), with 8 distinct high-contrast colors (`LS_COLORS`, `ls_color_rgb`).
  - Flood control enforced: burst cap of 4 messages per 4-second window (`LS_CHAT_WINDOW_MS`).
  - Untrusted text sanitised via `ls_sanitize_text`: control characters stripped, pipe delimiters escaped,
    length capped to 120 characters (`LS_CHAT_MAX`), preventing UI formatting injection.
- **Voice chat**:
  - Reuses the existing original WinMM/IMA ADPCM audio stack (`bridge/voice.c`) with zero external codec dependencies.
  - Recorded licence: original MIT code.
  - Wire protocol `MDKV` version 1, 16 kHz mono, 40 ms frames (640 samples, 320 bytes payload).
  - Push-to-talk (default key 'B') and Voice Activity Detection (VAD threshold 1000, hangover 250 ms).
  - Device selection for both input (microphone) and output (speakers) via WinMM enumeration.
  - Per-player mute and volume scaling (0% to 200%).
  - Live speaking indicators (`ls_local_talking`, `ls_member_talking`, `[SPEAKING]` tag).
  - Member-only routing: non-member packets rejected; audio frames never saved or written to disk.

## 6. UI layout and DPI verification

All launcher screens are authored in logical 1280x720 coordinates and dynamically scaled via
per-thread DPI awareness context (`SetThreadDpiAwarenessContext`).
`tools/test_ui_layout.py` verifies game-free geometry across:
- 100% (1280x720)
- 125% (1600x900)
- 150% (1920x1080)
- 200% (2560x1440)
- 300% (3840x2160)
These checks cover the geometry encoded in the test. They do not establish text
legibility or actual monitor DPI behavior. The Windows window test below exercises
the real installer controls and captures them without activating the window; inspect
its PNGs as well. Its doubled geometry case is not an actual monitor DPI change.

## 7. Ordered verification commands

### Game-free tests (Linux / CI / development host)

```sh
# 1. Native Windows DLL cross-build
make -C bridge

# 2. Pure install policy, actions, loader parser & 21 mutants
python3 tools/test_install_policy.py

# 3. Profile store, migration, probe scheduling & 25 mutants + local fake server
python3 tools/test_launcher_profiles.py

# 4. Lobby lifecycle, pregame chat/voice simulator & 38 mutants (100,000 sessions)
python3 tools/test_launcher_session.py

# 5. GitHub release checker, SHA-256 sidecar, ZIP/PE verification & 21 mutants + fake server
python3 tools/test_launcher_update.py

# 6. UI layout & DPI geometry verification at 100%, 125%, 150%, 200%, 300%
python3 tools/test_ui_layout.py

# 7. Release packaging and SHA-256 sidecar verification (zip, single-file Setup, embedded payload)
bash tools/release.sh v0.3.2-test
python3 tools/check_release_package.py local/release/mdk2-coop-v0.3.2-test.zip

# 7b. One-file Setup and plain-build-message cores, Linux install script, signing hook, updater layout (all with mutants)
python3 tools/test_setup_core.py
bash tools/test_install_linux.sh
bash tools/test_release_signing.sh
python3 tools/test_update_layout.py

# 8. Python syntax check
python3 -m py_compile tools/test_install_policy.py tools/test_launcher_profiles.py \
    tools/test_launcher_session.py tools/test_launcher_update.py tools/test_ui_layout.py \
    tools/check_launcher_state.py tools/check_release_package.py tools/qa_all.py

# 9. Shell script syntax check
bash -n tools/release.sh tools/qa_installer.sh tools/qa_lobby.sh tools/build_install_policy.sh
```

### Windows verification (verifier host with Windows / PowerShell)

```powershell
# 1. PowerShell installer policy, filesystem fixtures and mutants
powershell -NoProfile -ExecutionPolicy Bypass -File tools/test_install_state.ps1 -Mutants

# 2. Installer GUI DPI scaling layout checks (100%, 125%, 150%, 200%)
powershell -NoProfile -ExecutionPolicy Bypass -File dist/installer-gui.ps1 -TestLayout

# 3. Full installer fixture gate on disposable copy
bash tools/qa_installer.sh

# 4. Multi-client lobby lifecycle, persistent browsing, chat and voice gate (3 instances)
bash tools/qa_lobby.sh

# Optional extended checks: profile UI and real-host probes, full lobby with
# synthetic protocol members, loss/reconnect, bidirectional hostile/flood chat,
# synthetic speaking indicators and a process status probe after game handoff.
MDK2_EXTENDED=1 bash tools/qa_lobby.sh

# 5. Lobby start-protection mutant check (host countdown withheld)
MUTANT=nostart bash tools/qa_lobby.sh
```

The `nostart` variant withholds the host countdown and succeeds only when the
game-start oracle rejects the resulting lobby state. It also requires both
launchers to remain in the lobby without a countdown.

Extended checks archive and replace profile fixtures in the configured disposable
copies. `tools/check_lobby_live.py` can also drive individual checks against three
already running background test launchers. Its `mismatch` mode requires an existing
imported profile and a real host built with a different version; it verifies both
the profile warning and refusal to start. Dumps and captures are saved alongside
the suite output. The profile probe's successful live-lobby state is **In lobby**
with version and RTT. The separate **Reachable** label is reserved by the probe
format; current production responders report either In lobby or In game.

### Additional native Windows verification from WSL

`tools/test_launcher_audio.sh` enumerates input/output devices through the production
WinMM adapter without opening them, then checks the synthetic source's amplitude,
40 ms pacing and expiry. Real microphone capture, permission prompts, acoustics,
physical push-to-talk, and audible playback still need a hardware check.

The following commands use disposable scratch folders on a Windows filesystem:

```sh
# Real installer button states plus PrintWindow captures, without taking focus.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/test_installer_window.ps1 \
  -PackageDir '<unpacked release>' -TestRoot '<Windows scratch folder>'

# Build two local packages; publishing is a separate operation.
bash tools/release.sh v0.3.2-test
bash tools/release.sh v0.4.0-test
python3 tools/test_launcher_update_win.py --test-root '<WSL path to Windows scratch folder>' \
  --old-package local/release/mdk2-coop-v0.3.2-test.zip \
  --new-package local/release/mdk2-coop-v0.4.0-test.zip
```

The updater fixture includes the production WinINet adapter and loads the real
bridge DLL in a harmless executable. It uses an explicit non-elevating manifest.
It checks a local release server and attempts to replace the mapped DLL, reload
its replacement and restore verified originals. It does not launch MDK2 or prove
that a restarted game's launcher displays the new version. `--try-real-api` adds
one unauthenticated read-only request and prints the HTTP result, including 404
when no latest release exists.

`tools/check_launcher_update_live.py` covers the actual game restart. Supply a
stopped disposable game directory configured for background launcher testing, old
and new local packages with their version strings, an evidence output directory,
and `--start-command` / `--stop-command` argument lists. The caller's commands must
register the launched game and stop only that process under its resource lease.
The test hosts a local release server, updates the running game DLL, verifies its
bytes, restarts through those commands, checks the process's new version and
captures the About page. It never requests the public GitHub API.
