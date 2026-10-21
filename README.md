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
`bf5faa81b18837b4c07f35941b107e5c23caa8b105536ccd9c8e5a8e37357f43`), asks *Steam or UDP* and *host or join*,
and copies the mod in. New installs default to Steam; existing configurations keep their transport. **Uninstall.cmd** takes it out again.

**How to host over Steam**
1. Install, choose *steam*, then *host*. Keep Steam running and online.
2. Start MDK2 HD from Steam. In **Options > Co-op > Session**, choose **Invite Steam friends**,
   or share the numeric **Lobby ID** shown there.
3. Start a New Game or load a save. Friends follow you into your level.

**How to join over Steam**
1. Install, choose *steam*, then *join*. Start MDK2 HD from Steam.
2. Accept the host's Steam invite, use **Join Game** in Steam's friends list, or enter their
   lobby ID in **Options > Co-op > Session > Join lobby by ID**. Start the game before accepting
   an invite; Steam's launch argument `+connect_lobby <id>` is also supported.

Steam sessions use friends-only lobbies and modern Steam P2P messages with relay support. No public
IP, router port forwarding, SDK download, Steam DLL replacement, or Spacewar AppID is needed. Every
player must use the Steam transport and a compatible mod version. If the overlay is disabled, share
the lobby ID instead. Errors and the current lobby status appear in Session and `coop\bridge.log`.

**Direct UDP (LAN/testing or manual internet setup)**
Choose *udp* at install, then *host* or *join*. The host listens on UDP 27900 by default, forwards
that port on their router for internet play, and gives friends their public IPv4 address and port.
On the same LAN use the host's local IPv4 address. Allow MDK2 HD through Windows Firewall. A joining
player enters that address and port. Old `coop.ini` files without a `transport` key keep using UDP.

The installer's answers are only the first setup: run it again to switch, or change them in game in the
Options > Co-op > Session menu (network and startup role changes need a restart). The installer changes
`<game>\dinput8.dll`, `<game>\coop\coop.lua`, the
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

In game (developer keys: Options > Co-op > Gameplay, on in the test installs, off for players):
F9 BioWare debug menu, F6 / F7 next / previous checkpoint (host),
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

- `coop_init()` registers the others and starts the configured transport (Steam lobbies or UDP;
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
rotation and animation up to `ent_hz` (15) times a second, but only while the enemy changes (two more after
it stops, then one a second while it rests: most are in sleeping rooms); DESPAWN when it goes away. The client binds each to
its own gob of the same name, or creates one, skips the AI ("think" slot) of their classes, draws them
from the host's snapshots, and deletes actors its own spawners make that the host never announced.

Shots (`OBJ` types 400-499): the bridge hooks the engine's create_object (0x42d360) and shot launcher
(0x403b00), sending reliable FIRE events (the host announces enemy shots, each player their own).
Class 5 shots carry their final launch position, rotation, velocity and fuse; this includes Doganboy
grenades (417), created attached to a hand with no position or rotation and launched afterwards.
The peer translates the target and owner, creates the shot and restores its flight state so the game's
own physics takes over, without rerunning shooter scripts. Other shot classes replay their create_object
arguments; custom models and missing creation transforms remain unsupported for those classes.
Replayed shots are only for show: their damage is dropped and the real hits travel as HIT/APPLY/HURT.
Shots produced by copied attack animations are discarded when launched, avoiding duplicate projectiles.
`coop_shots_dump()` logs live class 5 shots, positions, velocities and ownership flags for diagnostics.

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

Going down (`bridge/revive.c`, `tools/qa_revive.sh`): with teammates in the game, a hit that would kill
you puts you **down** instead: you lie where you fell (the game's death animation, held) and cannot move or
shoot, and enemies stop aiming at you. A teammate who holds **R** next to you (5 units) for 3 s stands you up
with half your health; your 30 s clock stops while someone is reviving. Bleed out and you are **dead**: you
watch a living teammate (click: the next one) and come back next to them when no enemy is left standing in
your zone (the room your body lies in and the rooms visible from it, checked by the host) or when a
checkpoint is reached. Lava, black holes and falling more than 30 units below the level's lowest room (some
edges have no kill plane) leave no body to reach: straight to dead. Everyone down or dead: the host reloads
the checkpoint and everyone follows (the load is forced, so clients already at that checkpoint reload too).
Alone, the game's own death.
How it is done: the downed player is marked mirrored (its think is skipped, like a puppet's) and the death
animation's state is applied every frame (the game's death sequence cannot be stopped without a reload);
a dead player's hidden, untargetable body rides 3 units behind the teammate it watches, so the normal
camera follows them, and other games park its puppet out of the way. Each player packet carries a life
byte (alive, down, dead, spectating) and who they are reviving; the HUD panel shows DOWN and dead.

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

On screen (`bridge/hud.c`, Options > Co-op > Gameplay > Players on screen, `[hud] show`): a panel at
the top left with each other player's number, name, health bar and hp, character, distance (game
units; Kurt is about 1.45 tall) and ping, "loading" while they are not in our level; and a name tag over
each partner's head, pinned to the screen edge with an arrow when they are out of view, stacked when
they overlap. Both show through walls (no depth test), and each partner also gets a coloured capsule
outline (a flat one when downed) sized from their position (Options > Co-op > Gameplay > Player
outlines, `[hud] outline`); it is a fixed-size outline, not the character's real silhouette. Drawn into the back buffer in the SwapBuffers hook (screenshots include it) with the
fixed-function pipeline of the game's legacy GL context (Windows font through wglUseFontBitmaps), with
the game's shader program, framebuffer, texture unit, matrices and attributes put back. The tags
project through the game's camera gob (`mdkGetCameraGob`: looks along its +y, +z up) with a 64 degree
vertical field of view, measured from puppets at known spots. Hidden under menus. `tools/qa_hud.sh`.

