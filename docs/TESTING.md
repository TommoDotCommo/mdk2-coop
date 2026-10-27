# Automated regression

Run from the repository root with the configured test harness:

```sh
python3 tools/qa_all.py offline          # build and game-free tests
python3 tools/qa_all.py smoke            # offline + two-player QA + 1-1..1-3 + one fixture per level
python3 tools/qa_all.py features         # offline + every feature suite + lossy projectiles
python3 tools/qa_all.py campaign         # offline + all 96 checkpoint fixtures
python3 tools/qa_all.py campaign --campaign-shards 4  # four two-game sets; eight copies
python3 tools/qa_all.py full             # offline + features + loss + campaign
python3 tools/qa_all.py full --lanes 3 --campaign-shards 4  # isolated lanes, campaign alone
python3 tools/qa_all.py full --list      # inspect commands, player counts and time limits
python3 tools/qa_all.py features --gate revive --gate save-reconnect
python3 tools/qa_all.py smoke --route local/routes/room2.route
```

The default is `smoke`. If Windows forces system DLLs ahead of the proxy, prefix
the command with `MDK2_PROCESS_SERVICE=1` (see [DEVELOPMENT.md](DEVELOPMENT.md)). Game-free means no MDK2
instance or game files; native tests still require Windows interop and mingw-w64.
Python-only oracle tests:
`python3 -B -m unittest discover -s tests -p '*_test.py'`.

The runner uses explicitly configured disposable copies or an optional
resource allocator (see below). Gates run in background mode. `--lanes N` runs up
to N feature suites concurrently; each lane gets its own copies, ports, helper
folder and PID file. Offline gates run sequentially first; an offline failure
stops the ladder. Launcher and installer gates run alone because they edit shared
settings. Character-motion runs alone because its per-frame probes require consistent frame
timing. The campaign also runs alone and uses its own `--campaign-shards` setting.
The default is one lane. Do not edit or rebuild the mod during a regression run.

Each lane reserves enough copies for its largest suite and releases them before
the next phase. A busy allocator returns 3 immediately and any partial reservations
are released. Every game gate cleans up all of its lane's copies. Final cleanup
releases resources on success, failure, timeout or Ctrl-C. Use separate worktrees and an allocator
for concurrent runners.

## Configuring test resources

