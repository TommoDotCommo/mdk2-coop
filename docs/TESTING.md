# Automated regression

Run from the repository root on the WSL/Windows lab machine:

```sh
python3 tools/qa_all.py offline          # build and game-free tests
python3 tools/qa_all.py smoke            # offline + two-player QA + 1-1..1-3 + one fixture per level
python3 tools/qa_all.py features         # offline + every feature suite + lossy projectiles
python3 tools/qa_all.py campaign         # offline + all 96 checkpoint fixtures
python3 tools/qa_all.py campaign --campaign-shards 4  # four two-game sets; eight slots
python3 tools/qa_all.py full             # offline + features + loss + campaign
python3 tools/qa_all.py full --list      # inspect commands, player counts and time limits
python3 tools/qa_all.py features --gate revive --gate save-reconnect
python3 tools/qa_all.py smoke --route local/routes/room2.route
```

The default is `smoke`. If Windows forces system DLLs ahead of the proxy, prefix
the command with `MDK2_PROCESS_SERVICE=1` (see README). Game-free means no MDK2
instance or game files; native tests still require Windows interop and mingw-w64.
Python oracle tests alone run on Linux:
`python3 -B -m unittest discover -s tests -p '*_test.py'`.

The runner acquires the maximum required lab slots, provisions isolated copies,
imports their paths and ports, and runs gates sequentially in background mode.
It refuses to adopt an existing lease for this worktree. A busy lab returns 3
immediately; run offline checks and retry later. Every game gate is followed by
cleanup of all leased copies. Final cleanup releases the lease on success,
failure, timeout or Ctrl-C. Do not run two runners in the same worktree.

Each gate has a deadline: 10 minutes for features, 90 minutes for the campaign.
A timeout stops the harness process group and cleans up its games. Game gates
must return zero **and** print a nonempty successful terminal summary. No summary,
zero checks, explicit failure, missing sweep rows and timeouts cannot pass.
Feature failures do not prevent later feature gates from running. An offline
failure stops the ladder because later game results would not be trustworthy.

Evidence lives under `local/regression/<timestamp>/`: each gate's stdout/stderr,
cleanup log, `results.json` and `results.md`. The runner also copies each game's bridge log, configuration, route status and available crash dumps before the next
gate can overwrite them. Original suites retain screenshots and snapshots in the
folders printed in each gate's log.
The runner returns 0 only when every selected gate passes. `--gate` selects from
that profile without implicitly running other gates. Inherited simulation,
sharding, autostart and skip-build settings are cleared. The lossy projectile and epoch gates explicitly
apply 80 ms latency, 30 ms jitter and 3% loss; this is one simulated condition.

## Coverage and limits

| Area | Automated exercise | Oracle |
|---|---|---|
| Native build | All bridge translation units | Compiler/linker result |
| Network | Fake Steam, loopback UDP, authentication, reliable events, loss/latency, reconnect | Actual packet code assertions; welcome rules and acknowledgements |
| Input/routes | DirectInput immediate/buffered/peek reads, parser and playback | Actual wrappers/parser; wrong start, missed progression, drift and endpoint mutants |
| Controllers | Axis/buttons and control mapping | Native mapping assertions |
| Projectiles | Fake engine and real native launches, flight, client relay, simulated loss | Launch identity/transforms, movement, replay damage suppression, duplicate counts |
| Multiplayer | Two and three players, relays, joining/leaving | Logs and cross-process world snapshots |
| Campaign fixtures | 96 debug-menu checkpoints across characters/levels; walk, fire, strafe, client hits | Load following, survival, puppet/enemy counts, HP and world comparisons |
| Enemy targeting | A live Doganboy is put beside one client only, then beside the other (3 games) | The host's AI bites that client (`damage coop_dog1 -> coop_p<n>`), its game applies HURT events, its hp falls; others, and the faraway client in the control phase, stay unhurt; mutant without HURT sends fails |
| Death/revive | Native state-machine tests plus real clients: down, damage suppression, revive hold, bleed-out, spectating, lava, team wipe | 69 game-free assertions, ten broken source twins, life/HP/animation reads and HUD text |
| Cutscenes | The lounge scene and skydive of a new game with two players, then a host-triggered movie | Shared go, skip vote needing a majority and lasting the whole movie, the cutscene box (hidden while watching, shown on a key press and while a vote is open: time left from the measured table or a learned length, skip key, own vote, count), skydive ghost facing like your own Kurt, the other player's homing missile shown nose first, minigame not skippable, level change waiting for the slower player, trigger movie played by the client; mutants without the barrier, with a one-player vote, with an upright skydive ghost and with a sideways missile fail |
| Minigames | The torpedo runs of levels 2, 5 and 9 with two players, each won through the game's own end-of-course hook | Both games reach the minigame and each logs the other's ghost ship; both survive the end of the minigame (the client crashed at level 2 on a movie-camera emitter with a negative ring index); mutant with the bridge's emitter guard off fails; screenshots as evidence |
| Level 8 intro | The intro movie with two players | Both see it to its end and the client survives the movie deleting the host's enemies by name; mutant without coop.lua's gone-gob check crashes the client |
| Level epochs | Same-checkpoint reset after a spawn/pickup, then a same-epoch network outage | Actual loads, restored items/actors, world comparisons, retained world on reconnect; native spawn deferral and epoch-wrap assertions |
| Save/reconnect | Client/host crash, resume, network cut, subsequent level change | Saved identity, HP/position and world state |
| Menus/settings | Options, bindings, presets, conflicts, persistence, launcher | In-game reads, settings files and screenshot evidence |
| HUD/chat/voice | Players, messages/pings, visibility, range/relay | In-game state, rendered pixels, voice frame/gain counters |
| Installer | Fresh install, Steam/UDP configuration, upgrades, invalid inputs, uninstall and backups in disposable fixtures | Installed files and settings; source executable is only read |
| Recorded progression | Optional `--route` files replay ordinary controls with clients | Checkpoint order, endpoint, participants/life and world snapshots |

