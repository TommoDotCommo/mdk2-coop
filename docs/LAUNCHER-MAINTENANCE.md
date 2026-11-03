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

For a script-readable snapshot, from an unpacked package (the verbs every co-op Setup takes; `MDK2-Coop-Setup.exe` passes
them to `mdk2-coop.ps1` without a window, waits, and returns its exit code; `install-linux.sh` takes the same words):

```powershell
MDK2-Coop-Setup.exe status --json ['<game dir>']      # every install found, or that one
MDK2-Coop-Setup.exe install '<game dir>' --yes
MDK2-Coop-Setup.exe uninstall '<game dir>' --yes
powershell -NoProfile -File .\mdk2-coop.ps1 -GameDir '<game dir>' -StatusOnly -Yes   # the old form, still accepted
```

Exit codes: 0 done, 1 failed (also a damaged single-file Setup), 2 usage, 3 unsupported game build, 4 the game is running
from that folder. The verbs never ask questions. Setup.exe and its PowerShell are 32-bit (like the game): the running-game
check reads only 32-bit processes' paths. `tools/test_setup_verbs.ps1` (run by `tools/qa_installer.sh`) covers the verbs
and codes on the zip's and the single-file Setup, with mutants.

### Who installed it

`Get-CoopOwner` (install-state.ps1) answers `setup` (a `.coop-receipt.json` beside a payload file), `mod-manager` (no
receipt, and the files match the hashes in a `coop-mod.json`: the package's own or one in the game folder), `vortex` (the
same, with `dinput8.dll` hard-linked: `fsutil hardlink list`), or `none`. For a mod manager's install Setup shows
"Installed by a mod manager" with Play enabled and no Install/Uninstall, and `install` refuses (exit 1): the manager updates
and removes its files. `install-linux.sh status --json` reports the same states (`installed`, `mod-manager`, `vortex`).

### Another mod's dinput8.dll: the chain

A `dinput8.dll` that is not this mod's (no `mdk2-coop bridge attached` string) is kept as `dinput8.chain.dll` instead of
`dinput8.dll.coop-original`. The receipt format is unchanged: its `original` hash says which file is the original, and
`Get-CoopKeptPath` finds it under either name. Install and Update move a non-mod `dinput8.dll.coop-original` that an older
Setup kept to `dinput8.chain.dll` (verified). Uninstall puts the original back as `dinput8.dll`. An older copy of this mod is
never chained (it would run the mod twice).