Options > Co-op (the game's own menus, on the title screen and in the pause menu's Options):
- Players: everyone connected, with player number, name and ping (the host measures every client, a
  client its host).
- Session: your name (`coop\player.ini`, the others see it at once), host or join (next start), the
  transport (next start), Steam lobby status/ID, invites, lobby joining/leaving, or the UDP
  host's address (a client reconnects at once); Resume co-op and Save now on the host. Change lobbies
  from the title screen. A disconnected Steam client can accept a new invite or press Esc on the
  waiting screen to join by ID; its next LEVEL reloads the host's world, including at the same checkpoint.
- Gameplay (`coop.ini`; the host's settings count): friendly fire (off by default; hits between
  players arrive as enemy gun damage, since the game ignores a player hitting a player), tougher
  enemies (+0/25/50/100% hp per extra player, default 50, set when an enemy first appears), co-op
  save interval, modern controls (Space jumps, right mouse snipes) and developer keys (F1, F3-F11 and
  the F9 debug menu; off for players, on in the test installs; F2 stays on as the report marker).
Lua reads and writes the settings through `coop_get(section, key, default)` / `coop_set(section, key,
value)`, which change one key in place. `tools/qa_menu.sh` drives it, keys included.

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

Steam networking (`bridge/steam.c`): the game owns its original `steam_api.dll` and Steam connection.
The bridge requests versioned `SteamNetworkingMessages002`, matchmaking, friends and utilities interfaces
through that connection, under MDK2 HD's AppID 213350. It registers ordinary callbacks alongside the
game's callbacks; it never takes over the game's callback queue or shuts Steam down. Lobby metadata
identifies the mod and wire protocol. Only authenticated members of the current lobby can send packets;
a client accepts only the original host. Steam owner election does not migrate the game simulation:
when the host leaves, clients wait for a new invite or lobby ID. A transient P2P session failure restarts
on the next send. Restarted hosts create new lobbies, so their clients must join those lobbies again.
The existing event acknowledgements, snapshots, interpolation, loss simulation and timeouts run over
both transports. Steam sends snapshots unreliably with NoNagle; events retain the bridge's own retries.
Steam traffic counters report payload bytes; UDP counters also include the IPv4/UDP header.

`tools/test_network.sh` builds and runs offline Win32 tests under WSL/Windows: simulated Steam interfaces
exercise lobby lifecycle, invites, authentication, failures and real bridge packet handling; loopback
sockets exercise the UDP path. Configuration storage is simulated too, because Windows profile APIs
reject WSL UNC paths. No Steam account or game process is used.

The opt-in `tools/steam_smoke.c` test uses the game's original Steam DLL and the logged-in account
to create and leave friends-only lobbies, without starting the game or sending invites. On 2026-10-21
it passed two real lobby create/leave cycles under AppID 213350, including interface access,
owner/member IDs and protocol metadata. The offline network suite passed 234 assertions and the
installer suite below passed 22 checks. Live two-account invites, relay connectivity and gameplay
still need validation; the older campaign QA below used UDP.
Steam requires a separate logged-in account for each player; same-PC multi-instance QA uses UDP.

