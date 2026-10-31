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
- **Doc's items and hands** (round 5; `coop.CheckpointItems`, `doc_hold`, `doc_use`, `doc_leak_puzzle`, `ladder_hop`).
  A launcher start at a Doc checkpoint of level 3 (6, 16, 7, 8, 9, 10, 11, 13, 14) now gives Doc what a player
  arrives with (`mdkDocGiveItem` once he stands in the level, in `coop.DocGrantTick`; the list is in
  `coop.CheckpointItems`, from the level script and the walkthroughs: at 6 the atomic toaster, duct tape and pipes;
  later the ladder, the empty fish bowl). The loaf is not an inventory item (`mdkDocGiveItem` does nothing for it,
  `mdkDocHasItem` stays 0): it comes in through a native pickup like the jetpack. `MDK2_CPITEMS=0` turns the whole
  grant off (the mutant: Doc then carries nothing and `campaign_bot.py dochands` fails). The items were read back from the game at every listed checkpoint (a checkpoint reload resets the inventory; the next grant follows).
  Hands (Modern controls; `coop_press_frames` holds each key for whole frames; measured: 24 hand changes with 3 frame presses and 12 with 150 ms presses all went through, 60 ms presses lost 1 press in 20): every hand has a list of the items
  Doc carries with an empty place in it (left: atomic toaster, tape, empty; right: pipes, empty, loaf, later the
  ladder), the inventory keys G / F move the selection along the list *without wrapping*, and the hand key
  (Q left, E right) puts the selected item in that hand. Round 4's "half the presses are lost" was this: a press by time did not move the selection when the list had ended; `doc_hold` turns round after two tries that change
  nothing. An item is used by pressing its hand key again (the tape at the pillar); fire combines the two hands
  (cord in the left, pipes in the right: the ladder). Proven live from a clean launcher start at 3:6
  (`dochands`: items carried, tape used, leak plugged, cord picked up, ladder made; the game's own state read back).
  The bot does the same inside `advance` on level 3 (`doc_leak_puzzle`), then uses the ladder at an anchor
  (`OBJ_LADDERSPOT`; `ladder_hop`: the hero climbs 7 units from the pillar's beam to the catwalk, at (-27.8, 0.1, -93.2)
  from `l3_hublad00` and (-31.5, 2.9, -96.0) from `l3_hublad01`). Not solved: the way on from the catwalk (a mesh duct
  at y -11 that the plane scan maps badly) to checkpoint 16, and everything after it (fish bowl, magnet, the
  spacewalk).
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
- **Flights are flown at their recorded height**: points in the air, points risen to with jump held (a
  jetpack hop onto room 9's first lift is only 2 units above the floor at its start, so it never counts
  as air) and the landing after them go to the pilot's height hold (`coop.PilotGo` with `fly=2`:
  thrust below the point's height, none above it, hovering over the point until level with it, also
  while the gunner has the aim). A flight that fails (the hero fell, or stopped making progress) is
  flown again from the last point walked, after refuelling at the nearest refueller in reach, at most
  three times; a lift ride that leaves the hero on the floor goes back to the way onto the lift.
- **Fuel**: the follower counts the jetpack's thrust (`coop.jet_ms`, reset at each refuel and at the
  start, where a checkpoint past a refueller fills the tank) and refuels first when a flight's climb up
  to its landing or the next refueller would run a part-used tank dry (about 10.5 s of thrust, some 15
  units a second of climb). Refuellers the recording used are used the same way: stood on, or, for the
  pumps hanging in room 9's shaft (the person tapped jump beside them), hovered by for six seconds.
- **Falls are not lift rides**: a stretch with no jump held counts as a ride only at a lift's pace
  (under 8 units a second); a fall down a shaft is several times faster.
- **Kurt's glides**: a drop the recording made with jump held is walked with the chute (jump held in
  the air); without Max's jetpack (level 2 before checkpoint 5) the recording's air is plain jumps.

**Recording a level for the bot** (`tools/record_level.sh <level> [checkpoint]`): leases one game copy
from the resource hook (like the test suites), installs the test build with the journal on, starts that
checkpoint in front with sound for a person to play, waits until the game is closed, keeps the journal
under `local/journal/<level>_<checkpoint>_<time>/` and turns it into `local/routes/seg_<level>_*.route`,
then releases the copy (also when interrupted). Set `MDK2_BUILD=overhaul` and `MDK2_OVERHAUL_EXE` to
record on the Overhaul build. Play on through as many checkpoints as you like; dying and reloading are
fine (only the last attempt at each segment is kept). `tools/record_level.sh --convert <folder> [out]`
only converts a journal folder.

The owner's shaft climb also changed the bot's own play: one tank does a whole shaft when each level
is landed on (the light's grate, then each closed hatch), thrust is cut once high enough and the hero
drifts across, and a bay's turrets are shot from the hatch with fire held (`bay_climb`: 344 s with a
refuel trip per bay, then 252 s on one tank). A checkpoint start past a refueller fills Max's jetpack
(`coop.FuelGrantTick`: a refueller hidden under the hero for six seconds), like the key items: from
checkpoint 9 an empty tank could not reach room 9's first lift.

