# Player presence changes and verification

This change has game-free evidence only. Live behavior, engine callback ABI,
canopy attachment/rendering, Grunt fixture placement, and Lua 3.1 menu behavior
still require the supported game's processes. Do not treat the new scenarios as
passed until the commands below have run successfully.

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

The engine's AI-target getter at `0x42ad50` reads a global target handle, distinct
from the local-player handle. The mod previously did not change that getter.
The host now wraps class think and animation-event callbacks (slots 1 and 7),
scopes the querying actor, and returns the nearest living player in the same
level/epoch. Native AI retains its line-of-sight, navigation and attack checks.
Scripted targets other than players keep their original authority. The hook
checks its six expected bytes, including the build-specific global operand.
Clients continue to mirror enemies and receive host-authoritative HURT events.

The old `qa_ai.sh` spawned a Doganboy, counted `damage` and HURT log lines, and
checked HP. It never inspected which gob the AI selected, never disabled splash
sources, and never distinguished direct attacks from explosions hitting a puppet.
A damage callback naming `coop_p<n>` establishes the victim of that damage, not
the enemy's intended target. Moving a ranged enemy beside a client can put that
client in its splash radius even when it still aims at the host. Consequently
those checks could pass without proving client LOS targeting. Which particular
attack produced the owner's observed failures is unknown without live evidence.

The replacement uses a native Grunt, places the host more than 25 units away,
and starts the enemy eight units from the client. Its melee animation callback
queries the AI target, checks the strike, and directly calls damage, returning
to `0x434e56`. Background-only fixture mode rejects every other host damage
caller (also permits Conehead's corresponding direct strike at `0x434dfb`).
Thus projectile/explosion/AOE damage cannot satisfy this scenario. Atomic process
dumps include selected target, query/hit counts, enemy position, client-applied
actor HURT source/count, both players' health, and whether the direct-only filter
is active. No damage is injected by the scenario. LOS remains the engine's check;
this fixture proves an unobstructed target, not navigation around obstacles.

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
Space for cut-away. These input/terrain fixtures are written but unrun: marker
height, room clearance, bindings and native timing must be established by the
verifier. Rendered visibility also remains unverified; controller equality alone
does not establish pixels on screen.

`qa_ai.sh` requires the enemy's selected target to be the client, a native strike
on that target, the enemy to reach melee range, the matching HURT source to be
applied on the client, a health drop there, and unchanged faraway host health.
`targeting` disables the target patch; `hostaoe` drops actor-to-client HURT routing
while keeping the direct-only filter. Neither may pass. Repeat with host/client
roles exchanged and additional enemy families before claiming broad AI coverage.

Native offline Windows executables and live T1–T3 checks were not run during
implementation. The portable suite uses GCC/ASan/UBSan; leak scanning is disabled
because LeakSanitizer cannot operate under the test environment's process tracing.
The engine adapter's fake-world test runs with native host pointers and explicit
32-bit controller pointers. A 32-bit native Linux runtime was unavailable. Windows
cross-compilation does not establish runtime ABI correctness.
