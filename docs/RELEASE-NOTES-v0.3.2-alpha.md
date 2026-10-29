# MDK2 HD Co-op v0.3.2-alpha

## What's New

### Direct AI Targeting & Remote Kurt Canopy
- Enemies now target, pursue, and damage client players directly (`bridge/presence.c`, `bridge/presence_game.c`, `bridge/net.c`).
- Client join flow rewritten with explicit version mismatch detection, cancellation handling, and clean reconnection retry.
- Remote Kurt canopy state captured, encoded, decoded, and animated seamlessly on puppets across players.
- Engine ABI stack balance fixes for actor callbacks.

### Launcher, Installer & Lobby Enhancements
- Single graphical installer (`MDK2-Coop-Setup.exe`) with transaction-verified ownership receipts (`dinput8.dll.coop-receipt.json`), install policy engine (`install-policy.exe`), and status UI.
- Saved-IP server profiles with live UDP status probes (`bridge/launcher_profiles.h`, `bridge/launcher_profiles_win.h`).
- Persistent lobby sessions with pregame text chat and voice chat support (`bridge/launcher_session.h`, `bridge/launcher_voice_win.h`).
- Automatic update discovery from GitHub releases: queries GitHub releases list endpoint (`/releases?per_page=10`), parses release arrays, ignores drafts, orders pre-releases and final releases by semver (`v0.3.2-alpha` above `v0.3.1-alpha`, final `v0.3.2` above `v0.3.2-alpha`), breaks ties by `published_at`, and adds an "include pre-releases" option (defaults to ON for alpha releases).
- Atomic package installation with SHA-256 sidecar checksum verification, PE header validation, and verified automatic rollback on failure.

### Combat Feedback & DPS Meter
- Real-time combat feed and damage log (`bridge/combat.c`, `bridge/combat_core.c`, `docs/COMBAT.md`).
- Floating hitmarkers and rising damage numbers colored per player.
- In-game DPS meter showing 5-second rolling DPS, fight DPS, and total fight damage (toggle with `Ctrl+M`, reset with `Ctrl+R`, scroll with `Ctrl+PgUp`/`Ctrl+PgDn`, dump to `combat.txt` with `Ctrl+D`).
- Host-confirmed enemy HP loss telemetry using event `EV_COMBAT` in sync core.

### Sync Core & Gameplay Fixes
- Progression synchronization for mortar weapons, shared pickup distributions, and enemy health display bars.
- Comprehensive game-free mutation suites, sync simulator, fuzzers, and lossy link test suites.

## Known Issues

### Verified live (MDK2 HD Steam build 3902, disposable game copies, background mode)

Each baseline passed and each deliberately broken variant failed as required.

- **Lobby** (`tools/qa_lobby.sh`): 33 of 33 checks with a host and two clients, including chat, voice frames, Ready, countdown and start. The `MUTANT=nostart` variant was not run.
- **Enemy targeting** (`tools/qa_ai.sh`): client-near and host-near cases pass; `MUTANT=targeting` and `MUTANT=hostaoe` fail.
- **Remote Kurt canopy, three peers** (`tools/qa_presence.sh`): 27 of 27 including client-to-client relay; `MUTANT=canopy` fails all 27.
- **Client join flow** (`tools/qa_join.sh`): 7 of 7 (waiting screen, automatic pull, host leaving, cancel, retry, 65-second outage, and a packet from a previous protocol giving the update message); `MUTANT=join` fails 5 of 7. Version mismatch is detected by protocol magic only; there is no exact mod-version string check and no separate old release was installed.
- **Combat feed ledger and DPS meter** (`tools/qa_combat.sh`, three processes, state read from the processes): a host hit of 20 and a client hit of 30 produce identical event ledgers, per-player totals and 5-second DPS on all three, matching the measured enemy HP loss; idle expiry, the Ctrl+M toggle edges and reset also pass (7 of 7). `MUTANT=deaf` fails on the ledger comparison. `tools/qa_combat_startup.sh` passes.
- **Playtest sync** (`tools/qa_playtest.sh`, 40 of 40): mortar progression through the dome callbacks, shared pickups, scaled enemy health bars on both games; `MUTANT=pickups` and `MUTANT=dome` fail.

### Not verified live

- **Rendering of hitmarkers, coloured damage numbers and the feed text** is not covered: the live check reads the ledger that feeds them (attacker, amount, target, position), not drawn pixels. Layout is checked by rectangle tests only.
- **Lossy-link combat** (`lossy-combat` gate), projectile source attribution, reconnect and host migration during combat were not run live.
- **Dome mortar puzzle** is exercised through collision and explosion callbacks, not projectile flight.
- **Voice chat hardware**: tested with synthetic audio and protocol simulations only, not a physical microphone.
- **Full regression ladder** (`qa_all.py full`) was not run for this build; only the suites above were run live.
- **`tools/qa_sign.sh`** (level 1 sign, glass and dog) passed 37 of 38 in both runs, with a different "the dog hurt player N" check missing each time, and the same happened before the fixes below. Treat the dog's bite as an intermittent test miss, not yet explained.
- **Pre-release status**: this is an alpha release candidate.

### Fixed while verifying live

- The host crashed when a hit with no attacking object (environmental damage) reached the player-presence check. Covered by `tests/presence_game_test.c` (mutant in `tools/test_presence.py`) and by `tools/qa_playtest.sh`.
- The combat and playtest suites aimed at `r2_dog01`, which is immune until its glass sequence, so no damage landed; they now use `doganboys07`, and the combat oracle also checks the DPS values.

## Maintainer Release Instructions

To create the GitHub pre-release, run:
```bash
gh release create v0.3.2-alpha local/release/mdk2-coop-v0.3.2-alpha.zip local/release/mdk2-coop-v0.3.2-alpha.zip.sha256 \
  --title "mdk2-coop v0.3.2-alpha" \
  --notes-file docs/RELEASE-NOTES-v0.3.2-alpha.md \
  --prerelease
```