- **Moving lifts** (level 2 room 9's `r9_platform*`): the platforms swing on their own, 21 units in a 20 second
  period, the neighbours in anti-phase, whatever the player does (read from the objects' heights in the game:
  `r9_platform` 239 to 260, `r9_platform_s` 280 to 259, the others likewise). The owner boarded each at the
  bottom of its swing; the follower used to arrive when it was overhead and hover under it. Now a flight that
  lands on a lift (`plan_boardings`) leaves the ground when the platform is coming down to the boarding height,
  holds 8 units out from its edge (not under it) for the last stretch until it is down, and keeps two units above
  the platform's current height as it rises (`coop.pilot.track`); the points between the last flown one and the
  first standing on the lift are flown too (`extend_to_lifts`: a hero let go early fell off the edge). A hop from
  one lift straight to the next is not waited for: the lifts are in step. Shown live: the lifts 0 to 5 are
  ridden first try (`local/dev1` traces: `rode to 257.1 / 277.3 / 294.8 / 318.1 / 337.0 (recorded 258.2 / 277.7 /
  295.7 / 318.9 / 338.3)`).
- **Carrying on**: the follower takes the earliest of the next 80 points that is nearly (1.5 units) as near as the
  nearest (`nearest_ahead`); the nearest alone sent a hero hovering at a pump to a later point at the same height
  that was not flown, and he let go of the jetpack and fell 200 units. Points the recording spent hovering by a
  hanging pump are flown too (`hover_points`).
- **Bays**: where the recording fired at the turrets of a bay (level 2's hatched shafts: rooms 5 and 9 climb through
  them), the follower stays, the gunner having the aim, until the turrets near it (`turrets_to_clear`: within 45
  units, up to 16 above or below, since a bay's turrets sit 13 above its floor) are dead or 30 s have passed. Without
  it the follower climbs on at once and meets a hatch that is still locked. Hovering with jump held after a climb
  (the owner shot the bay's turrets from the air) counts as flown. The gunner takes turrets when given their names (`coop.GunnerTargets`: a bay's
  turrets are scripted objects, not actors, so `coop_nearest_enemy` never lists them); the bay wait uses it. Untested live
  (the earlier fire-held loop it replaces passed 5→6).
  Shown live: 5→6 passes at game speed without the fuel assist (979 s).
- **Fuel assist** (`campaign_run.py --fuel-assist`, a test crutch, off by default and named in the report's mode):
  a refueller kept under each hero (`coop.fuel_assist`), so a recorded flight is not lost to one tank. Without it
  the follower spends about three times the person's thrust on room 9 (the person: 3.6 s of held jump up to the
  first pump; the follower hovers while it waits for lifts and spends a round trip with the game on every point),
  and runs dry at lift 2. Results are reported with and without it.

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
- `tools/campaign_run.py` runs checkpoint-to-checkpoint segments in parallel, (flight segments at the game's own speed: the tank is per frame), one pair of copies each.
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
up, take turns at objectives, revive each other, and each step ends with the worlds compared. The
co-op logic that needs no game is in `tools/campaign_coop.py`, checked by `tools/test_campaign_bot.py`.

- **Worlds compared after every step** (`campaign_bot.py squad`): each time the leader has gone 25
  units along the ground (`--step-units`), or every 45 s (a shaft climb), every game dumps its state
  and `statecheck.py` compares the host's world with each client's (again 1.5 s later on a mismatch).
  Lines are sorted by a tolerance table (`campaign_coop.KNOWN`): known differences (flying enemies
  drifting on clients) are listed with their reason, every other FAIL is a finding. Per step:
  `steps.json` and `squad.json` in the squad's evidence folder, and `steps` / `findings` in the
  segment's `results.json`. Mutant: `campaign_run.py --mutant desync` deletes a living enemy in the
  client's game only after the first step. Shown live (level 2, 5 to 6, x2): the next step reported
  `enemy 20003 sod101 (type 230, hp 150) deleted on the client` as a finding.
- **Transient differences** (`campaign_coop.settle`, run at the end of a squad): a health that differs on a
  client, or an enemy only on the client, is moved to `known` only if the next comparison no longer has
  that enemy; the last comparison and everything else stay findings (a deleted-on-the-client enemy, the
  desync mutant, is never tolerated). Evidence from every recorded squad run (7 runs, 156 findings):
  38 of 38 client-lower health lines (turrets, SAM missiles, a spawner's grunts) and 104 of 104
  client-only enemies (types 230, 201, 217, 511, 200) were gone at the next comparison; 8 findings are
  left, the mutant's and puppet-distance lines after a checkpoint reload (a hero moved 170 to 190 units
  while its puppet lagged one step).
- **Crashes fail a segment** (`campaign_run.py`): the segment keeps `tools/mdk.sh`'s crash guard
  (`tools/crashwatch.py`) from before the boot to the end. Every 10 s the Overhaul build's error
  dialog ("Jinkies!") is closed and recorded and the game processes are checked; a game gone ends the
  run. A new `dump%04i.dmp` of the segment's copies, a dialog, a `coop/logs/*.crash` marker or a gone
  process turns the result into `CRASH`, with the dump's path and exception, and the dumps are moved
  into the segment's `dumps/`. Mutant: `--mutant crash` crashes the last game (`coop_crash()`, test
  builds only). Shown live (level 2, 7 to 8): `CRASH` with the dump (access violation writing 0x10),
  the dialog, the marker and the gone process. A real crash was caught the same way in round 5 (3 to 4 without God mode, an
  access violation reading 0x8 on the client, `local/campaign/runs/20261031-122223/run2/2-3-4`); the
  other runs of the batch went on and their results were kept.
- **Without God mode** (`--no-god`): a hero below 35 health goes for the nearest health pickup
  (Double-A or car battery batteries) within 60 units, otherwise backs off along its way; a downed
  player is revived by the nearest standing teammate, who walks to the body and holds R (the mod's
  revive, `bridge/revive.c`, 3 s within 5 units, as `tools/qa_revive.sh` does); downs, revives, deaths
  and returns are counted from each game's own `coop_life()` (`results.json` `lives`, `care`). The
  gunner shoots the nearest enemy it sees, which is not necessarily the one attacking it. Shown live
  (level 2, 7 to 8, x1, three runs): all pass, nobody went down (lowest health 8, `care.retreats`
  0 to 2). Mutants: `--mutant down` puts a client down with a lethal hit after step 2 and the squad must
  revive it; shown live, player 1 walked to the body and revived it after 3.1 s of R
  (`local/live/dn78b_bot.log`); `--mutant down-stay` leaves it unrevived and the segment FAILs with
  `revive: player 2 down seen True, revived False, died True` (the 30 s bleed-out; `local/campaign/runs/20261030-234947`).
  `results.json` also has `bridge_lives`, the mod's own `life:` log lines counted per segment (downs,
  revives by a teammate, wipe reloads `new_level`, bled out), because the bot's life readings cannot tell a
  revive from a checkpoint reload after a wipe (`Lives` counts both as a revive).
  The down mutant was re-run after the care-loop fix: player 1 revived player 2 in 3.4 s of R, PASS
  (`local/campaign/runs/20261031-071759`); down-stay failed again as required
  (`.../20261031-072417`). The no-God series on other segments, at x1, two players:
  - 3 to 4 (recording), three runs: all fail. The SAM spawn and turret room shoots the heroes faster than
    the bot (which only backs off and fires aimed shots) can clear it. Run 1: 7 downs, 3 revived by a
    teammate, 4 wipe reloads, 595 s, `.../20261031-071820/run1`. Run 2: 12 downs, 4 revived by a
    teammate, 8 wipe reloads, 1 bled out, TIMEOUT at 1542 s, `.../run2`. Run 3: 5 downs, 3 bled out
    (the reviver arrived 0.8 units from the body but was itself downed first: nobody revives under fire),
    4 wipe reloads, 609 s, `.../run3`. The mod's wipe (`revive: everyone is down: reloading the
    checkpoint`) worked every time.
  - 8 to 9 (recording), three runs: all fail, the leader is lost on the route at point 120, 46 and 48 of
    187 with no downs in the first two (run 3: 2 downs, 1 bled out). The same segment with God mode at
    x1 passes (815 s, 0 findings, `.../20261031-081607`), so the cause is playing without God mode;
    which part of it (knockback, a blocked door, damage slowing the hero) is unknown.
  - 5 to 6 was not run: its recording and its own play fail at the tip with God mode too (flight code).
  - **Survival layer (round 4)**. The host's state dump says which enemy targets which player
    (`ai_target <id> <type> @pN`, the engine's own AI target); `campaign_coop.host_threats` turns it into
    threats per player, `pick_focus` picks what each hero shoots first (a missile within 25 units, else the
    weakest within reach; the Lua gunner takes it as `coop.GunnerFocus`), `hold` stands and finishes a
    shooter, `cover` walks to the nearest place no shooter sees (`coop.NavCover`, the engine's line of
    sight) and waits. The gunner now aims up and down (it had always aimed flat, so the turrets 20 to 40
    units above the hero in room 3 were never hit), and a gunner that fired at something far away kept
    the revive key from registering (the gunner is paused while reviving). A follower rewinds to the
    nearest earlier route point after a checkpoint reload (it pushed at walls 100 to 390 units short of
    a point it had already passed). `squad --tactic fight` is this layer; `--tactic run` sprints and
    shoots only what is within 6 units, plus missiles within 25.
  - Tactic result at x1, two players, no God mode, recording. **8 to 9: run passes, three runs of three**
    (146, 118 and 118 s of play, 0 downs, 0 deaths, 0 world findings;
    `local/campaign/runs/20261031-100949/run{1,2,3}/2-8-9`; run 1 before the missile focus, 2 and 3
    after). A first single run passed too (181 s, 1 down, 1 revive, `.../20261031-100541`). The fight
    tactic fails 8 to 9 (lost at point 45 under the room's spawner at 916, 940: grunts keep dropping on the
    hero, who stands and shoots; `.../20261031-093548`). So what stopped 8 to 9 without God mode was
    standing to fight a spawner's endless grunts; sprinting through it is how it is played.
  - **3 to 4 passes without God mode only by luck so far.** Room 3 is a gauntlet: three wall turrets,
    three SAM generators (homing missiles, 30 hp) and eight grunt spawn groups (97 hp each; the bot's
    bullets do 6). Fight tactic (`.../20261031-091932`: 5 downs, 4 revives, 0 deaths, 639 s;
    `.../20261031-093106`: 3 downs, lost at point 66): holding the ground kills about 35 grunts and
    still loses; cover does not help since nothing heals (room 3 has no health pickup; a revive restores health: 83 to 200 at
    the next reading). **Run tactic, round 5, x1, two players:** one run PASSED in 415 s
    (6 downs, 6 revives, 0 deaths, 244 of 244 points, `local/campaign/runs/20261031-121331`); then
    five more runs did not pass: one client CRASH at 773 s (see the findings), and with the next
    change, 0 of 3 in 1156 s (`.../20261031-124135/run{1,2,3}`: 25, 25 and 21 downs, 17, 13 and 13
    revived by a teammate, 8, 12 and 8 wipe reloads, 0 to 1 bled out). The squad is wiped between y 400
    and 590 (the second and third SAM generators); the run that passed had never stood and aimed at the
    first generator, and the failing runs had (34 to 36 s under missile fire at `l2r3samspawn01`, every
    round into the ceiling, the generator 40 units above the hero). The run tactic therefore no longer
    aims at a threat generator (`aimable`); that did not change the outcome (0 of 3), so the pass rate
    is low (one in six runs, roughly) and 3 to 4 is not counted as passing without God mode.
    **Three players pass it once** (run tactic, x1, `.../20261031-135938/2-3-4`): PASS at 891 s of a 900 s
    limit, 6 wipe reloads, 16 downs, 14 revives (13 by a teammate), 2 deaths (bled out), regroup checked;
    the 29 world findings are all type 217 grunt copies of `l2_r3spawn07_spa` 27 to 46 units apart on the
    clients (the fall in the findings). One run, so only a hint that a third player's revives help.
    What would help and is not done: wait out the missiles behind the corner at y 330 to 390 (the
    generators only spawn once a hero is in sight, not measured), kill the generators from a place that
    sees them (`kill_objective` does not find one from the floor), or a third player (its revives).
  - **7 to 8 passes without God mode, three of three** (fight tactic, x1, two players, recording,
    after the gunner pitch change; `local/campaign/runs/20261031-130622/run{1,2,3}`): 184, 209 and 163 s
    of wall clock, downs 0, 1 and 0, revives 0, 1 and 0, deaths 0; the care loop took cover twice in each
    run and held for 8 to 10 shooters. One finding in run 1 (`host's player is 36.6 from its puppet in
    the client's game`, one comparison, gone at the next). With God mode the same segment passes in 114 s,
    0 findings (`.../20261031-130133`), so the pitch change did not regress it.
  - **5 to 6** fails with God mode too (x1, `.../20261031-133047`: the flight failed three times at
    points 27, 47 and 55, the jetpack hops, the flight code's job), so the no-God series was not run.
  - **Three players, 8 to 9, no God mode, run tactic** (x1): first run FAILED in 402 s
    (`.../20261031-132324`): player 2 followed all 187 points; the host (leader) went down at 6 hp
    beside the body it was reviving and bled out unseen (the bot's care loop cannot read a game whose
    lock the reviving thread holds, so nobody was sent: `revive()` now publishes the reviver's own state
    and gives up when it is down itself); player 3 bled out after three revive tries at 0.0 units from
    its body; 2 downs, 1 revive, 2 deaths. The second run, with the fix, reached the end with 4 downs, 4
    revives and 0 deaths in 389 s, worlds equal (0 fail, 160 enemies), and FAILED the regroup: player 2's
    follower thread had died of an `IndexError` after a wipe reload (the traceback was not logged then; it
    is now, and the follower falls back to chasing the leader), leaving it 73 units from the goal
    (`.../20261031-133614/run1`).
    Then three more runs with the traceback logging, all PASS (`.../20261031-134806/run{1,2,3}`:
    185, 163 and 244 s wall, downs 0, 0 and 2, revives 0, 0 and 2, deaths 0, regroup in 3, 4 and 3 s,
    0 world findings): three players on 8 to 9 pass 4 of 5 runs since the reviver fix, and the one
    `IndexError` has not recurred (it came after a wipe reload; none of these runs had one).
- **Roles** (`--roles`): when a step has several objectives at the goal's height (level 2, 3 to 4: eight
  alerts, a door lock and walls; 5 to 6: two generators), the leader keeps its route and the next
  players each take another objective (aimed fire), then everyone regroups within 15 units of the
  goal (`regroup_s`). With 3 or more players the squad always waits for the regroup. Shown live on
  3 to 4 at x1: three players (PASS, 470 s, 26 steps, followers 9.6 and 9.8 units from the leader at the
  end; `local/campaign/runs/20261031-065804/2-3-4`) and four players (PASS, 513 s, 24 steps, followers
  13.5, 13.2 and 13.2 units; `local/campaign/runs/20261031-070742/2-3-4`). Roles taken: player 2
  `l2_alert09`, player 3 `l2_alert08`, player 4 `l2_r3doorlock`. The alert objectives are not always
  killed by aimed fire (a tunnel wall is in the line), which is the bot's aim, not co-op.
  The regroup is a check: a roles or 3+ player segment whose players are not all within 15 units of the
  goal after 60 s fails with `regroup: the players did not all reach the goal in 60 s`. It caught a bug
  in the bot on its first live run (four players, the leader's route ended 17.7 units short of the portal
  and only the followers were sent; `local/campaign/runs/20261031-081843`); with the leader sent too,
  four players regrouped (followers 0.5 to 1.1 units from the leader, PASS, 458 s,
  `local/campaign/runs/20261031-083108`). Three players again with the final code: PASS, 477 s, regroup
  checked, `world_ok` true (`local/campaign/runs/20261031-084429`).
- `--explore` plays a segment with the bot's own play instead of its recording; `--timescale 1`
  confirms a finding at normal speed; `--repeat N` runs each segment N times. Each segment's
  `results.json` lists every copy's build hash and `[test] save_dir` (every copy keeps its own saves).

## Findings so far (co-op, from the bot's world comparisons)

Whole-campaign survey: 84 segments (every launcher checkpoint to the next), two bots each, 168 game
instances, Overhaul build, God mode, game clock x2. No Lua errors and no desync reports. Mismatches
found by comparing the host's world with the client's at the end of each segment (and again 1.5 s
later):

- **Flying enemies drift on clients**, on four levels: mantas up to 231 units from the host's
  positions (levels 2, 4, 8), level 1's flyers up to 53, level 8's ship turret 50.
- **An enemy dead only on the client**: level 4's `l4_r5grunt` with 60 hp on the host and 0 on the
  client (two segments). A replication gap, fixed: the host had brought the grunt back to 60 hp
  without a hit and clients were only told hp in SPAWN and in the APPLY of a hit. The host now sends
  an hp-only APPLY when an enemy's hp differs from what clients were told (`tools/qa_hpsync.sh`).
- **Pickup counts differ** on the final levels (11 and 12): 7 against 4, 3 against 2.
- **A puppet left behind**: on level 3 the client showed the host's puppet 997 units from the host.
- **A client crash at a checkpoint reload** (round 5, level 4 checkpoint 5 to 6, two runs out of three): the
  client dies at `mdk2hd.exe+0x2a95` reading address 8 while it binds the host's enemies after the host's
  checkpoint reload that follows a hero's death in the vortex. The same signature as the level 8 crash above,
  which makes it the reload path, not a level. The third run reloaded the checkpoint 12 times (11 deaths) and did
  not crash, so it is intermittent (about one reload in a dozen or more).
- **A host crash after a destroyed scenery piece** (round 5, 4:11 to 14): missed sniper rounds took
  `l4_r7a_90tun01` from 100 to 0 hp and the host crashed on the next frame (`+0x570bd` reading 0x64). The bot
  stops aiming at a piece after three misses on it (`note_collateral`).
- **Item use by a script is not carried to the other player** (`OnItemUsed`, unaudited in
  `tools/script_events.txt`): the cord that level 3's leak puzzle spawns, and the drained water, exist only in the
  game of the Doc who used the tape (`FAIL item obj1: 1 on host, 0 on client`).
- **God mode does not survive a checkpoint reload** (a hit of 20 after `mdkNewGame` took 60 hp to 40; `God(1)` put
  it back). Every squad run since the start played on without it after the first death; the squad now sets it
  again when a hero comes back.
- **Line of sight is one-way** for scripted locks: `coop.NavView` takes a ray in either direction, and a lock
  such as level 4's `l4_r7lock` is clear from the lock outward to 568 of 588 cells of the ledge and from none of
  them inward. A viewpoint search for a projectile must test inward rays only.
- **A client crash** on level 8 (checkpoint 6 to 7): access violation at `mdk2hd.exe+0x2a95` reading
  address 8, while binding the host's enemies after the checkpoint load (last bound: a type 511
  generator); the same load gave the checkpoint's atomic jetpack. Not reproduced in four 5 minute
  reruns, two with the grant and two without (`MDK2_CPITEMS=0`); the original came 9 minutes in, so it
  is intermittent or needs longer play.
