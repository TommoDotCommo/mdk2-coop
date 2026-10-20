# mdk2-coop

Co-op for MDK2 HD (Steam app 213350, build 3902). Work in progress, nothing playable yet.
Built with the [Co-op Anything](https://github.com/cranebeaker/coop-anything) playbook; research notes live in
`games/mdk2-hd/NOTES.md` there.

No game files are in this repo. You need your own copy of MDK2 HD.

## Dev harness

`lua/coop_dev.lua` is installed as `<game dir>\coop\coop.lua`; the bridge runs it from `coop_init()`, which one
line added to `%LOCALAPPDATA%\MDK2HD\settings.lua` calls after the game's own scripts. Keep settings.lua
small: the game reads it into an 8 KB stack buffer and crashes (0xc0000409) if it is bigger.

    tools/inject.sh         # install (backs up settings.lua once to settings.lua.coop-backup)
    tools/inject.sh --off   # remove the settings.lua line

In game: F9 BioWare debug menu, F7 jump to level 1 checkpoint 2, F6 spawn the player 2 puppet,
F8 toggle drawing the puppet 3 units up (with `peer_port` = `listen_port` in coop.ini the puppet mirrors
your own Kurt, handy for checking sync with one instance), F11 log anim states to bridge.log, F5 (host) spawn a Doganboy ahead (coop_dog1, 2, ...), F4 teleport onto coop.tpname (level 1's decoy item, in another room).
Room changes are logged to bridge.log as they happen.

## Bridge (dinput8.dll)

`bridge/` builds a dinput8.dll proxy (mingw-w64, `make -C bridge install GAME_DIR=/mnt/c/coop/mdk2-test`).
It forwards DirectInput to the system DLL and adds Lua functions to the game:

- `coop_init()` registers the others and opens a UDP socket (ports from `<game dir>\coop\coop.ini`,
  see `dist/coop.ini.example`)
- `coop_send(gob)` sends the gob's position and orientation to the peer
- `coop_recv()` returns the peer's latest `x, y, z`, or nil before the first packet
- `coop_log(msg)` writes a line to bridge.log
- `coop_gob(name)` returns the gob for an `n<id>` network name; `coop_role()` returns "host" or "client"
- `coop_event()` returns the next damage event from the peer (`kind, name, damage, type, part`) or nil
- `coop_apply(kind, gob, damage, type, part)` carries one out on the gob Lua found by name

Enemies (`OBJ` types 200-259): the host scans the engine's gob table and announces each actor with
a reliable SPAWN (network id = its gob handle; unnamed ones are called `n<id>`), then sends position,
rotation and animation `ent_hz` (15) times a second; DESPAWN when it goes away. The client binds each to
its own gob of the same name, or creates one, skips the AI ("think" slot) of their classes, draws them
from the host's snapshots, and deletes actors its own spawners make that the host never announced.

Shots (`OBJ` types 400-499): every gob is made by the engine's create_object (0x42d360), which the
bridge hooks. Creating a shot sends a reliable FIRE event (the host announces every shot, the client its own
Kurt's); the peer replays it through the same function, with the target and owner translated, so it flies
with the game's own physics. Replayed shots are only for show: their damage is dropped and the real hits
travel as HIT/APPLY/HURT.

Pickups (`OBJ` types 300-399): every pickup path asks the engine whether the item has an OnPickedUp
handler (0x40cf60, event 15), which the bridge hooks. The picker keeps the item and sends PICKUP; the
peer deletes its copy. Items are matched by name, so only level-placed items are synced so far.

Rooms: the engine only runs rooms around the camera (others go into stasis, flag 0x8). The bridge
redirects the room manager's call at 0x430314 so the rooms around the other player's puppet stay active
too (enemies, doors and scripts keep running where either player is). Proximity doors only check the
local player; their think is wrapped to use whichever player is nearer.

Damage authority (`[net] role=host|client`): the bridge hooks the game's central damage function
(0x40d590). The host applies everything and tells the client (APPLY); the client applies damage to its own
player only and sends its hits on anything else to the host (HIT), which applies them as if the client's
puppet fired. Hits on the client's puppet in the host's world go to the client's player (HURT). Events are
numbered and resent until acked. Targets are matched by gob name, so unnamed gobs are not synced yet.
- `coop_face(gob)` gives the gob the peer's latest orientation and animation state

Log: `<game dir>\coop\bridge.log`. Only Steam build 3902 is supported; on any other build the bridge
logs "unknown game build" and only forwards DirectInput.

Updates go out at `send_hz` (30) and the other player is drawn `interp_ms` (100) in the past, blended
between the two updates either side; `[sim]` fakes latency, jitter and loss for testing. While the other
player moves, bridge.log gets a `smooth:` line every 2 s (step size mean/sd/max, stalled frames, cv = sd/mean;
lower is smoother).

UDP on localhost/LAN is a stand-in; Steam relay networking comes later.

Untested: changing options in game probably rewrites settings.lua and drops the injection; if so, rerun the script.
