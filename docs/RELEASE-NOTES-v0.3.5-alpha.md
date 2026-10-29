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

## Verified live

MDK2 HD Steam build 3902, disposable game copies, background mode, this exact
merged tree. Every baseline passed and each mutant that exists failed as required.

- Level 2 rear gate (`tools/qa_rear_gate.sh`): 15 of 15; with `SAVE_RELOAD=1`
  18 of 18; with `LEADER=2` 15 of 15; `MUTANT=earlylock` fails 6.
- Level 1 final boss targeting (`tools/qa_hans.sh`): 6 of 6, including a native
  Hans projectile travelling to the remote player; `MUTANT=script-scope` and
  `MUTANT=targeting` fail.
- Enemy targeting (`tools/qa_ai.sh`): 3 of 3; `MUTANT=targeting` and
  `MUTANT=hostaoe` fail.
- Remote canopy, three peers (`tools/qa_presence.sh`): 54 passed, 0 failed;
  `MUTANT=canopy` fails.
- Client join flow (`tools/qa_join.sh`): 7 of 7; `MUTANT=join` fails 10 checks.
- Combat ledgers (`tools/qa_combat.sh`): 7 of 7 across three processes;
  `MUTANT=deaf` fails. `tools/qa_combat_startup.sh` passes.
- Playtest sync (`tools/qa_playtest.sh`): 40 of 40; `MUTANT=pickups` fails 6
  and `MUTANT=dome` fails 5.
- Native attack tasks across an instant save (`tools/qa_save_scripts.sh`): 11 of
  11; `MUTANT=unresolved` fails 7.
- Lobby (`tools/qa_lobby.sh`): 33 of 33; `MUTANT=nostart` is detected.
- Offline: all 18 game-free gates (`tools/qa_all.py offline`) and 70 Python
  unit tests pass.

## Not verified

- **The animation crash seen in an internet playtest is not fixed.** It was a
  crash in the Overhaul Games build at `exe+0x672ed`. It was not reproduced, so
  its cause is unknown. This release only adds diagnostics (fault registers and
  animation dispatch arguments) so the next occurrence can be analysed, and the
  save-resolver correction above, which addresses one possible way a native
  attack callback could be lost across a save; it is not shown to be the cause.
- **Level 1 final boss.** The playtest report that the boss ignored the remote
  player could not be reproduced on the Steam build, where he attacks the remote
  player; the regression above guards that. Recorded playtest logs show the boss
  damaging both players, so they contain no evidence of him ignoring one, but
  they do not establish which game build produced them. No speculative runtime
  change was made.
- **Overhaul Games build:** on the integrated tree, with the targeting suites
  keeping the client at least 300 units from the host, these passed on the
  Overhaul build: `qa_ai` (`MUTANT=hostaoe` and `MUTANT=near` fail), `qa_hans`,
  `qa_room_ai`, `qa_sign` (three players), `qa_animguard`, `qa_presence`,
  `qa_join`, `qa_combat`, `qa_lobby`, `qa_proximity`. The same suites were run on
  the Steam build with two exceptions: `qa_presence` failed its native-deploy
  canopy check on two runs (cause unknown, not seen on the Overhaul build) and
  one `qa_combat` run missed its DPS check before passing on a rerun.
  `qa_sign` also failed once on the Steam build (a player 2 bite baseline race)
  before passing. The save, resume, reload and rear-gate suites were not rerun
  on the Overhaul build.
- The full regression ladder (`qa_all.py full`) and the campaign checkpoint
  sweep were not run on this build, nor were the other feature suites.
- `tools/qa_sign.sh` had intermittent misses in earlier runs. It now prints how
  long each player took to be bitten; passing runs showed 1 to 2 s for host and
  clients with the client far away.
- Rendering of the damage numbers, hitmarker, ticker and panel is not asserted
  by pixels; only the ledger that feeds them is checked live.
- Voice chat was tested with synthetic audio only.
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
