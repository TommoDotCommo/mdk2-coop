# Player presence changes and verification

Live AI validation now covers native client targeting and direct damage on Steam
build 3902. Canopy rendering/input and join-flow checks retain the separate
validation limits below.

## Native level-one encounter regression

The isolated Grunt matrix did not cover the native ranged path or the first
scripted behind-glass enemy. A later paired playtest exposed three additional
faults:

- Enemy field `+0x88` holds a generation-checked **handle**, resolved by native
  ranged fire at `0x42e096`; the adapter and its fake-engine test had incorrectly
  treated it as a pointer. The test now resolves the stored handle and rejects
  the pointer-writing mutant.
- Some ranged tasks carry a cached host target into projectile launch. Native
  launch now receives the same eligible player as acquisition and computes its
  own trajectory and spread for that player. Explicit non-player scripted
  targets and player-fired aim are preserved. `ai_fire` state rows count targets
  read from actual launched projectile state, rather than candidate selection.
- The first enemy's authored `OnUpdate` calls `mdkDoganboyAttack` directly,
  outside `ScriptUpdate`. That direct Lua entry is now scoped and protected too.

`tools/qa_room_ai.sh` runs the authored level-one encounters with two background
clients. It does not create or alert enemies or reset the native AI target. Its
eight process-state checks cover generator animation replication, native round
enemies firing at and damaging the client with the host distant, reannounced sibling bindings, sibling
removal, matching survivor HP after a client hit, survivor death without a
leftover mirror, corridor turret launches at the client, and direct damage from
the first enemy after the client snipes its actual sign. The baseline passed all
eight. `MUTANT=targeting` failed the real round-enemy damage check as required.
This is encounter coverage, not a completed campaign or general pathfinding proof.

Generator synchronization also required stable lifetime identities: generated
enemies can share a name, which becomes unique again when siblings die. HIT/APPLY
now address bound entities by network ID throughout their lifetime. Reannounced
spawns reuse their current binding and cannot bind another object's type or
steal a sibling's binding. The visible generator (type 511) now receives host
animation/lifetime snapshots while its client-side spawning remains disabled;
generic item spawner policy is preserved. Generators do not count as live enemies
for revive-zone clearance. A null object no longer matches an empty table slot,
which prevents a late projectile from crashing after its shooter is removed.

## Implementation and evidence

Kurt's constructor at `0x418240` makes a separate attached `chute` gob and
stores its generation-checked handle at character state `+0x44` (object info
`+0x40`). Deploy/cut-away at `0x416150`/`0x4161a0` play canopy animations 66/67.
The old snapshots covered only Kurt's controller. The new pointer-free extension
carries both canopy animation blocks, current/previous IDs, and the controller's
`0x22` flags. Body animation/orientation already carry steering. Each snapshot
contains the whole canopy state, so a lost transition can recover on the next
snapshot. Non-Kurt snapshots carry one absent byte. Character replacement and
level epochs keep using the existing puppet lifecycle. Canopy state is never
copied between engine objects by raw handle.

The engine's AI-target getter at `0x42ad50` reads a global target handle,
distinct from the local-player handle. Class think scoping alone does not cover
the game's AI:

- `script.lua` maps both Grunts and Doganboys to `mdkDoganboyAttack` through
  `DefaultAIScripts`. Its `ScriptUpdate` runs native pursuit and attack tasks
  after the class think callback has returned. The mod now scopes this path to
  the nearest living player too. Lua 3.1 protected `call` restores the handles
  even if a script task errors; non-actor scripts retain their existing path.
- The Grunt class descriptor's slot **3** (`+0x0c`, `0x434d00`) handles animation
  keys, including direct strikes. Slot **7** (`+0x1c`, `0x434ed0`) handles
  collisions. The earlier code mislabeled slot 7 as animation events, leaving
  the actual strike's target lookup unscoped. Both mirror filters remain, and
  think/strike callbacks preserve the original engine stack frame and return
  convention. The offline harness exercises the actual slot dispatcher and
  rejects wrong-slot and unscoped-strike mutants.
- The guard spawn scripts explicitly call `mdkWalkerAlert(spawnee, 1)`.
  The fixture now does likewise. No targetability setter, forced movement,
  scripted damage or forced attack animation is needed.

Native line of sight, movement and attack decisions remain authoritative.
Scripted non-player targets retain their authority; players must be alive and
in the same level/epoch. Clients mirror enemies and apply host HURT events.
The target hook checks its expected bytes and build-specific global operand.

The old Doganboy test counted damage/HURT logs and HP but did not inspect which
player the enemy targeted or distinguish direct attacks from splash. It could
therefore pass on collateral damage. The next Grunt fixture had additional
problems: no explicit activation, an eight-unit spawn already within native
strike reach, and an invented five-unit final-distance limit. Live process code
shows the Grunt's direct strike at `0x434e25..0x434e3d` passes **50 units** and
**pi/4 radians** to the engine angle/distance/LOS check (`0x402a40`). At close
range a Grunt can attack while stationary or backing away. This explains the
fixture failures without claiming those observations prove general navigation.

