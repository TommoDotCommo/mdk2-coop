# mdk2-coop

Co-op for MDK2 HD (Steam app 213350, build 3902). Alpha: two or more players can play through the
campaign together (host runs the world, up to 16 players), but the whole campaign has not been finished in
co-op yet. Every checkpoint passes an automated two-client sweep.
Built with the [Co-op Anything](https://github.com/cranebeaker/coop-anything) playbook; research notes live in
`games/mdk2-hd/NOTES.md` there.

No game files are in this repo. You need your own copy of MDK2 HD.

## Playing with friends

Download the zip from [Releases](https://github.com/TommoDotCommo/mdk2-coop/releases), unzip it and
double-click **Install.cmd**. No dev tools needed. The installer finds MDK2 HD in your Steam libraries (or asks
for the folder), refuses any build other than Steam 3902 (SHA-256 of `mdk2hd.exe`
`bf5faa81b18837b4c07f35941b107e5c23caa8b105536ccd9c8e5a8e37357f43`), asks *host or join* (and the host's
address and port), and copies the mod in. **Uninstall.cmd** takes it out again. The zip's README.txt has the
details; this is the short version.

**How to host**
1. Install, answer *host* (UDP port 27900 unless you change it).
2. Forward UDP 27900 on your router to your PC. Only the host needs this, until Steam networking replaces
   direct connections.
3. Tell your friends your public IP (search "what is my IP"), or your 192.168.x.x address on the same network.
4. Start the game; allow MDK2 HD when Windows Firewall asks (Private and Public). If you missed the prompt,
   run this in an administrator prompt:
   `netsh advfirewall firewall add rule name="MDK2 co-op" dir=in action=allow protocol=UDP localport=27900`
5. Start a New Game or load a save; players who join follow you into your level.

**How to join**
1. Install, answer *join*, type the host's address and port.
2. Start the game after the host. You arrive in the host's level next to them.

The installer's answers are only the first setup: run it again to switch, or change them in game in the
Options > Co-op menu (being built). The installer changes `<game>\dinput8.dll`, `<game>\coop\coop.lua`, the
`[net]` keys of `<game>\coop\coop.ini` (other keys are kept) and one loader block in
`%LOCALAPPDATA%\MDK2HD\settings.lua` (backed up first as `settings.lua.coop-backup`, restored on uninstall).
The game rewrites settings.lua whenever options are saved, which drops that block. The bridge puts it back
when the game starts (and creates settings.lua with just the block on a first start), so co-op keeps working.

**Making a release** (maintainers): `tools/release.sh v0.x.y` builds `local/release/mdk2-coop-v0.x.y.zip`
(dinput8.dll, coop\coop.lua, Install.cmd, Uninstall.cmd, mdk2-coop.ps1, README.txt, LICENSE.txt), then
`gh release create v0.x.y local/release/mdk2-coop-v0.x.y.zip`.

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
- `coop_send(gob)` sends the gob's position and orientation to the others
- `coop_recv()` reads the network
- `coop_peer(slot)` returns player `slot`'s `x, y, z` for this frame, or nil until it has sent one in this level
- `coop_slot()`, `coop_connected(slot)`, `coop_players()` (others connected, number of slots), `coop_peer_type(slot)`
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

Players (up to 16, tested with 8): the host is player 1 (slot 0) and gives each client a slot when its
first packet arrives (`we are player N` in the client's log). Clients only talk to the host, which
passes each client's state, shots, pickups and markers on to the others. Every game draws a puppet
(`coop_p<N>`) for each other player; a player silent for 10 s has left and its puppet goes. Players'
characters travel as `@p<N>` in events, the same name in every game. A client that joins (or restarts)
while the host is in a level is sent the level, every live enemy with its hp, and the items already
taken. Level epochs: the host counts the level loads it announces; states and events from another
epoch are old-level leftovers and are dropped, and a client holds events back while it is still
loading the level they belong to.

Dropping out (no progress lost): every game copy has a player id (`coop\player.ini`, made on first
run) and the host remembers each id's number, also across its own restarts, so a player who comes back
is the same player. Co-op saves: 10 s into each level, a few seconds after each checkpoint and every
2 minutes (`coop.save_interval`), the host calls a save and every game in the level makes a full instant
save of its own world (the game's quicksave: inventory, hp, position, every object) under the same
number, in two alternating files (`save\c<id><a|b>.sav`, recorded in `coop\saves.ini`).
- A client whose game crashes comes back as the same player and loads its own copy of the newest save
  of that level; the host then sends its world again (enemies with hp, taken items).
- A client whose connection drops sees "Connection to the host lost - reconnecting" (input goes to
  that message, nothing moves in its world without the host) and plays on when the host is heard
  again, without reloading anything; after 10 s the host drops its puppet and takes it back when it
  returns.
- When the host's game crashes, the clients wait the same way. The host's Load Game menu (title
  screen or pause) then offers "Resume co-op <level>-<checkpoint>", and every client loads its copy of
  that save. Where the game cannot save (Max's tutorial at the start of level 2), the host's progress
  (each level start and checkpoint) is newer than its last save, and Resume starts that checkpoint
  instead of going back a level.

Damage authority (`[net] role=host|client`): the bridge hooks the game's central damage function
(0x40d590). The host applies everything and tells the client (APPLY); the client applies damage to its own
player only and sends its hits on anything else to the host (HIT), which applies them as if the client's
puppet fired. Hits on the client's puppet in the host's world go to the client's player (HURT). Events are
numbered and resent until acked. Targets are matched by gob name, so unnamed gobs are not synced yet.
- `coop_face(gob, slot)` gives the gob that player's latest orientation and animation state

Log: `<game dir>\coop\bridge.log`. Only Steam build 3902 is supported; on any other build the bridge
logs "unknown game build" and only forwards DirectInput.

Updates go out at `send_hz` (30) and the other player is drawn `interp_ms` (100) in the past, blended
between the two updates either side; `[sim]` fakes latency, jitter and loss for testing. While the other
player moves, bridge.log gets a `smooth:` line every 2 s (step size mean/sd/max, stalled frames, cv = sd/mean;
lower is smoother).

UDP on localhost/LAN is a stand-in; Steam relay networking comes later.

Untested: changing options in game probably rewrites settings.lua and drops the injection; if so, rerun the script.

## Manual playtest on one PC

    tools/play.sh          # install into the copies, start host (left) and client (right)
    tools/play.sh stop     # close them, remove the settings.lua line, save every log to local/sessions/
    MDK2_N=3 tools/play.sh # host and two clients (rows of three windows); stop with the same MDK2_N

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

More players: `MDK2_N=8 tools/qa_n.sh` (default 3) runs a host and N-1 clients: everyone joins and
follows, puppets for all in every game, enemies and hp everywhere, a client's shots, pickup and door
seen by the other clients, damage reaching the right client, a client leaving and joining again
mid-level, checkpoint keys, level 2. `MDK2_N=3 tools/qa_save.sh` checks dropping out: co-op saves in every game, a client crash
(back with its saved hp, position and number), a 15 s network cut (`coop_netblock`), a host crash
and resume, and a level change after it. `tools/copies.sh` makes the extra game copies (mdk2.tar
hard-linked, ~2 MB each). Folders and ports are set by `MDK2_DIR_BASE` / `MDK2_DIR<i>` and
`MDK2_PORT` (see `tools/mdk.sh`), so two sets of copies can run side by side; `mdk_kill` only stops
games running from its own copies. 2026-10-21: qa.sh 30/30, qa_n.sh 38/38 with 3 players and 53/53
with 8 (~370 MB per game), qa_save.sh 41/41 with 8; after adding the resume-at-checkpoint case, 38/38 with 3.

Checkpoint sweep: `tools/sweep.sh [from] [to]` visits every checkpoint of the debug menu's list (96)
with MDK2_N games (default 2), has everyone walk and fire and every client land a hit, then checks that all
followed, all alive, puppets, every client bound the host's enemies, hp equal, no Lua errors, and diffs
every game's world against the host's. About 55 minutes with two; `local/sweeps/<date>/results.md`.
2026-10-21: with `MDK2_N=3`, 1-1 .. 1-3 and 8-4 .. 8-5 all clean (every client bound every enemy, equal hp, no state differences).

Last runs (2026-10-20, Steam build 3902, level 1 cp 2): 21/21 on plain localhost (after the first
manual session's client crash was fixed); earlier 17/17 with `[sim] latency_ms=120 jitter_ms=40
loss_pct=5` on both sides. The enemy-attack and shot-replay checks depend on the Doganboy acting during
the window; in the lossy run it did not (0/0).

## License

MIT, see [LICENSE](LICENSE). Covers this repo's code only; MDK2 HD itself is not included.