- **A grunt's copy falls on clients** (found without God mode, round 5): the type 217 enemies that a
  generator's spawner makes (`l2_r3spawn07_spa`, `l2_r8gen03_spawn`) stand at one height on the host
  and sink on the clients while keeping their x and y. 3 to 4: z -13.5 on the host, -67.2 on the client,
  four grunts, the gap growing 35.5, 40.2, 42.1 in the statecheck lines of the last three comparisons
  (`local/campaign/runs/20261031-121331/2-3-4/*/end_1.txt`, `end_2.txt`). 8 to 9, three players: z 211.0
  on the host, 93.1 on client 2 and 146.0 on client 3 (different ages of the same fall)
  (`local/campaign/runs/20261031-132324/2-8-9/*/step14_{1,2,3}.txt`). The host's grunts were idle
  (targeting a downed hero). Not fixed; the comparison reports it as a finding, not as known.
- **The client crash at `mdk2hd.exe+0x2a95` again** (reading address 8), now with a context: the client
  (3 to 4, no God mode, x1, about 12 minutes in, 14 downs and 8 wipe reloads before it) crashed 40 ms
  after `ent despawn ... died: running its OnDie` for two enemies and `replay launch type 423 ... replay
  fire type 423 target '@p2' (found) -> created` (a homing missile launched at the player). The bot
  reported `CRASH` with the marker, the Jinkies dialog, the dump and the gone process. Evidence:
  `local/evidence/crash-r5/` (flight record `...flight-0-crash.txt`, `dump0000.dmp`, the segment's
  `results.json`), `local/campaign/runs/20261031-122223/run2/2-3-4`. The older level 8 crash has the
  same address and fault; both are a null pointer in the engine (eax 0). Unresolved.
