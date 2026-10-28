# MDK2 HD Co-op v0.3.2-alpha

This alpha includes the merged gameplay, launcher and installer work, plus fixes
from paired multiplayer playtesting.

## Gameplay fixes

- Enemies can acquire, pursue and damage client players. Native ranged attacks
  now aim at the selected player, including corridor turrets and round enemies.
  The first behind-glass enemy's scripted attack follows the same targeting rules.
- The first visible enemy generator now mirrors its animation and lifetime on
  clients. Stable enemy identities prevent repeated spawn messages or changes in
  a group of same-named enemies from producing stray or apparently unkillable mirrors.
- Fixed a crash when a projectile hits after its original shooter has disappeared.
- Included mortar progression synchronization, shared pickup rewards and enemy
  maximum-health display corrections.
- Remote Kurt parachutes mirror deployment, steering, landing and cut-away.
- Client joining handles waiting, host departures, cancellation, retry and
  incompatible versions more clearly. Native actor callback fixes are included.

## Launcher, installer and lobby

- Fixed Leave lobby immediately rejoining from the same click.
- Save address adds the current lobby address and port to Saved addresses without
  leaving the lobby. Existing names and notes are preserved when already saved.
- Joining a saved address opens the connected lobby page directly.
- Saved addresses include live UDP status checks. Lobby connections and pregame
  text and voice chat remain active while browsing launcher pages.
- The graphical installer supports Install, Update and Repair, with ownership
  receipts and clearer installation status.
- Automatic updates include alpha releases by default, ignore drafts, compare
  release versions and verify package checksums. Failed installations can roll back.

## Combat feedback

- Host-confirmed hitmarkers and floating damage numbers remain enabled.
- The combat log and DPS panels are hidden for now while their presentation is
  revised. Bounded event history, counters and diagnostic dumps remain available.

## Validation

- All 16 offline regression gates and all 66 Python unit tests passed.
- Eight native level-one encounter checks passed: generator animation, real
  ranged client damage with the host unharmed, repeated spawn announcements,
  sibling removal, matching survivor health, survivor death on both peers,
  client-targeted turret launches, and the first enemy after sniping its sign.
  Disabling targeting made the native ranged-damage check fail as required.
- The existing three-check Grunt targeting, direct-damage and script-error
  restoration regression also passed.
- Focused lobby checks passed for leaving/rejoining, duplicate-free address
  saving and joining a saved address directly into the connected lobby.
- Earlier targeted canopy and join-flow checks passed; the complete campaign
  and feature ladder have not been rerun on this final combined build.

## Known limitations

- These AI results cover the tested encounters, not every enemy, level or
  navigation situation in the campaign.
- Voice chat was tested with synthetic audio, not a physical microphone.
- Hitmarkers and floating damage numbers have automated core coverage; a full
  multiplayer rendering matrix has not been completed.
- The Overhaul Games build is accepted by build detection but remains untested.
- This is an alpha: expect rough edges. Use the diagnostics bundle when reporting
  a problem, with the level, checkpoint and whether you hosted or joined.

## Maintainer publishing

The package is prepared locally. From this release branch, after reviewing the
changes, publish its commit and create the GitHub pre-release:

```bash
git push origin HEAD:main
gh release create v0.3.2-alpha \
  local/release/mdk2-coop-v0.3.2-alpha.zip \
  local/release/mdk2-coop-v0.3.2-alpha.zip.sha256 \
  --repo TommoDotCommo/mdk2-coop \
  --target "$(git rev-parse HEAD)" \
  --title "mdk2-coop v0.3.2-alpha" \
  --notes-file docs/RELEASE-NOTES-v0.3.2-alpha.md \
  --prerelease
```