`tools/test_installer.ps1 -GameExe <original exe> -PackageDir <unpacked release> [-TestRoot <scratch>]`
checks new installs, upgrades, transport selection, lobby IDs, settings preservation and uninstall in
disposable fixtures. It copies the executable to check its hash, never runs it, and overrides the settings
path so the real game settings stay untouched. Run it from a Windows filesystem; from WSL, give an
NTFS scratch directory with `-TestRoot`.

To build the Steam smoke test: `i686-w64-mingw32-gcc -O2 -o steam_smoke.exe tools/steam_smoke.c
-static-libgcc`. With permission to use the Steam account, run it on Windows as
`steam_smoke.exe "<game dir>\steam_api.dll" "<scratch directory>"` while Steam is online.
The test leaves both lobbies and shuts down only its own Steam API instance.

Network traffic. Snapshots go out compact (`snap_encode` in net.c): position, rotation as 4 x int16, and
of each running animation only its non-zero values, play positions and weight as 16-bit numbers; a
`struct snap` in memory is ~390 bytes, a resting enemy on the wire 23 and a running animation ~30. The
host relays the other players' states to each client in one bundle per send tick ("MDKS"), with names
about once a second. Packet magics are the protocol version: "MDKC" player, "MDKF" enemies, "MDKS"
relays; builds that differ ignore each other. `bridge/netstats.c` logs `net out:` every 10 s (kbit/s by
kind, per link). `MDK2_N=8 tools/netmeasure.sh [level checkpoint]` measures the host's upload at 2-1
(71 enemies) while everyone stands and while everyone moves. 2026-10-21, 8 players at 2-1, everyone moving:
27.0 Mbit/s before (enemies 21.7, relayed states 4.6), 1.3 Mbit/s (enemies 0.1, relay bundles 0.9, own state 0.3) after; a client uploads ~36 kbit/s.
`MDK2_SIM="80 30 3" tools/<suite>.sh` runs a suite with 80 ms latency, 30 ms jitter and 3% loss in every game.

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

Projectile regression: `make -C bridge test-shots` (or `tools/test_shots.sh`) tests capture/replay against
a fake engine (no game files needed). `MDK2_N=3 tools/qa_shots.sh` launches type 417 on the host
and a client, checking one launch, actual flight and suppressed replay damage in every game, including
the client-to-client relay. It does not depend on an enemy choosing a grenade attack.
`tools/qa_shots_native.sh` checks a Doganboy's held grenade through release, motion and expiry, matching
launch transforms on every peer, and exactly one replay of a client's native sniper launch.
If the calling process forces Windows to prefer system DLLs over the proxy, set
`MDK2_PROCESS_SERVICE=1` to launch test games through Windows' process service.

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
with MDK2_N games (default 2), has everyone walk and fire and every client land a hit (all games at
once), then checks that all followed, all alive, puppets, every client bound the host's enemies, hp
equal, no Lua errors, and diffs every game's world against the host's; `local/sweeps/<date>/results.md`.
It waits on conditions, not fixed sleeps: until every client has bound every announced enemy, and
until hits and revives stop showing up. A state difference is checked again 1.5 s later; one that is
gone by then is reported as "transient" (a revive's teleport caught mid-way), not as a failure.
Screenshots only where something failed (`MDK2_SHOTS=1`: always). `MDK2_SHARDS=4` runs four sets of
games side by side on their own runs of checkpoints (copies 1..4*MDK2_N, ports +10 per set) and merges
the results: about 4 minutes (236 s) for all 96 with two games per set, against 35 min one at a time before.
2026-10-21: with `MDK2_N=3`, 1-1 .. 1-3 and 8-4 .. 8-5 all clean (every client bound every enemy, equal hp, no state differences).

Recorded progression: `tools/replay.sh record <name>` / `stop <name>` capture a route in a running
session; `tools/replay.sh run local/routes/<name>.route` replays its controls in a background
host with clients. It requires the recorded checkpoint transitions and endpoint, checks the
worlds after progression, and saves logs, state comparisons and failure screenshots to
`local/replays/`. See [routes/README.md](routes/README.md) for recording, tolerances and limits.
`tools/test_replay.sh` runs the parser, playback and failure checks without the game. Recorded
routes need real replay validation before being counted as campaign coverage.

Last runs (2026-10-20, Steam build 3902, level 1 cp 2): 21/21 on plain localhost (after the first
manual session's client crash was fixed); earlier 17/17 with `[sim] latency_ms=120 jitter_ms=40
loss_pct=5` on both sides. The enemy-attack and shot-replay checks depend on the Doganboy acting during
the window; in the lossy run it did not (0/0).

## License

MIT, see [LICENSE](LICENSE). Covers this repo's code only; MDK2 HD itself is not included.