- **A one-sided face in level 2's arena** blocks the engine's line of sight going south (not north);
  heroes walk across it both ways, so this is only a quirk for line-of-sight users (enemy AI included).

**Jetpack findings (flight work in progress).** A full tank is about 10.5 s of thrust at `timescale` 1 but
only about 5.5 s of game time at 2: fuel and climb are spent per frame, so flights are planned and tested
at x1. Only a refueller within a few units fills the tank (measured away from one: no refill). A hero that
lands on a ledge with the jump key still down does not take off again (the engine wants a new press): the
pilot's height hold now lets go of the key for a tick when thrust does not move him. The follower's thrust
estimate (`coop.jet_ms`) undercounts on a loaded machine (ticks are further apart than its 50 ms step); the
owner's 5 to 6 recording uses 9.6 s of a 10.5 s tank, so a follower that wastes thrust on hovering or
failed hops cannot repeat it on one fill.

Seen by the per-step comparison of this round (level 2, 3 to 4, 3 and 4 players with roles, x1):

- **Enemy health and extra enemies on clients** seen in single steps of these runs (a turret or SAM spawn's hp
  lower on every client; type 201 enemies only on clients) were dump skew, explained below.
- **Revive under fire fails** (no-God 3 to 4, run 3): the reviver reached the body but was downed before
  the 3 s of R, and the downed player bled out. The mod works as designed; this is what a player sees.
