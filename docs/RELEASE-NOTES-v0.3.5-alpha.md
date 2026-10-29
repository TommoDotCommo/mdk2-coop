# MDK2 HD Co-op v0.3.5-alpha

This alpha combines the work after v0.3.3-alpha: fixes from a real internet
playtest, enemy targeting of client players, a redesigned combat display, the
lobby and join-flow work, and the installer fixes. Extract the entire ZIP, then
open `MDK2-Coop-Setup.exe`. It replaces v0.3.2-alpha and v0.3.3-alpha.

## Gameplay fixes

- **Level 2 rear gate.** The gate behind the leading player no longer locks until
  every living player has crossed, so a player who falls behind is not shut out.
  The host owns the decision, clients cannot request the lock, and a pending or
  completed lock survives co-op saves and reconnects.
- **Saves while a native attack is in progress.** An attack callback is kept
  resolvable across co-op saves and instant resume.
- Enemies can acquire, pursue and damage client players; native ranged attacks
  aim at the selected player, including corridor turrets and round enemies.
- The first enemy generator mirrors its animation and lifetime on clients, with
  stable enemy identities.
- Fixed a host crash when damage with no attacking object reached the
  player-presence check, and a crash when a projectile hits after its shooter
  has gone.
- Mortar progression, shared pickup rewards, enemy maximum-health display,
  remote Kurt parachutes and clearer client joining are included.

## Combat feedback

- Coloured floating damage numbers (rapid hits merge), a hitmarker, a kill ticker
  and a compact damage panel at the right edge. The panel starts as one line;
  `Ctrl+M` expands it to a bar per player. `Ctrl+R` resets local counters and
  `Ctrl+D` dumps `combat.txt`. See `docs/COMBAT.md`.

## Launcher, installer and lobby

- Setup supports Install, Update and Repair, opens without console windows,
  and has Play and desktop-shortcut actions; update checks show their result.
- Fixed Leave lobby immediately rejoining; saved lobby addresses can be joined
  directly and show live status.
- Automatic updates include alpha releases, compare versions and verify
  checksums; failed installations roll back.

## Diagnostics

- Crash reports now include the fault registers and the arguments of the native
  animation dispatch.

## Fixed after the last playtest

- Held enemies (the level 1 dogans on their platform) no longer fall after the
  host resumes a co-op save: the bridge stopped writing a target into every actor
  each frame. Enemies still acquire and attack client players through the
  creation hook and the think/animation scope (`tools/qa_ai.sh`, `qa_hans.sh`,
  `qa_room_ai.sh` and `qa_sign.sh` keep the client far from the host).
  `tools/qa_save.sh` now compares the host's dogans with their saved height and hp.
  **Why the old per-frame store released them is not explained**: the engine
  behaviour behind it was not identified, only that removing the store fixes it.
- `tools/qa.sh` with three game copies no longer fails 11 checks: the script
  sent the client's keys to the host (a test-harness fault, not a product bug).

## Verified live

Summary of what was run on this exact tree. Per-copy user data was redirected
(saves, crash reports and settings of each test copy stay in its own folder). A
second job was running other suites on the same PC at times, so run times and a
strict hash of the real user folders are not meaningful and were not compared.

**Overhaul Games build** (the build players are expected to use), disposable
copies, background mode, no new crash dump and no crash dialog in any run:

- Game-free gates (offline ladder, 22 of 22 including targeting geometry,
  animation guard, crash watch and redirect core).
- Live gates of `qa_all.py full`, all passed: two-player (`qa.sh`), multiplayer,
  save-reconnect (`qa_save.sh`, three players, host dogans compared with saved
  values), save-script-tasks, save-redirect, level-epochs, native-room-ai,
  attached-canopy (`qa_presence.sh`), enemy-targeting (`qa_ai.sh`),
  hans-targeting, client-join, revive, cutscenes, level8-intro, menu,
  character-puppet, crash-log, diagnostics.
- `tools/qa.sh` with three copies: 52 of 52 after the harness fix (41 of 52
  before it).
- Crash hunt: 15 consecutive runs (about 16 minutes, two and three game copies) of
  `qa_sign` (46 of 46), `qa_proximity` (13), level 2 rear gate with `SAVE_RELOAD=1`
  (18, three runs) and `LEADER=2` (15), `qa_animguard` (14), `qa_combat` (7), `qa_hud`
  (11), `qa_lobby` (33), `qa_playtest` (40), `qa_shots_native` (26), `qa_save` with
  three players (56), `qa_epoch` (20) and `qa_join` (7). Every run passed, with
  no crash dump and no crash dialog. This is less than the 20 minutes aimed for.

**Steam build (3902) only**, from the earlier run of this release: the
level 2 rear gate suites, `qa_hans`, `qa_ai` (mutants `targeting`, `hostaoe`),
`qa_presence` (mutant `canopy`), `qa_join`, `qa_combat`, `qa_playtest`
(mutants `pickups`, `dome`), `qa_save_scripts` (mutant `unresolved`) and
`qa_lobby`. They were not repeated after the dogan change on Steam.

## Not verified

- **The animation crash seen in an internet playtest is not fixed.** It was a
  crash in the Overhaul Games build at `exe+0x672ed`. It was not reproduced, so
  its cause is unknown. This release only adds diagnostics (fault registers and
  animation dispatch arguments) so the next occurrence can be analysed, and the
  save-resolver correction above, which addresses one possible way a native
  attack callback could be lost across a save; it is not shown to be the cause.
- **A model-accessor crash at `exe+0x4da3e`** (address 0 read after a level 2
  resume) was seen once while several runs overlapped. Three disk-save/resume runs at level 2 and one three-player
  save run did not reproduce it, so its cause is unknown and it is not fixed.
- **Level 1 final boss.** The playtest report that the boss ignored the remote
  player could not be reproduced on the Steam build, where he attacks the remote
  player; the regression above guards that. No speculative runtime change was made.
- Not run on the Overhaul build in this verification (they passed earlier on the
  tree before the dogan change, or only on Steam): the campaign checkpoint sweep,
  and, in the full ladder, every gate after `crash-log` (diagnostics, triggers, remote events, motion, chat, voice, projectiles, installer, launcher, lossy-link variants and the animation-guard mutants), plus `qa_hans` and `qa_rear_gate` `MUTANT=` variants. Microphone, controller and installer gates were not repeated. The ladder was stopped early to leave time for the crash hunt.
- Rendering of the damage numbers, hitmarker, ticker and panel is not asserted
  by pixels; only the ledger that feeds them is checked. Other resolutions than
  the test windows were not looked at.
- Voice chat was tested with synthetic audio only; a real microphone is unverified.
- `tools/qa_sign.sh` had intermittent misses in earlier runs; it prints how
  long each player took to be bitten.
- This is an alpha: expect rough edges. Use Help > Send diagnostics when
  reporting a problem, with the level, checkpoint and whether you hosted or joined.

## Maintainer publishing

From this release branch, after reviewing the changes:

```bash
git push origin HEAD:main
gh release create v0.3.5-alpha \
  local/release/mdk2-coop-v0.3.5-alpha.zip \
  local/release/mdk2-coop-v0.3.5-alpha.zip.sha256 \
  --repo TommoDotCommo/mdk2-coop \
  --target "$(git rev-parse HEAD)" \
  --title "mdk2-coop v0.3.5-alpha" \
  --notes-file docs/RELEASE-NOTES-v0.3.5-alpha.md \
  --prerelease
```