Checkpoint fixtures load directly through the developer path. They establish
checkpoint compatibility and short actions, **not** ordinary traversal, puzzle
completion or boss victory. No validated campaign route is bundled yet. Add
recordings only after real replay validation using
[routes/README.md](../routes/README.md). Real controllers, microphone hardware,
independent Steam accounts and long internet sessions remain separate validation.
The installer gate reserves a copy to read its executable but launches no game.
Default features use at most three players; historical eight-player results are
not revalidated by this profile.

## Failures must be detectable

`tools/qa_ai.sh` (gate `enemy-targeting`) checks that enemy AI attacks players who are not the host.
Teleporting the dog sometimes drops it through the floor, so the script confirms it stands beside the
target (retrying up to three times) before it asserts anything; baselines for the target are taken
before the move because the first bites land during that check.

`tests/qa_oracles_test.py` mutates evidence and runs the production scorers:
wrong level/role/slot, enemy type/HP/deletion/extra enemy, missing item/puppet,
large drift, non-finite/truncated coordinates, missing headers, invalid tolerance,
missing/duplicate checkpoints, absent clients, world errors, crashes, Lua errors,
missing summaries, zero checks and timeouts.

`tools/sweep.sh --levels` visits the first checkpoint of every level, using F8
for level changes. This gives the smoke profile coverage of all 12 levels and all
characters without running the full checkpoint list.

The ordered checkpoint list is `tools/checkpoints.txt`, including nonnumeric
progression order. Unknown/reversed endpoints fail before launching games.
Individual and sharded sweeps return nonzero for failed/missing checkpoints;
all shard exit statuses are checked. `--campaign-shards N` reserves two slots per
set and captures every set's game logs. Ports follow the actual lab slot numbers.
Use the campaign profile for a parallel sweep on its own; adding sharding to the
full profile reserves those slots throughout the feature gates too.

World comparisons preserve timing warnings: small differences, swinging doors,
unbound actors. Persistent large differences fail after the existing 1.5-second
retry. Missing living players, malformed dumps and invalid coordinates fail.
Orphan-deletion logs count successful removal of client-local spawns; they are
evidence, not surviving extra enemies. Matching samples cannot rule out every
transient fault.

The offline profile includes `make -C bridge test-revive` (also
`tools/test_revive.sh`). It compiles the production `revive.c` against a fake clock,
world, input and event sink. The baseline must pass 69 state assertions, and ten
broken source variants must fail those assertions. It covers pause clamping,
bleed-out stopping during revive, revive range and hold/release behavior,
checkpoint authority, health restoration, spectating/cycling, zone-clear delays,
one-shot team wipe and solo fallback without launching MDK2.

`tools/test_network.sh` also requires four broken epoch variants to fail: premature
epoch adoption, applying spawns during a load, blocking a same-epoch reconnect,
and ignoring epoch zero at wraparound. The real-game epoch gate changes the world
with a spawn and pickup before a same-checkpoint host reset. It requires both
worlds to reload and restore the fixture, then verifies that a same-epoch outage
keeps that restored world. The feature profile repeats this under simulated loss.
