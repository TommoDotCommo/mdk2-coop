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

- **Combat feedback not live-verified**: The combat feed, DPS meter, hitmarkers, and floating damage numbers have been verified in native core tests, sync simulation, and mutation suites, but have not undergone live in-game multiplayer playtesting.
- **Voice chat hardware**: Voice chat codecs and networking have been tested with synthetic audio loopbacks and protocol simulations, but have not been tested with a physical microphone.
- **Base playtest synchronization**: Features from earlier playtest work (mortar progression, shared pickups, and enemy health displays) are included in the build but are not live-verified.
- **Pre-release status**: This is an alpha release candidate; live multiplayer verification may follow.

## Maintainer Release Instructions

To create the GitHub pre-release, run:
```bash
gh release create v0.3.2-alpha local/release/mdk2-coop-v0.3.2-alpha.zip local/release/mdk2-coop-v0.3.2-alpha.zip.sha256 \
  --title "mdk2-coop v0.3.2-alpha" \
  --notes-file docs/RELEASE-NOTES-v0.3.2-alpha.md \
  --prerelease
```