The proxy (`bridge/dinput8.c` `chain_load`) loads `<game>\dinput8.chain.dll` at the end of its own start-up (still inside
the loader's attach, which is when ASI loaders and fixes expect to run) and logs what it chained. If the chained DLL exports
`DirectInput8Create` it is a full dinput8 proxy, and every dinput8 export goes through it (it forwards to the system DLL
itself); a call that comes back into this DLL from inside it goes to the system `dinput8.dll`, and the interface is wrapped
once. Otherwise it is a plug-in loaded for its side effects and the exports go to the system `dinput8.dll` as before. A file
containing this mod's marker string is never loaded. `tools/test_chain_load.py` runs the real DLL in a host exe with fixture
chain DLLs (plug-in, full proxy, a copy of the mod, none) and a `-DCOOP_MUTANT_NO_CHAIN` build that must fail.

### Nothing written into the game folder at run time

`choose_coop_dir` (dinput8.c): the bridge keeps its data (coop.ini, logs, diagnostics, `unsupported-build.txt`, profiles) in
`%LOCALAPPDATA%\mdk2-coop\coop`. The only exception is a test or couch copy: a `<game>\coop\coop.ini` with a `[test]` or
`[couch]` section, placed by the test tools or Couch.cmd (the mod never creates it), keeps that copy's data in
`<game>\coop` as before. Top-level files an older version wrote into `<game>\coop` move to the profile once (copied,
verified, then deleted; never overwriting; subfolders such as old logs stay). The bridge no longer writes a `write.test`
probe into the game folder.

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
- Keeps its own files outside the game folder, in `<user data>\mdk2-coop\update` (`%LOCALAPPDATA%`, or the `[test]
  save_dir` folder of a test copy): `update-state.ini` and the verified backups `dinput8.dll.previous` and
  `coop.lua.previous`.
- Replaces running files atomically (`lu_apply_package`): copies the current files to those backups, renames the running
  `dinput8.dll` aside to `dinput8.dll.update-old` (a mapped DLL can be renamed, not deleted), moves the verified new files
  into place, updates `update-state.ini`, and synchronizes installer receipts and `install-linux.sh`'s record.
- Replaces only files Setup installed (`lu_install_owner`: a receipt, or install-linux.sh's record). Otherwise it still
  checks, says "Update it in your mod manager" (Vortex wording when the files are hard-linked) and offers the release page
  instead of Install (`LU_ERR_NOT_OWNED`; `update_owner` in `launcher-state.ini`).
- At every start (`lu_startup`): deletes the `update-old` DLL of the last update, and moves the `.previous` files and
  `update-state.ini` an older updater (v0.3.5 and before) wrote into the game folder to the data folder (verified).
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

The updater of every installed version (`bridge/launcher_update.h`; v0.3.5-alpha is public) needs exactly these things: the
asset `mdk2-coop-<tag>.zip` with its `.zip.sha256`, and the members `mdk2-coop-<tag>/dinput8.dll` and
`mdk2-coop-<tag>/coop/coop.lua`. The Setup.exe assets are ignored by its asset matching (it matches names exactly), and so
are the two `coop-mod.json` copies (it extracts only the members it names; every name in the zip must still be a safe path).
**Keep it that way**: do not rename the zip or its top folder, and keep both files in it. `tools/test_update_layout.py`
checks this against a real `release.sh` build with both the current updater and the v0.3.5-alpha one (read from the git
tag): release JSON with all four assets, apply, rollback; mutants for a wrong root folder and a missing sidecar. The
payload zip stays far below the updater's 32 MB limit (the Setup.exe is never downloaded by it).

### coop-mod.json, VERSIONINFO and the Nexus note

`tools/mod_manifest.py` writes `coop-mod.json` (co-op mod standard J5: schema, id, name, version, game with its builds,
proxy with `chain_file`, Proton DLL overrides, the game-folder files with SHA-256 and size, user data, homepage, update
sources) into the zip's top folder (and the single-file Setup's payload) and at the zip root. Setup reads its file list
from the package's copy (`Get-CoopPackageFiles`, with the old two-file list as the fallback). The in-game updater keeps its
compiled-in member names, which the old updater needs anyway. `tools/check_release_package.py` checks both copies against
the zip (`mod_manifest.check`), and that `dinput8.dll`, `install-policy.exe` and both Setup exes carry a VERSIONINFO whose
ProductVersion is the release version (`tools/versioninfo.py`); `tools/test_mod_manifest.py` has four manifest mutants.
The Steam build's exe hash in `BUILDS` is not known yet (null); build detection does not use it.
`release.sh` also writes `local/release/NEXUS-<version>.txt`: the copy note for Nexus Mods (the same zip, its SHA-256 and
the same version string). Nothing is uploaded.

Windows-side checks for the Setup exe (`tools/test_setup_exe.py`: extraction, damaged payload, junctions, zip message,
mutants) and for the plain build messages (`tools/test_installer_builds.ps1`, `tools/qa_build_message.sh`) run in
`tools/qa_installer.sh` and `tools/qa_build_message.sh`; `tools/test_setup_core.py`, `tools/test_install_linux.sh`,
`tools/test_release_signing.sh` and `tools/test_update_layout.py` are game-free offline gates.

`tools/test_launcher_update_win.py` loads an old and a new DLL in one process and unloads the old one. Build both packages
from the same tree (as the commands above do): an old DLL from a different source version leaks its exception handler,
which then points into the new DLL's code. The current DLL removes its handler when it is unloaded.

## Code signing

The release route is SignPath Foundation (application in progress). Free code signing provided by
[SignPath.io](https://about.signpath.io/), certificate by [SignPath Foundation](https://signpath.org/).
The [code signing policy](../README.md#code-signing-policy) identifies the author, reviewer and release approver.
Signing does not guarantee that SmartScreen or antivirus will accept a new release immediately.

### One-time maintainer setup

1. Create the SignPath account with **MFA**, complete the Foundation application, and connect this public GitHub
   repository as a [trusted build source](https://docs.signpath.io/trusted-build-systems/github).
2. Create project `mdk2-coop` and signing policy `release-signing`. Require **manual approval** by the maintainer
   for every request; do not enable automatic approval. Restrict the policy to this repository's release workflow
   on GitHub-hosted runners and reviewed release tags. Give the API token submitter access only.
3. Configure the default artifact configuration to accept the ZIP produced by `actions/upload-artifact` with PE
   files at its root. It must sign exactly the matching `dinput8.dll`, `install-policy.exe` and `MDK2-Coop-Setup.exe`
   when present, allow the one-Setup stage B artifact, and preserve their names. Enforce ProductName `MDK2 Co-op`
   and the release ProductVersion. Stage A has three binaries; stage B has only the single-file Setup.
4. Store the submitter token as repository **secret** `SIGNPATH_API_TOKEN`, and the organization ID as repository
   **variable** `SIGNPATH_ORGANIZATION_ID`. Optional variables `SIGNPATH_PROJECT_SLUG` and
   `SIGNPATH_SIGNING_POLICY_SLUG` override the defaults above. No certificate or private key belongs in GitHub secrets
   or this repository; SignPath holds the signing certificate.

The action's exact inputs and artifact download behavior are described in its
[pinned action definition](https://github.com/SignPath/github-action-submit-signing-request/blob/c92b958760219087e01f8d67a1669ed57afe2627/action.yml).
The initial project/artifact configuration must be checked in the SignPath account before the first signed release.

### Each release

After the normal regression/review gates, push the approved `v*` release tag, or dispatch
[release.yml](../.github/workflows/release.yml) with an **existing tag** in `version`. The workflow validates that
HEAD equals that tag; arbitrary versions are refused. Tagging, signing approval and publishing remain maintainer actions.

1. GitHub Actions builds from the public tag using the same `tools/release.sh` as local packaging. It runs the portable
   `qa_all.py offline` gates. Windows/PowerShell-only gates and Windows-only subtests are named in the job summary;
   this CI run does not replace Windows or live-game release checks. Gate logs are uploaded even on failure.
2. It always builds, checks and uploads `unsigned-<version>` downloads. With either signing credential unset, it
   ends there, with an explicit skip summary and **no GitHub release**.
3. With signing configured, `release.sh <version> prepare` builds the three package binaries. Stage A uploads them
   as one GitHub artifact, then the pinned SignPath action submits it and **waits for manual approval** (up to two hours).
   Review the source tag and request, then approve it in SignPath.
4. The signed files are downloaded, cryptographically verified with `osslsigncode`, and used by
   `RELEASE_REQUIRE_SIGNED=1 release.sh <version> assemble`. The manifest and payload hash cover signed bytes;
   the single-file Setup is built from that payload without rebuilding the three signed binaries.
5. Stage B uploads only that outer Setup. Review and approve the second request in SignPath. After download and
   signature verification, `RELEASE_REQUIRE_SIGNED=1 release.sh <version> finalize` verifies the assembly inputs,
   writes the zip and checksum sidecars and runs `check_release_package.py --require-signed`.
6. Only a successful signed run creates a **draft GitHub release** containing the zip, single-file Setup and both
   `.sha256` files. Review it and publish manually. A run never publishes an existing draft, and refuses to overwrite
   an existing release. Download signed assets from `signed-<version>` if a draft must be prepared separately.

`check_release_package.py` checks ProductName and a common ProductVersion for every shipped DLL/exe, signed payload
copies equal to the zip, inventory, hashes and certificate-table presence. This structural signature check is **not**
cryptographic validation: CI also runs `osslsigncode verify` on every SignPath output before using it.
PowerShell scripts are plain text; archives and checksum sidecars are not Authenticode-signed.

### Reproduction and local signing fixtures

The workflow pins Ubuntu 24.04 (instead of the moving `ubuntu-latest` label), Python 3.12.8, the cross GCC packages
13.2.0-6ubuntu1+26.1, MinGW headers 11.0.1-3build1, and cross binutils 2.41.90.20240122-1ubuntu1+11.4 from Ubuntu's
2024-05-01 snapshot. Action revisions are commit-pinned. It forces the Win32 thread-model compiler alternatives.
`SOURCE_DATE_EPOCH` is the source commit time; PE linker timestamps are zero, ZIP timestamps use that epoch in UTC,
entries are sorted, and archive modes are normalized (the Linux installer remains executable).

For a local Linux reproduction with those prerequisites installed, use a fresh clone with tags:

```sh
git checkout <release-tag>
export SOURCE_DATE_EPOCH=$(git log -1 --format=%ct)
python3 .github/scripts/linux-gates.py
RELEASE_OUT=local/release-unsigned bash tools/release.sh <release-tag>
python3 tools/check_release_package.py local/release-unsigned/mdk2-coop-<release-tag>.zip
```

Unsigned downloads should repeat byte for byte with the same source, version, toolchain and compression libraries.
The GitHub runner image itself, native compilers used only by test fixtures, and dependency/security updates are not
bit-for-bit pinned. Python's zlib version may affect archive compression. Different toolchains are not expected to
produce identical binaries. Authenticode signatures, signing timestamps and therefore signed archives/checksums are
**not reproducible**. GitHub's artifact transport ZIP also has its own metadata; compare the downloads inside it.

Local `SIGN_CMD` / `SIGN_VERIFY_CMD` templates remain available for development fixtures (each replaces `{file}` with a
quoted path). Foundation release signing uses only the CI artifacts above. Unset `SIGN_CMD` builds unsigned with a warning.
`tools/test_release_signing.sh` uses a fake local SignPath step, never contacts the service, and proves the two approval
boundaries, signed-before-hashing order and unsigned reproducibility. Late signing, skipped stages and replacement of a
stage A file after assembly must fail. The fake certificates are deliberately not cryptographic signatures.

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

# 7b. One-file Setup and plain-build-message cores, Linux install script, signing hook, updater layout, updater ownership
#     and game-folder check, coop-mod.json, chain-load (all with mutants; the chain-load test runs a Windows host exe)
python3 tools/test_setup_core.py
bash tools/test_install_linux.sh
bash tools/test_release_signing.sh
python3 tools/test_update_layout.py
python3 tools/test_update_ownership.py
python3 tools/test_mod_manifest.py
python3 tools/test_chain_load.py

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
