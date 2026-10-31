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
  (`DAMAGE_SNIPER`: a sniper round; known shortcut: launched along Kurt's aim by `coop_test_sniper`,
  as sniper mode's camera is not steered yet). Openers that switch something on near the way
  (level 4's sniper locks that raise platforms or start fans) count as well; each is set off once,
  the nearest first, and one enabled by standing somewhere (`OnCollision`) is never shot at.
- **Pickups are not objectives**: an item with an `OnDie` handler (level 3's BFB) is carried, not
  shot; it had filled every stall of level 3. On Kurt's levels an objective the chaingun does not
  break gets sniper rounds.
- **Moved by the level**: when a script carries the hero off the planned room route (level 3 starts
  with Doc moved 165 units after the intro), the route is planned again from where he is.
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
- Ideas not yet built: record a person's
  playthrough with the route recorder and follow its positions closed-loop (robust to drift, unlike
  timed replay); decode the room meshes (`.mod`) for an offline navigation mesh; sniper aim through
  mouse look calibrated against the camera.

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

This file is the integrated status of the worker branches (flight and chaining, levels 3 and 4,
co-op checks, flying-enemy drift, the second host dump and the hp-only APPLY fix) merged onto the campaign-bot branch. Evidence paths below are
under `local/` (ignored), from each branch's own runs; the integrated tree has been checked with
the game-free gates and a short live smoke (see the last section).

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
  (run tactic, 3 of 3 with two players) pass without God mode; three players on 8→9 passed 1 of 2 (the regroup
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
