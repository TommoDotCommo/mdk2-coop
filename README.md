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

In game: F9 BioWare debug menu, F6 / F7 next / previous checkpoint (host),
F8 next level (host), F2 marker in both logs, F11 log anim states to bridge.log, F5 (host) spawn a Doganboy ahead (coop_dog1, 2, ...), F3 deal 200 lava damage to yourself, F1 (host) spawn a Poopsy named coop_spawn (same name each time), F4 teleport to the next test spot (level 1's decoy item, door dr1_03, then 400 units below you).
Room changes are logged to bridge.log as they happen.

Background test mode (`tools/qa.sh` and `tools/sweep.sh` use it; `MDK2_BACKGROUND=1 tools/install.sh` sets
`[test] background=1` in coop.ini): both games open side by side on the second monitor, behind other windows,
never take focus, are muted and capped at 60 fps (`[test] fps`). They ignore the real keyboard and mouse;
scripts write a Lua line to `<game dir>\coop\command.lua`, which the bridge runs within a few frames:
`coop_press(code, ms)` holds a DirectInput scancode (256 / 257 = left / right mouse) and `coop_shot(name)`
saves the next frame to `coop\<name>.tga`. `tools/play.sh` stays in normal mode for playing by hand.

State checks: `coop_state(tag)` on the host writes `coop\state_<tag>.txt` (players, puppet, synced enemies,
other actors, items, doors) and has the client do the same; `tools/statecheck.py host client` reports where
the two worlds disagree. `mdk_state` (tools/mdk.sh) runs both; qa.sh checks state at four points and
sweep.sh at every checkpoint.

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
travel as HIT/APPLY/HURT. Not yet: shots created without a position and placed afterwards (the
Doganboy's grenade, 417) are logged as "not replicated".

Pickups (`OBJ` types 300-399): every pickup path asks the engine whether the item has an OnPickedUp
handler (0x40cf60, event 15), which the bridge hooks. The picker keeps the item and sends PICKUP; the
peer deletes its copy. Items are matched by name, so only level-placed items are synced so far.

Rooms: the engine only runs rooms around the camera (others go into stasis, flag 0x8). The bridge
redirects the room manager's call at 0x430314 so the rooms around the other player's puppet stay active
too (enemies, doors and scripts keep running where either player is). Proximity doors only check the
local player; their think is wrapped to use whichever player is nearer.

Levels (the host decides): the bridge runs `coop.Tick()` after every scene update (0x402638, menus and title included), so
sync no longer depends on the puppet. Every level load ends in the Lua `level()` function, which the harness
wraps: it resets the bridge's per-level state and, on the host, asks the client to follow with a LEVEL event (not for the title
screen, and not when the load was itself following the peer) and the next tick adds the puppet. Events and
enemy packets carry the sender's level and are dropped by a game in another level.

Going down: with a partner connected, a hit that would kill your Kurt revives him at the partner with the
health he started the level with, instead of reloading the checkpoint in one game only. Falling more than
30 units below the level's lowest room (some edges have no kill plane, Kurt falls forever) also brings you
back to the partner.

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

## Manual playtest on one PC

    tools/play.sh          # install into both copies, start host (left) and client (right)
    tools/play.sh stop     # close both, remove the settings.lua line, save both logs to local/sessions/

Play in the left window; the right one follows level loads by itself. `tools/play.sh <level>
<checkpoint>` starts both straight at a checkpoint; in game the host's F6 / F7 go to the next / previous
checkpoint (the debug menu's list: 1-1 .. 1-8, 2-1 .. 2-10, ...) and F8 to the next level. The game's own
quicksave (F6) and quickload (F7) are switched off while the mod runs: a quickload restores a save the
partner cannot follow.

Controls while the mod is on: Space jumps (jetpack, parachute), right mouse toggles Kurt's sniper mode
(the game's defaults are the other way round). F2 drops a numbered marker in
both logs, F8 skips to the next level.

The puppet is never placed within 3 units of your own player (it is solid and moved every frame;
both players spawn on the same point and an overlapping puppet shoved the host 25 units into a wall).
The puppet is the same character as your own player (each level fixes Kurt, Max or Doc and only loads
that model); a different partner type is logged.

## Automated checks

`tools/qa.sh` runs two real game instances on this PC (host + client over localhost UDP), drives them
with key and mouse input and checks each side's bridge.log: level following, enemy binding, damage
through the host with identical enemy hp, HURT-only damage on the client, shots both ways, pickups,
doors opening for the puppet, reviving at the partner, no crash. About 2 minutes; the game windows
take focus. `tools/mdk.sh` has the helpers (start, keys, mouse, window screenshots).

`tools/qa.sh` installs the current build first (`tools/install.sh`), so it always tests the repo.

Last runs (2026-10-20, Steam build 3902, level 1 cp 2): 21/21 on plain localhost (after the first
manual session's client crash was fixed); earlier 17/17 with `[sim] latency_ms=120 jitter_ms=40
loss_pct=5` on both sides. The enemy-attack and shot-replay checks depend on the Doganboy acting during
the window; in the lossy run it did not (0/0).
