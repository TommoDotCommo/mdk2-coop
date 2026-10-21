# Recorded progression routes

The bot records keyboard state, mouse buttons and relative mouse motion inside the DirectInput
bridge. During replay the game handles these controls normally. Recorded positions, orientations
and checkpoints are assertions; only the initial checkpoint is loaded by the launcher.

## Record

Start a fresh session at the checkpoint where the route should begin, using the current bridge:

```sh
tools/play.sh 1 2
tools/replay.sh record room2
# Play through at least one later checkpoint.
tools/replay.sh stop room2
```

`record` and `stop` use the running session's PID file. They also work in background mode with
`coop_press` commands; the recorder captures what the game actually reads. An optional third
argument selects a game instance (default 1, the host). Record before moving, turning or fighting
at the starting checkpoint. Use the same controls, mouse sensitivity, game settings and co-op
player count when replaying. Short routes between checkpoints make failures easier to diagnose.

The recording is exported to `local/routes/room2.route`. The in-game commands are
`coop_route("record", "room2")`, `coop_route("stop")`, `coop_route("replay", "room2")`
and `coop_route("status")`; the last returns the current status and refreshes
`coop/route_status.txt`. Names contain letters, digits, `_` and `-` (up to 63 characters).
Existing recordings are never overwritten. A failed/interrupted recording stays as
`<name>.route.partial` for diagnosis; choose a new name to record again.

Menus, developer keys, death, disconnection and checkpoint reloads fail an active recording or
replay. A recording that ends at its initial checkpoint is rejected. Checkpoint IDs are compared
in the recorded sequence, since some campaign checkpoints are out of number order.

## Replay and check co-op

```sh
tools/replay.sh run local/routes/room2.route
MDK2_N=3 tools/replay.sh run local/routes/room2.route
MDK2_SIM='80 30 3' tools/replay.sh run local/routes/room2.route
```

The runner validates the entire file with the same parser as the bridge before changing any
copies. It installs the current build, starts a background host and clients, loads the recorded
start once, and replays the host's controls. Clients stay in the world as co-op observers, so the
run checks progression with teammates present. It uses the copy/port/PID settings in `tools/mdk.sh`;
set `MDK2_DIR_BASE`, `MDK2_PORT` and `MDK2_PIDS` to isolate concurrent sessions.

A pass requires all recorded checkpoint/level transitions in order, a living player, the recorded
endpoint and all co-op participants. Start position must be within 0.75 units and orientation
within about 11 degrees of the recording. Sustained position drift over 12 units fails after two
seconds. A transition has five seconds of timing grace; after the recorded duration all inputs
are released and the endpoint must be reached within five seconds, within four units.
Timed input is sensitive to simulation timing and combat randomness; drift is reported as a failed
route, so a failed replay alone does not establish a co-op defect.

At each observed progression boundary and at the end, the runner compares all worlds using
`statecheck.py`; a mismatch is retried after 1.5 seconds. A missing/dead player, missing puppet,
Lua error, crash, startup timeout or missing replay result fails the run. The runner prints
`RESULT: PASS` or `RESULT: FAIL`, returns nonzero on failure, saves evidence under `local/replays/`
and stops its own game copies. Evidence includes the input route, game logs, participant checks,
state dumps and comparisons; failures also capture game screenshots and available crash dumps.

Recordings are plain text, versioned for Steam build 3902, and never executed as Lua. They include
player transforms but no game assets. Keep experimental recordings in ignored `local/routes/`;
only add a route here after it has passed a real two-client replay, with its settings and evidence.
No campaign route is bundled yet; this tool does not establish full campaign coverage.

## Tests without the game

```sh
tools/test_replay.sh
```

These exercise the actual route parser/player and progression oracle, including incomplete files,
invalid inputs, wrong starts, missed checkpoints, drift, endpoint checks and participant failures.
They do not substitute for replaying a recorded route in real game clients.