`qa_ai.sh` now starts a native Grunt 64 units from the selected player, records
its starting process position, then alerts it. It runs client-near/host-far and
host-near/client-far cases. Each case first sets the native AI target to the
host player, establishing the host-only control's default explicitly.
The fixture never assigns the client as the native default target; the mod must
select that client from its position. Its process oracle requires:

- Fresh native target-getter calls selecting the intended player, plus fresh
  direct strike evidence. Per-frame target seeding does not count as a query.
- The same live enemy moves more than eight units, reduces its distance by more
  than eight units, and crosses from outside 50 units to inside native reach.
  The player moves less than three units, so teleporting toward it cannot pass.
- Matching applied `qa_melee` HURT receipts and HP loss on the selected player;
  the other player is over 100 units away and keeps its health.
- The background fixture's direct-only damage filter is active. It admits only
  the native Grunt/Conehead strike callers `0x434e56`/`0x434dfb`, excluding every
  projectile, explosion and AOE path.

This establishes unobstructed native targeting, approach and direct damage;
it does not establish navigation around arbitrary obstacles. The targeting
mutant disables the fix; `hostaoe` disables direct actor damage routing, leaving
collateral damage unable to satisfy the oracle. Both must fail the client case.

Clients at the title screen wait behind a dedicated menu after joining. Host
LEVEL events still initiate loading; the UI does not invent a local game start.
Cancel disables network acceptance/sending and guards queued level follows.
Retry changes the process nonce to require a fresh host welcome. Connecting,
waiting, loading, playing, host loss, timeout, protocol mismatch and cancelled
states are tested by the portable state machine. Steam clients can access Session
before accepting an invite or entering an ID. A loading timeout is 60 seconds;
waiting for a connected host to choose a game has no deadline.

Player/relay protocol magics change, so everyone must update together. A
state-free legacy hello obtains a response from the preceding protocol's host;
that reply is rejected before world/events decode and produces update guidance.
Other unknown protocols can time out instead. Exact mod-version string matching
inside the game is not implemented; compatible protocol versions can connect.

## Ordered verifier commands

Use configured isolated disposable copies and background mode. These commands
launch games only on the verifier's machine. Baselines return zero with nonzero
passed counts; deliberately disabled variants must return nonzero because the
state assertions fail. Inspect the saved `local/qa/*-presence-*` process dumps.

```sh
python3 tools/test_presence.py
python3 -B -m unittest discover -s tests -p '*_test.py'
make -C bridge
python3 tools/qa_all.py offline
MDK2_BACKGROUND=1 bash tools/qa_join.sh
MDK2_BACKGROUND=1 MUTANT=join bash tools/qa_join.sh
MDK2_BACKGROUND=1 bash tools/qa_presence.sh
MDK2_BACKGROUND=1 MUTANT=canopy bash tools/qa_presence.sh
MDK2_BACKGROUND=1 bash tools/qa_ai.sh
MDK2_BACKGROUND=1 MUTANT=targeting bash tools/qa_ai.sh
MDK2_BACKGROUND=1 MUTANT=hostaoe bash tools/qa_ai.sh
python3 tools/qa_all.py features
```

`qa_join.sh` checks title/waiting-menu state, automatic host pull, host return to
title, cancel/retry, a 65-second network outage, and an actual legacy-magic packet
from the configured host (background-only test switch). It does not exercise a
separate old release or independent Steam accounts.

`qa_presence.sh` uses three peers to include client-to-client relay. It drives the
actual attached native canopy's open/close animations and a body steering pose,
then compares the controller state read from every process. It then drives native
Space input above a room marker, steers with A, lands at the marker, and releases
Space for cut-away. Live Steam build 3902 validation passed all 27 controller
and native-input assertions across three peers; disabling canopy replication
failed all 27. A separate three-peer native-airborne capture also showed the
client-controlled Kurt's deployed canopy in the host view. That is visual
inspection of one scenario; the automated assertions compare controller state,
not pixels, and do not establish rendering across arbitrary poses or levels.

`qa_ai.sh` exercises both direct-targeting cases in one run. A baseline must
report three passing checks: both targeting cases and restoration of the native
player handle/scope depth after a deliberately failing script task. `MUTANT=targeting` must fail the client case while its
host-near control can still pass through the original host-only AI.
`MUTANT=hostaoe` must fail because no direct HURT reaches the intended player.
Inspect the `client_before`, `client_after`, `host_before`, and `host_after`
process dumps printed by the harness.

Live revalidation on Steam build 3902 passed the AI baseline's three assertions.
The targeting mutant failed the client case while passing the host-only control;
the direct-damage-routing mutant failed both damage cases. The join baseline
passed all seven assertions, including the network outage and incompatible packet;
disabling its waiting-screen update failed five of the seven assertions.
The harness installer now clears pending background commands before a new launch:
a leftover damage command otherwise crashed a fresh client at the title screen.

The initial implementation was game-free. The portable suite uses GCC/ASan/UBSan; leak scanning is disabled
because LeakSanitizer cannot operate under the test environment's process tracing.
The engine adapter's fake-world test runs with native host pointers and explicit
32-bit controller pointers. A 32-bit native Linux runtime was unavailable. Windows
cross-compilation does not establish runtime ABI correctness.
