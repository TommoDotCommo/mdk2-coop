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
your own Kurt, handy for checking sync with one instance), F11 log anim states to bridge.log, F5 spawn a Doganboy ahead (coop_dog1, 2, ...), F4 teleport to level 1 checkpoint 3's room.
Room changes are logged to bridge.log as they happen.

## Bridge (dinput8.dll)

`bridge/` builds a dinput8.dll proxy (mingw-w64, `make -C bridge install GAME_DIR=/mnt/c/coop/mdk2-test`).
It forwards DirectInput to the system DLL and adds Lua functions to the game:

- `coop_init()` registers the others and opens a UDP socket (ports from `<game dir>\coop\coop.ini`,
  see `dist/coop.ini.example`)
- `coop_send(gob)` sends the gob's position and orientation to the peer
- `coop_recv()` returns the peer's latest `x, y, z`, or nil before the first packet
- `coop_log(msg)` writes a line to bridge.log
- `coop_event()` returns the next damage event from the peer (`kind, name, damage, type, part`) or nil
- `coop_apply(kind, gob, damage, type, part)` carries one out on the gob Lua found by name

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