- No world difference lasted to a segment's end in any 3 or 4 player run besides the known flying-enemy
  drift (mantas 17 to 36 units apart on the clients at the end of the last 4 player run).

### Level 2 (3 to 4) per-step mismatches: dump skew, not co-op bugs

The bot's per-step comparison reported `type 201` / `217` enemies "on the client, gone on the host" and
turrets / SAM spawns with a lower hp on the clients (host 14 against 0; 34 against 16, 16, 4). Cause,
from the bridge logs of runs 20261031-065804, -081843, -083108 and -084429: the games dump one after
another (host first, then each client, hundreds of ms apart), and a fight or a spawner moves the host's world in
between.
- hp: client 4's own log in run -083108 holds the host's hp sequence for the SAM spawn exactly
  (44, 38, 34, 28, 24, 18, 14, 8, 4, 0); the host dumped at 14, the clients' dumps came 2 to 3 hits later (8, 8, 0), after the retry's host dump too. The client is
  always the lower one because it dumps last (client <= host in all 27 per-step hp findings of the 2026-10-31 runs).
- extra enemy: the host's `ent spawn` line comes after its own dump in all of -065804 (`e003d`), -081843 (`a0407`),
  -083108 (`c040d`); the clients bound it before their dump. In -084429 (`103fc`) the host's dump read `ent 103fc gone`
  (corpse removed, DESPAWN sent at the next tick, 100 ms later) while the client's dump came before the DESPAWN arrived.