For one lane on a dedicated machine, export `MDK2_DIR1` through the number of copies
needed, `MDK2_PORT`, `MDK2_TOOLS` and `MDK2_PIDS`. Installer checks additionally need
`MDK2_RUN`, a harness-visible directory on a Windows filesystem. Prepare all copies first;
the runner never reads a default game install. See
[test configuration](DEVELOPMENT.md#test-configuration).

Parallel lanes require an executable allocator selected by `--resources <path>`,
`MDK2_QA_RESOURCES`, or the default ignored `local/qa_resources.py`. The allocation interface is:

- `allocator acquire <copy-count> <ttl-seconds>` reserves and provisions copies.
  On success, print one JSON object mapping environment names to string values.
  Return 3 immediately when busy; other nonzero exits mean failure. A failed
  acquisition must roll back its own partial allocation.
- `allocator release` stops the reserved games and releases the copies. The runner
  calls it even if successful acquisition returned malformed JSON.
- Both calls receive `MDK2_QA_OWNER`, a unique identity per run/lane. Never adopt
  another owner's reservation. Diagnostics go to stderr, not JSON stdout.
- Return `MDK2_DIR1..N` (Windows paths), `MDK2_PORT`, `MDK2_TOOLS` (Windows path),
  `MDK2_PIDS` (harness-visible path) and `MDK2_RUN` (on a Windows filesystem). Return
  `MDK2_PROCESS_SERVICE=1` when needed. Only `MDK2_` variables are imported.
- For noncontiguous resources, also return `MDK2_PORTS`: a comma-separated reserved
  base port for each copy. A campaign shard uses the first copy's port and consecutive
  ports for its clients; reserve that range. Without this list, shard base ports
  advance by 16. Distinct lanes must never share game paths, ports, tools or PID files.

Acquisition has a three-minute deadline and release a one-minute deadline. The
allocator must clean up on termination and use expiring reservations so a killed
runner cannot hold resources indefinitely. Allocator implementation and setup are supplied by the test environment.

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
| Multiplayer | Two and three players, relays, joining/leaving, two players standing on the same spot | Logs and cross-process world snapshots; the host is not pushed and sees its partner right beside it (mutants keeping the partner 0.5 or 3 units away fail) |
| Campaign fixtures | 96 debug-menu checkpoints across characters/levels; walk, fire, strafe, client hits | Load following, survival, puppet/enemy counts, HP and world comparisons |
| Enemy targeting | A live Doganboy is put beside one client only, then beside the other (3 games) | The host's AI bites that client (`damage coop_dog1 -> coop_p<n>`), its game applies HURT events, its hp falls; others, and the faraway client in the control phase, stay unhurt; mutant without HURT sends fails |
| Death/revive | Native state-machine tests plus real clients: down, damage suppression, revive hold, bleed-out, spectating, lava, team wipe | 69 game-free assertions, ten broken source twins, life/HP/animation reads and HUD text |
| Cutscenes | The lounge scene and skydive of a new game with two players, then a host-triggered movie | Shared go, skip vote needing a majority and lasting the whole movie, the cutscene box (hidden while watching, shown on a key press and while a vote is open: time left from the measured table or a learned length, skip key, own vote, count), skydive ghost facing like your own Kurt, the other player's homing missile shown nose first, minigame not skippable, level change waiting for the slower player with no time limit (held 25 s), Esc on a client counting as a vote instead of ending its movie alone, trigger movie played by the client; mutants without the barrier, with a one-player vote, with an upright skydive ghost, with a sideways missile and with Esc skipping alone fail; an early host GO received before movie creation must initialise the playback clock (the earlygo mutant must fail that assertion) |
| Minigames | The torpedo runs of levels 2, 5 and 9 with two players, each won through the game's own end-of-course hook | Both games reach the minigame and each logs the other's ghost ship; both survive the end of the minigame (the client crashed at level 2 on a movie-camera emitter with a negative ring index); mutant with the bridge's emitter guard off fails; screenshots as evidence |
| Level 8 intro | The intro movie with two players | Both see it to its end and the client survives the movie deleting the host's enemies by name; mutant without coop.lua's gone-gob check crashes the client |
| Level epochs | Same-checkpoint reset after a spawn/pickup, then a same-epoch network outage | Actual loads, restored items/actors, world comparisons, retained world on reconnect; native spawn deferral and epoch-wrap assertions |
| Save/reconnect | Client/host crash, resume, network cut, subsequent level change | Saved identity, HP/position and world state |
| Menus/settings | Options (Gameplay, Interface, Voice chat, Controls), bindings, presets, conflicts, persistence, launcher (its Settings sections: an Interface switch and a sound level), the emote wheel switched off and on | In-game reads, settings files (coop.ini, settings.lua), the wheel playing nothing while off (mutant ignoring the switches fails) and screenshot evidence |
| Lobby | The launcher's lobby (`tools/qa_lobby.sh`): the host opens it, two clients join over UDP and are listed, Ready shows on the host, Start counts down on every launcher, then all three games start and connect | Launcher logs and screenshots; `MUTANT=nostart` (the host never starts) must fail the start checks |
| First enemy | Level 1's sign, glass and Doganboy played for real (`tools/qa_sign.sh`, `SNIPER=<n>` picks who fires): a native sniper round on the sign, the glass in every game, the dog targeting and hurting each player, every player's hits lowering the one host hp, the kill running OnDie on the clients and `dr1_02` opening for everyone | Logs and Lua reads in all three games; a client that never ran the dog's OnDie leaves the door shut and fails |
| HUD/chat/voice | Players, messages/pings, visibility, range/relay | In-game state, rendered pixels, voice frame/gain counters |
| Diagnostics | `tools/test_diag.sh` (game-free, needs Windows PowerShell): the log reader, and `dist/diag.ps1` on a folder whose newest log is held open for writing; then `tools/qa_diag.sh` with two games: link quality lines, no desync in a quiet game, an F2 mark filed on both machines, a host reporting the wrong enemy count (client names the field, host told, both dump, clears again), intact hash chains, two bundles read together; the launcher's Help tab clicked for real in `tools/qa_launcher.sh` (the zip lands where `[diag] out_dir` says and the report tool reads it) | Names, addresses, Steam ids and e-mail addresses planted in logs are absent from the zip; zip entries use `/`; hashes and chains check out; a log changed after bundling, a log with a line missing and an edited log are caught; a script that cannot read a log the game holds open fails; `MUTANT=deaf tools/qa_diag.sh` (run by hand) fails the desync checks |
| Crash evidence | One game crashed on purpose (`coop_crash()`, background mode only), then started again | The crash line (exception, module + offset, the address written) in bridge.log, and that log kept as bridge.previous.log after the restart; builds without the crash logger or without keeping the log fail. Also: a session log per run, the crashed one marked `.crash` and without a clean-exit line, its flight dump holding samples and the log tail, and every hash chain intact |
| Emotes and heroes | Emote wheel per hero and its "All..." list; switching hero, also from Doc against a wall | The animation each slot plays (Max and Doc get ones they have), the list's count (Kurt 61), the hero moving after the switch (mutant without unsticking fails) |
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

The cutscene suite also has a focused event-order regression, using two configured
copies: `tools/qa_cutscene.sh --early-go`. It delivers GO through the real movie
event handler before creating the client's movie, then requires the playback clock
to advance and the movie to finish without Lua errors.
`MUTANT=earlygo tools/qa_cutscene.sh --early-go` removes clock initialisation and
succeeds only when the clock assertion catches it. The full cutscene gate includes
the same checks.

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
set and captures every set's game logs. Ports follow MDK2_PORTS when provided.
The campaign runs in its own phase, so its copies are reserved only for that phase.

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
