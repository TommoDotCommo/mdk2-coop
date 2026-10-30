# Campaign bot

A bot that plays the campaign with ordinary controls in N co-op game copies, so a full playthrough,
and the co-op behaviour along it, can be tested without a person. Work in progress: what exists and
what has been shown live is listed under [Status](#status).

## Where the knowledge of "how to play" comes from

Web walkthroughs for MDK2 are incomplete (single levels) or refuse automated reading, so the
authoritative source is the installed game itself:

- **Level scripts** (`levelN.lua`, `lN.lua` in the game archive): rooms with bounding boxes, doors,
  checkpoints with positions, pickups, enemy spawners, scripted blockers (`OBJ_DESTRUCTABLE` with
  `OnDie` handlers that break a wall or a lock), Doc's item combinations, boss state machines and the
  level exits. `tools/campaign_mine.py` turns them into a model per level under `local/campaign/`
  (nothing from the game is stored in the repository).
- **The engine's own collision**, asked through line of sight between two invisible probes
  (`coop.NavStart` in `lua/coop_dev.lua`): a flood fill from the hero finds each cell's floor by a
  vertical search and links cells whose way is clear at chest height. A room is mapped in seconds
  (level 2 room 3: 6,700 cells in 4 s), and multi-level floors come out right because the fill
  follows the floor it stands on.
- **A human walkthrough**, when available, only to cross-check the order of objectives.

## How it plays

- **Pilot** (`coop.PilotGo`): the hero faces its next point (an aim, like a mouse turn) and the
  forward key is held; the game's physics does the walking. A point above a step gets a jump.
- **Planner** (`tools/campaign_bot.py`): A* over the scanned cells. A rise of up to 0.7 per 2-unit
  cell is walkable (about 20 degrees: Max slid back down a 33 degree gutter wall); steeper rises are
  jumps, allowed only onto a cell flat enough to stand on.
- **Advance** (`campaign_bot.py advance`): the coarse route is the level model's room portals; each
  portal is reached by scanning, walking to the reachable cell nearest it and scanning again. With no
  progress the bot pushes on toward the target (doors open as the hero comes near), then fires along
  its way until a nearby scripted destructible is gone (a wall's registered position is its pivot,
  not its face), and scans again.
- **Objectives**: when stuck, the nearest living thing whose death the level waits for (an `OnDie`
  handler: generators, locks, alarms, walls) within 150 units is shot from a reachable place that sees
  it (`coop.NavView`; a line of sight to an object's centre ends in its own body, so heights on it are
  tried). Portals the hero is already past are skipped.
- **Scan artefact**: the hero's own body blocks the probes, so links next to where it stands are not
  tested (a scan in level 2's arena had seen a cage around the hero).
- **Script chains** (`campaign_mine.py` `deps`): for each scripted object, what its handlers open
  (door animations, door locks released, stasis left, deletions), what they enable (`ANIM_ENABLED`) and
  which damage types they check. When stuck, the bot kills the live prerequisites of an opener near it
  (level 1 room 4: three enemies enable the lock) and triggers the opener with its damage type
  (`DAMAGE_SNIPER`: a sniper round, aimed through the camera, see below). Openers that switch something on near the way
  (level 4's sniper locks that raise platforms or start fans) count as well; each is set off once,
  the nearest first, and one enabled by standing somewhere (`OnCollision`) is never shot at.
- **Pickups are not objectives**: an item with an `OnDie` handler (level 3's BFB) is carried, not
  shot; it had filled every stall of level 3. On Kurt's levels an objective the chaingun does not
  break gets sniper rounds.
- **Moved by the level**: when a script carries the hero off the planned room route (level 3 starts
  with Doc moved 165 units after the intro), the route is planned again from where he is.
- **Sniper rounds through the camera** (`sniper_aim`): sniper mode is switched on (the left trigger of a faked
  pad, `coop_pad_fake`; in it the camera sits at the hero's eye), then the camera is turned by mouse look (the
  right stick) until its own orientation (`coop_pose` of `mdkGetCameraGob()`) points at the target, and the
  fire trigger is pressed. `coop.Look` holds the stick for an exact number of frames, so a turn does not
  depend on how soon the driver's next command arrives (a half stick turns 0.43 degrees a frame, a full one
  2.83; pitch 0.17 / 0.70 / 2.05). The loop measures again after each turn and fires inside 0.8 degrees; the
  hit comes from the bridge's damage log. Rounds that hit something else (no line of sight from here) are given
  up after three, and the old launched round (`coop_test_sniper`, along the hero's facing) is the fallback
  (`--shortcut-sniper` uses it alone). Shown live: level 4's tunnel plug `l4_r1shatter` is broken with the
  camera aim. A lock the level watches (`omAnimIsPlaying(lock, ANIM_ENABLED)`) is still only hit from where
  the line of sight reaches it: level 4's fan locks hang under a ledge and are seen from the air or from the far
  side of the pit, not from anything the scan walks to (the grid of seeing points for `l4_blowlock02` was all
  at y > -26, the ledge at y -61).
- **Elevators** (`take_lift`; `campaign_mine.py` `lifts` from `Level.InitElevDoor(top, bottom, mover, cab, ...)`):
  when the goal is a floor or more away in height and an elevator's far end is at the goal's height, near the
  goal's column, the bot walks to the door at its own height, steps into the cab (the door opens inside 10
  units while the mover is not locked, the cab ride starts on touching `EL_CENTER`), waits until the height
  has been steady a few seconds, and plans again from the new place. Followers ride it too. A lift the level
  keeps locked behind a puzzle (level 3's walkway lift, `l3_elev00`, opens after Kermit has eaten the aliens
  and been given the toaster) is left alone, or opened with `--assist lift` (`Level.UnlockElevDoor`: a script
  shortcut, named in the run's results as `assist`). The follower's own copy had the lift locked as well, so
  the unlock is not carried by the host's puzzle in this test.
- **Doc's items** (not built; what was found): a launcher start at level 3's checkpoint 6 gives Doc an empty
  inventory (`mdkDocHasItem` false for tape, pipes, cord), so the walkway's puzzle (tape at the crack, cord,
  ladder at the ladder spots) cannot be played from it; `mdkDocGiveItem(doc, OBJ_x, 1)` fills it. Items are
  chosen by the hand keys (`Q`/`E` in the Modern preset: each press moves one step along that hand's list,
  empty, item, item...) and used or combined with the fire button, but a press every so often is lost (about
  half of 200 ms presses changed anything), so a frame-exact press (`coop.Look`'s way) is the next step.
- **Kurt's fans** (level 4's updrafts, `fan_ride`): when the way on is well above Kurt and a fan
  (`OBJ_BLOWERCYLINDER`) stands below it, the bot snipes the lock that switches the fan on (a mined
  `deps` opener whose handler `enables` it), walks to the column's edge, steps in with jump held so
  the chute opens in the draft, keeps over the column's centre while rising and steers to the target
  once above it.
- **Kurt's chute and Max's jetpack**: drop links to landings far below (jump held in the air opens the
  chute); a fuelled jetpack lifts Max 38 units in 3 s of held jump, so fly links reach up to 34 units
  from under the open column's ceiling, and the bot picks up the jetpack and refuels first. Only a
  refueller fills the tank (it sits on a step: the bot walks at it with a jump); a tank burns for about
  10.5 s, some 15 units a second of climb once moving. `coop_gob_dump` / `coop_gob_float` (test
  builds) dump a gob's memory for finding state like this; the fuel is not in Max's own gob.
- **Hatched shafts** (`bay_climb`; level 2 rooms 5 to 6, 118 units): each bay's floor is a proximity
  hatch that opens when Max comes within about 10 units of its centre, and a bay's upper hatch stays
  locked until its four wall turrets are dead (`Level.SetUpDoorAndEnemies` / `Level.AddEnemy`, mined
  generically as `locks`: a helper that locks its argument and one that gives its argument an OnDie
  event). One tank cannot both climb and hover through the turret fights, so the bot makes a trip per
  bay: refuel, burn up the open ring and then the centre column, drift 16 units off-centre so the
  hatch shuts under him, land, shoot the turrets, walk back to the centre to drop through the hatches,
  refuel. With every hatch unlocked one burn reaches the top (first pass: 342 s, 12 turrets).
- **Kurt's blind drops** (`blind_drop`): when the way on lies far below and the scan shows no landing (an
  object the line of sight misses, or beyond 300 units), the bot walks to the rims nearest the target
  (reachable cells with an open side) and off them toward it with jump held, then scans again from the
  landing. A rim the hero cannot reach (a wall in the way) is skipped, so the wall is broken instead. The
  push toward the target holds the chute too: without it the hero walked off level 4's tunnel into a
  175 unit shaft and died of the fall, in God mode.
- **Curbs**: a step of up to 2 units onto a sloping cell that leads on to level ground counts as a jump
  (`Nav.lip`; level 4's tunnel mouth, 58.9 then 60.6 then 61.7, was unreachable for the planner, and so
  was the neck on the way to the fans), while a steady 40 degree slope is still not walked.
- **Sniper locks the level watches itself** (`sniper_locks`): on Kurt's levels, locks with no handler in
  the model (level 4's rising spheres under the vortex) get one sniper round each when the way on is above.
- **A scan that finds almost nothing** (level 3's rooms have floors the line of sight does not see, so a
  floor search from the hero's neighbours finds none) is repeated as a plane scan: the hero's height is
  the floor and the walls are found by the chest-height probes (`coop.NavStart(..., plane)`, at most 6000
  cells). It gives a rough map (20 cells became 98 on the walkway), not a reliable one.
- **Notes the game could not move**: `coop_note` writes `note_<key>.tmp` and moves it over the `.txt`;
  the move fails while the bot has the `.txt` open, leaving the newer text in the `.tmp` for good. A finished
  scan then looked 'running' for the whole 15 minute scan timeout. `read_note` takes the newer of the two.
- **Gunner** (`coop.GunnerOn`): the nearest enemy the hero can see is faced (with pitch) and shot;
  a target that loses no health in 3 s is left alone (out of reach, or line of sight through a gap the
  bullets do not pass).

## Learn once, follow fast

`campaign_bot.py follow --route R` plays a recording (`tools/replay.sh record`, by a person or made
while the bot explores) closed-loop: it walks the recording's positions with the pilot, skipping on
when the hero is already nearer a later point, presses the action keys (jump for chute and jetpack,
fire, use) that were held on each stretch, and fires along the way where the recording was firing
when held up. Unlike replaying inputs by the clock it does not drift. Shown live: a 13 minute
recording of the bot exploring level 2's gutter (walls broken, enemies fought) followed in 245 s on a
fresh game. A recording that never reached a later checkpoint is kept by the recorder as
`<name>.route.partial`, which the follower reads too.

**A person's play, hands-free** (`MDK2_JOURNAL=1` at install, `[test] journal=1`): the bridge records
every frame of the session in the route format to `coop/journal/L<level>_cp<cp>_<time>.route`, a new
file at each checkpoint the game reports. Pausing, menus and dying do not stop it.
`campaign_bot.py journal <folder>` turns the files into `local/routes/seg_<level>_<from>_<to>.route`,
the routes `campaign_run.py` follows. The game does not report every checkpoint passed (level 2 played
from checkpoint 6 to its end came as one file), so a file is cut where the hero first comes within
12 units of each next checkpoint's position, and only the last attempt (after a death's jump back) is
kept.

What following a person's play needs beyond positions, learnt from the owner's level 2:
- **Lift rides** (room 9's rising platforms): stretches where the hero stood still while its height
  changed, with no jump held, are waited out once for the whole ride, giving up when the height stops
  changing.
- **Refuellers**: a point on a refueller is mounted, as the person stood there to fill up.
- **Shooting**: where the recording fired, the nearest living scripted objective within 50 units is
  aimed at (a generator the person finished off gates the arena's exit); when held up 25 s, the bot's
  own objective killing runs once from that spot (door locks).
- **Glides and drops**: below a stretch the person flew or glided, the hero flies back up from a
  shaft's open centre line (at most three times per point).
- **Every player plays the recording** in a squad, rather than chasing the leader on foot (Max's
  flights cannot be chased).

The owner's shaft climb also changed the bot's own play: one tank does a whole shaft when each level
is landed on (the light's grate, then each closed hatch), thrust is cut once high enough and the hero
drifts across, and a bay's turrets are shot from the hatch with fire held (`bay_climb`: 344 s with a
refuel trip per bay, then 252 s on one tank). A checkpoint start past a refueller fills Max's jetpack
(`coop.FuelGrantTick`: a refueller hidden under the hero for six seconds), like the key items: from
checkpoint 9 an empty tank could not reach room 9's first lift.

## Speed

- Navigation is scanned, not explored by walking, and scans are cached per checkpoint.
- Segments (checkpoint to checkpoint) are independent and run in parallel on the lab's slots.
- A segment the bot has solved can be kept as a recorded route (`tools/replay.sh`) for fast
  deterministic regression runs; the bot is only needed again when a route stops passing.
- `tools/test_lua31.sh` checks `coop_dev.lua` with the real Lua 3.1 compiler, so a Lua 4 construct
  fails in a second instead of as a silently unloaded `coop.lua` after a game boot.
- `[test] timescale k` (`MDK2_TIMESCALE`): a background game's clocks run k times as fast and its
  frame cap rises k times, so each frame advances the world by the same step. Level 2 room 3 plays
  normally at k = 2 (walls broken, enemies fought, doors opened).
- Background test games read `command.lua` every other frame (the driver's round trip), and a
  viewpoint search is one call in the game rather than one per candidate.
- `tools/campaign_run.py` runs checkpoint-to-checkpoint segments in parallel, one pair of copies each.
  A crash fails a segment (`tools/crashwatch.py` through `mdk.sh`'s guard: a new `dump%04i.dmp`, a
  `coop/logs/*.crash` marker or the error dialog, which is closed and recorded every 20 s);
  `--build overhaul` refuses copies of another build; `--timeout` caps each segment; each run writes
  `report.md` with the build, the copies' save folder, where the leader stopped and its last words.
- `tools/test_campaign_bot.py` checks the planner without the game (walking rules, landings,
  reachability, portal route).
- Ideas not yet built: decode the room meshes (`.mod`) for an offline navigation mesh; viewpoints for locks that
  only the air sees (jump off a rim with the chute and fire while falling); frame-exact key presses for Doc's
  hands.

## Co-op

The goal is N copies where every player is a bot: a leader advances and the others follow, split
up, take turns at objectives, revive each other, and each step ends with the worlds compared
(`statecheck.py`, the desync check). Not built yet.

## Findings so far (co-op, from the bot's world comparisons)

Whole-campaign survey: 84 segments (every launcher checkpoint to the next), two bots each, 168 game
instances, Overhaul build, God mode, game clock x2. No Lua errors and no desync reports. Mismatches
found by comparing the host's world with the client's at the end of each segment (and again 1.5 s
later):

- **Flying enemies drift on clients**, on four levels: mantas up to 231 units from the host's
  positions (levels 2, 4, 8), level 1's flyers up to 53, level 8's ship turret 50.
- **An enemy dead only on the client**: level 4's `l4_r5grunt` with 60 hp on the host and 0 on the
  client (two segments).
- **Pickup counts differ** on the final levels (11 and 12): 7 against 4, 3 against 2.
- **A puppet left behind**: on level 3 the client showed the host's puppet 997 units from the host.
- **A client crash** on level 8 (checkpoint 6 to 7): access violation at `mdk2hd.exe+0x2a95` reading
  address 8, while binding the host's enemies after the checkpoint load (last bound: a type 511
  generator); the same load gave the checkpoint's atomic jetpack. Not reproduced in four 5 minute
  reruns, two with the grant and two without (`MDK2_CPITEMS=0`); the original came 9 minutes in, so it
  is intermittent or needs longer play.
- **A one-sided face in level 2's arena** blocks the engine's line of sight going south (not north);
  heroes walk across it both ways, so this is only a quirk for line-of-sight users (enemy AI included).

## Status

Level 2 on the Overhaul build, God mode on. The owner played all of level 2 with the journal on, and
the bot follows those routes in two-player squads: 1→2, 3→4, 5→6 (55 s), 7→8 and 8→9 pass, and 4→5 is
traversed (worlds differ only by the known flying-enemy drift). 2→3, 6→7 and 9→10 still fail on
stretches the person flew or glided; flying those airborne points at their recorded height is the
newest change and is not yet tested. On its own, without a recording, the bot passes 3→4, 5→6
(252 s), 7→8 and 8→9.

Levels 3 and 4 on the bot's own, third run (full table in the run's `report.md`; Overhaul build, two players,
God on): passing are 3:1→2, 3:2→3, 3:3→4 (the plane scan), 3:4→5 (timing dependent), 3:10→11 (trivial),
4:1→2, 4:2→3 (362 s: the tunnel plug sniped, the tunnel walked, the 175 unit shaft dropped with the chute
held, the room's generator killed), 4:3→4, 4:6→7 (131 s), 4:8→9 (1069 s, needs a 1200 s limit) and 4:9→10.
Worlds matched at the end of every one of them (L3 13 enemies, L4 48 to 53 compared). Held up by something
the bot does not do yet: 3:5→6 (the elevator at (17,34) is not entered), 3:6→16 to 3:9→10 (walkway, slide,
Doc's items, the spacewalk; the plane scan only gives a rough map there), 4:4→5 (the neck is passed, then the
platforms: the lock chain l4_blowlock02 → blower02 is sniped from the pit and misses), 4:5→6 (the vortex:
the three spheres are hit, the fans lift Kurt 17 units at a time, the target is 138 higher), 4:7→8 (the exit
door of the arena does not open), 4:10→11 (the pit's lock hit without a draft) and 4:11→14 (a generator no
reachable place sees). The owner's level 3 journals hold no play (the hero stands still), so they give no
routes. Not yet: levels 5 to 12, Doc's items, bosses, minigames,
movies, combat damage to the hero.