- fix: `statecheck.py --after` takes a second host dump written once the clients' are; a client value only fails
  outside what the host held in between (hp between the two, position on the path, enemy made or removed in between).
  `campaign_bot.compare_worlds` dumps the host again after the clients. A client stuck at 0 while the host holds 60 still fails.

## Status

Level 2 on the Overhaul build, God mode on, two players, the owner's recordings, flights at the game's own speed
(`campaign_run.py` forces `timescale` 1 for flight segments). Passing, worlds compared at the end with no difference:
**1→2 (186 s), 3→4 (412 s), 7→8 (155 s)** (fuel assist on in that run) and **5→6 (979 s, no fuel assist, after the bay fix:
the turrets of each bay are shot with fire held, the hero turned on each; the climb used the tank, refuel trips and
all)**. The others stop at:

| segment | reached | where it stops |
|---|---|---|
| 2→3 | 81 of 236 | room 1: the alarm `l2_alert03` sits behind the wall `l2_r1_90tun01`, which absorbs the aimed shots, and the objective kills walked the hero 80 units off the recording. Since: aimed fire gives up at point-blank without a hit, and a hero far from the next point walks back along the scanned floor (`off_route`). Not run live since. |
| 4→5 | 166 and 90 of 286 (round 4) | an aimed kill of room 4's generators takes a hero down into their pit (6 below the floor); in the run of 2026-10-31 10:00 one hero was stuck there each time (the other passed). Since: `off_route` also counts a hero more than 4 below the point (back along the scanned floor), `climb_out` flies a jetpack hero up. Not run live since; the worlds differ only in the flying mantas. |
| 6→7 | 78 of 326 | the pad refill ran Max off the ledge (fixed after that run: `mount_refueller` uses the pilot); the run after the fix is in the report |
| 8→9 | passes (225 s, 2 players, no assist, 0 world differences, gap 3.3) | the host had stood at (922, 942) shooting what room 8's generator `l2_r8gen00` (a spawner, so not an objective) spawned without end: the gunner holds the pilot while it fires. The gunner now rests 6 s after 10 s of fire while a recording is followed (`coop.gunner_cap`). |
| 9→10 | 240 of 276, twice | past the lifts and pumps, then the upper bays (their turret fights came in the fix that made 5→6 pass); the hero ends at the floor of the shaft |

The last full table of the level 2 branch (x1, two players, God on, shards 2; its tip as merged): 1→2 (120 s),
7→8 (130 s), 8→9 (329 s) and 5→6 (1294 s) pass; 4→5 failed (the follower ended in the generator pit), 6→7 and 9→10
stalled and were stopped, 2→3 had the leader at 236 of 236 points but the follower stopped at the closed door
`l2_r2entrdoor` after the lock was killed on the client (gap 58.6, a possible co-op finding: the door's open
state on the client; not confirmed).

