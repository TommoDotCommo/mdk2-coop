# mdk2-coop

Co-op for MDK2 HD (Steam app 213350, build 3902). Work in progress, nothing playable yet.
Built with the [Co-op Anything](https://github.com/cranebeaker/coop-anything) playbook; research notes live in
`games/mdk2-hd/NOTES.md` there.

No game files are in this repo. You need your own copy of MDK2 HD.

## Dev harness

`lua/coop_dev.lua` is appended to `%LOCALAPPDATA%\MDK2HD\settings.lua`, which the game executes at boot
after its own scripts.

    tools/inject.sh         # inject (backs up settings.lua once to settings.lua.coop-backup)
    tools/inject.sh --off   # remove

In game: F9 BioWare debug menu, F7 jump to level 1 checkpoint 2, F6 spawn the player 2 puppet,
F8 toggle drawing the puppet 3 units up (with `peer_port` = `listen_port` in coop.ini the puppet mirrors
your own Kurt, handy for checking sync with one instance), F11 log anim states to bridge.log, F4 teleport to level 1 checkpoint 3's room.
Room changes are logged to bridge.log as they happen.

## Bridge (dinput8.dll)

`bridge/` builds a dinput8.dll proxy (mingw-w64, `make -C bridge install GAME_DIR=/mnt/c/coop/mdk2-test`).
It forwards DirectInput to the system DLL and adds Lua functions to the game:

- `coop_init()` registers the others and opens a UDP socket (ports from `<game dir>\coop\coop.ini`,
  see `dist/coop.ini.example`)
- `coop_send(gob)` sends the gob's position and orientation to the peer
- `coop_recv()` returns the peer's latest `x, y, z`, or nil before the first packet
- `coop_log(msg)` writes a line to bridge.log
- `coop_face(gob)` gives the gob the peer's latest orientation and animation state

Log: `<game dir>\coop\bridge.log`. Only Steam build 3902 is supported; on any other build the bridge
logs "unknown game build" and only forwards DirectInput.

UDP on localhost/LAN is a stand-in; Steam relay networking comes later.

Untested: changing options in game probably rewrites settings.lua and drops the injection; if so, rerun the script.