Room 9's lifts are ridden first try (see Moving lifts), a flight that was lost to them in earlier rounds. The bay
wait runs only at points walked (the hero standing on the bay's floor); stretches the recording fires on while
flown (room 9's bays) skip it, which is where 9→10 ends. Not done: the whole level chained in one run, the bot
without a recording (autonomous), levels 3 to 12, Doc's items, sniper locks, bosses, minigames, movies, combat
damage to the hero.

This file is the one integrated status of the worker branches (flight and chaining, levels 3 and 4,
co-op checks, flying-enemy drift, the second host dump and the hp-only APPLY fix, item-use replication, the
AI-target guard, no-God tactics, Doc's items and hands, the level 2 flight and lift work). Evidence paths
below are under `local/` (ignored), from each branch's own runs; the integrated tree has been checked with
the game-free gates and a short live smoke (see the last section).

### Without God mode, and the crash status (rounds 5)

- **No God, two players:** 7→8 passes 3 of 3 (the fight tactic: 184, 209 and 163 s, downs 0/1/0, no deaths).
  8→9 passes with the run tactic (3 of 3). 3→4 passed once in six runs: the gauntlet's missiles (SAM spawner
  generators the follower stood under) wipe the squad, and the spawner-aim fix did not change that (0 of 3).
- **No God, three players:** 8→9 passes 4 of 5 (the one failure was a follower thread's `IndexError` after a
  wipe reload, now logged with a traceback and a chase fallback; the rerun is 3 of 3); 3→4 passed once (891 s).
- **Crashes are failures** (`campaign_run.py` reports a `dump%04i.dmp`, the Jinkies dialog and a non-zero exit).
  Every sighting so far is the client at `mdk2hd.exe+0x2a95` reading address 8, within seconds of an enemy
  bind after a checkpoint reload or a despawn: level 8 once, 3→4 no-God once, level 4 5→6 twice. The cause is
  an AI actor whose target gob has no info record. The bridge now guards that read (`e9e9935`) and the crash
  log names the gob; `tools/qa_ai_target_guard.sh` plants such a target: 8 of 8 pass with the guard, and the
  `noguard` mutant crashes with the same registers as the sightings. A 40 minute reload loop without the guard
  (`tools/reload_loop.sh`, 92 reloads on 2→9) did not reproduce it, so the natural trigger is another actor type;
  the guard is proven on the planted case only, not on a natural one. A host crash at `+0x570bd` (a tunnel piece
  shot to pieces) is bot-side and fixed by the collateral limit.
- **Item use:** Doc's item use is replicated to the other games (`EV_ITEMUSED`, `tools/qa_itemused.sh`, with a
  mutant); see `docs/RELEASE-NOTES-item-use.md`.

### Round 4 (levels 3 and 4, elevators, camera-aimed sniper rounds)

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
routes. Round 4 added elevators (3:5→6 passes with `--assist lift` in 92 s, both players riding; that lift is locked
behind Kermit's puzzle) and camera-aimed sniper rounds (level 4's tunnel plug is broken that way; 4:2→3 passes). Level 3's
walkway and everything after it need Doc's items (a launcher start at checkpoint 6 gives him none), and level 4's fan
and pit locks (`l4_blowlock02`, `l4_r7lock`) are not seen from anything the bot walks to. Not yet: levels 5 to 12,
Doc's items, bosses, minigames, movies, combat damage to the hero.

### Round 5 (Doc's items and hands, ladder anchors)

Doc's checkpoint inventory, frame-exact keys, Doc's hands and ladder anchors are built and proven live on
level 3's walkway puzzle (see "Doc's items and hands" above): a launcher start at 3:6 gives Doc his items,
the bot plugs the leak, picks up the cord and makes the ladder by pressing keys, and climbs with it; a start
without the grant (`MDK2_CPITEMS=0`) fails the same check. No new segment passes: 3:6 to 16 stops after the ladder
(the anchors beyond are reached from the walkway above, and the catwalk's mesh duct is mapped badly), 4:5 to 6
crashed twice (client, checkpoint reload, finding above), 4:7 to 8 stalls at the arena door and 4:11 to 14 at a
generator no reachable place sees (and once crashed the host after a tunnel piece was shot to pieces; fixed on
the bot's side). The remaining level 3 and 4 stretches need a person's recording, listed in the run's report.

### Integrated state (merged branches)

**What exists**
- The bot, the planner and the pilot/follower (see the sections above), `tools/campaign_run.py`
  (parallel segments, `--chain` to play consecutive segments in one pair of games, `--timeout`,
  `--build overhaul`, `--explore` (alias `--no-routes`), `--no-god`, `--roles`, `--timescale`,
  `--repeat`, `--mutant desync|crash|down|down-stay`, `CAMPAIGN_IDLE`), crash fail-through
  (`tools/crashwatch.py`), `report.md` per run, `tools/record_level.sh` (record a person's play of a
  level), `tools/campaign_coop.py` (per-step world comparison, lives, care, roles), and
  `tools/qa_flyers.sh` with `tools/flyers_check.py` (the flying-enemy gate, with its `MUTANT=attached`).
- The client no longer keeps mirrored flying enemies on their parent's bone (see
  `docs/RELEASE-NOTES-flying-enemies.md`): the "flying enemies drift on clients" finding below
  predates that fix. `campaign_coop.KNOWN` still lists flyer drift as known, so a regression of the
  fix would not be reported by the bot's comparison; `tools/qa_flyers.sh` is the gate for it.

**Proven live (God mode, Overhaul build)**
- Level 2, following the owner's recordings, two-player squads: 1→2, 3→4, 5→6 (55 s), 7→8 and 8→9
  pass; 4→5 is traversed (worlds differ only by flyer drift, measured before the drift fix).
  2→3, 6→7 and 9→10 fail on stretches the person flew or glided. The pilot's height hold (`fly=2`)
  hovers within about 1.5 units of a height. The follower has not passed 6→7 or 9→10 with it: in
  room 9 the one tank given at the checkpoint start runs out on failed hops between the lifts, and
  no jetpack fuel readout is found yet. A later change (the hold lets go of the jump key when
  thrust does not move the hero, a ledge landing needs a new press) is from the branch that found
  the 5→6 stall.
- Level 2, the bot's own play: 3→4, 5→6 (252 s), 7→8 and 8→9. At the co-op branch's tip the owner's
  recording of 5→6 fails (both players stop under the shaft) and the bot's own 5→6 failed at x1
  (`local/campaign/runs/20261030-233117/run1/2-5-6`).
- Levels 3 and 4 on the bot's own, third run (full table in the run's `report.md`; two players, God on):
  passing are 3:1→2, 3:2→3, 3:3→4 (the plane scan), 3:4→5 (timing dependent), 3:10→11 (trivial),
  4:1→2, 4:2→3 (362 s: the tunnel plug sniped, the tunnel walked, the 175 unit shaft dropped with the
  chute held, the room's generator killed), 4:3→4, 4:6→7 (131 s), 4:8→9 (1069 s, needs a 1200 s
  limit) and 4:9→10. Worlds matched at the end of every one (L3 13 enemies, L4 48 to 53 compared).
  Held up by something the bot does not do yet: 3:5→6 (the elevator at (17,34) is not entered),
  3:6→16 to 3:9→10 (walkway, slide, Doc's items, the spacewalk; the plane scan only gives a rough
  map there), 4:4→5 (the neck is passed, then the platforms: the lock chain l4_blowlock02 → blower02
  is sniped from the pit and misses), 4:5→6 (the vortex: the three spheres are hit, the fans lift
  Kurt 17 units at a time, the target is 138 higher), 4:7→8 (the exit door of the arena does not
  open), 4:10→11 (the pit's lock hit without a draft) and 4:11→14 (a generator no reachable place
  sees). The owner's level 3 journals hold no play (the hero stands still), so they give no routes.
- Co-op: the per-step comparison and its desync mutant (level 2, 5→6, x2), the crash watch and its
  crash mutant (level 2, 7→8), the no-God care on 7→8 (three runs at x1) and the revive with the
  `down` / `down-stay` mutants (`local/campaign/runs/20261030-234947`). The care loop keeps each
  game's last reading, so a busy leader no longer stops a revive.
- Flying enemies on clients: `tools/qa_flyers.sh` measured at most 97.0 units of drift with the old
  behaviour (mutant `attached`) and at most 3.3 with the fix (91.8 and 9.0 at x2).

- Roles with three and four players (level 2, 3→4, x1, Overhaul build): both PASS with the regroup
  checked. 3→4 was used because 5→6 does not pass; it has eight objectives at the goal's height
  (alerts, a door lock, walls). The no-God series fails on 3→4 (the bot cannot yet survive room 3's
  turrets) and 8→9 (the route is lost without God mode, reason unknown); 5→6 still fails with God.
- Fixed on the way: the care loop could not read the leader's position while the leader's pilot held
  the command file, so a downed player got no reviver for 30 s and bled out (it keeps each game's last
  reading); the regroup did not send the leader; `world_ok` at a segment's end counted known
  flying-enemy drift as a difference.
- World comparison against a second host dump (`statecheck.py --after`, taken once the clients' are
  written): hp, position and spawn differences that the host's own world passed through in between are
  dump skew, not co-op bugs; `tools/qa_hpsync.sh` (with its `nosync` mutant) gates the hp-only APPLY fix
  for the silent host hp refill. See the per-step mismatches section above.

**Not yet**
- The no-God series on 3→4 passed once in six runs (the gauntlet's missiles wipe the squad; see the Co-op
  section) and was not run on 5→6 (5→6 does not pass with God mode: the flight code). 7→8 (3 of 3) and 8→9
  (run tactic, 3 of 3 with two players) pass without God mode; three players on 8→9 passed 4 of 5 (the regroup
  failed once on a follower thread's `IndexError`, traceback now logged).
- Levels 5 to 12 beyond the survey, Doc's items, sniper locks that need long chains, bosses,
  minigames, movies, combat damage to the hero.

**Integrated-tree smoke (second merge round, Overhaul build, x1, God on, two players)**
- `campaign_run.py --level 2 --segments 3-4`: PASS in 401 s (244 of 244 points, worlds equal, no findings, no crash).
- `tools/qa_hpsync.sh`: 4 passed (players 405 units apart); with `MUTANT=nosync` it fails (client keeps hp 97
  against the host's 33 and 61).
- `tools/qa_flyers.sh`: 3 passed, flyer drift at most 5.5 units.
- Game-free: `tools/test_campaign_bot.py`, `tools/test_lua31.sh`, `tests/flyers_check_test.py` and
  `qa_all.py offline` pass.

- **The `mdk2hd.exe+0x2a95` crash is the bot's own line of sight (round 6).** The faulting function is the sight
  check `0x402a40`; the dump of the no-God 3 to 4 crash has the Lua binding `mdkAILineOfSight` (`+0x3ac10`, return
  address `+0x3ac96`) as its caller, not the AI target getter. Its actor is the hero `bob` and its target the gob
  `l2r2_aa1sign__5` (a level sign, 3 units from the hero, game object pointer null). The only Lua callers are the
  bot's: the pilot's probe (`coop.PilotSees`), the nav probes and the gunner. The probe is most likely a stale one (not yet seen in play): it lives in a Lua global that
  outlives a level load; the engine reuses the freed address for a level gob, which `coop_alive` accepts because it
  only looks for the pointer in the gob table. `coop.ProbeOk` now checks the probe's name and game object
  (`coop_gob_ok`) and `coop.Los` refuses gobs without a game object. `tools/qa_stale_probe.sh` reproduces the fault
  on demand (mutant `unchecked`: crash at `+0x2a95`, same stack) and shows the fix. The AI target guard
  (`ai_target_ready`) was aimed at a different path; it is harmless but was not this crash.
