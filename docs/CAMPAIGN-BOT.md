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
- **Crashes fail a segment** (`campaign_run.py`): the segment keeps `tools/mdk.sh`'s crash guard
  (`tools/crashwatch.py`) from before the boot to the end. Every 10 s the Overhaul build's error
  dialog ("Jinkies!") is closed and recorded and the game processes are checked; a game gone ends the
  run. A new `dump%04i.dmp` of the segment's copies, a dialog, a `coop/logs/*.crash` marker or a gone
  process turns the result into `CRASH`, with the dump's path and exception, and the dumps are moved
  into the segment's `dumps/`. Mutant: `--mutant crash` crashes the last game (`coop_crash()`, test
  builds only). Shown live (level 2, 7 to 8): `CRASH` with the dump (access violation writing 0x10),
  the dialog, the marker and the gone process.
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
- **Roles** (`--roles`): when a step has several objectives at the goal's height (level 2, 5 to 6: two
  generators), the leader keeps its route and the next player takes another objective (aimed fire),
  then everyone regroups within 15 units of the goal (`regroup_s`). With 3 or more players the squad
  always waits for the regroup. Not yet shown live.
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

**Jetpack findings (flight work in progress).** A full tank is about 10.5 s of thrust at `timescale` 1 but
only about 5.5 s of game time at 2: fuel and climb are spent per frame, so flights are planned and tested
at x1. Only a refueller within a few units fills the tank (measured away from one: no refill). A hero that
lands on a ledge with the jump key still down does not take off again (the engine wants a new press): the
pilot's height hold now lets go of the key for a tick when thrust does not move him. The follower's thrust
estimate (`coop.jet_ms`) undercounts on a loaded machine (ticks are further apart than its 50 ms step); the
owner's 5 to 6 recording uses 9.6 s of a 10.5 s tank, so a follower that wastes thrust on hovering or
failed hops cannot repeat it on one fill.

## Status

This file is the integrated status of four worker branches (flight and chaining, levels 3 and 4,
co-op checks, flying-enemy drift) merged onto the campaign-bot branch. Evidence paths below are
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
- Levels 3 and 4 on the bot's own, second run (eight segments): 3:2→3 and 4:9→10 pass; 3:1→2,
  4:1→2 (the window broken) and 4:3→4 passed in an earlier partial run. 4:5→6 rides the vortex fan
  (a lift of 218 units) and then falls out; 4:8→9 got to 61 units from its checkpoint when the
  time ran out; 4:2→3 now has blind chute drops, not yet run live; 4:4→5 and 4:10→11 are held up
  by fans and sniper locks whose chains are long. The fan ride and the switch-on chain openers are
  built and unit-tested, not yet live. Most of level 3 needs Doc's items, the fish minigame or the
  spacewalk, and level 4 needs the theatre's '?' doors, the stage dance and bouncing sniper shells,
  so these need a person's recording; the owner's level 3 journals hold no play.
- Co-op: the per-step comparison and its desync mutant (level 2, 5→6, x2), the crash watch and its
  crash mutant (level 2, 7→8), the no-God care on 7→8 (three runs at x1) and the revive with the
  `down` / `down-stay` mutants (`local/campaign/runs/20261030-234947`). The care loop keeps each
  game's last reading, so a busy leader no longer stops a revive.
- Flying enemies on clients: `tools/qa_flyers.sh` measured at most 97.0 units of drift with the old
  behaviour (mutant `attached`) and at most 3.3 with the fix (91.8 and 9.0 at x2).

**Not yet**
- `--roles` live; 3 and 4 player runs; the no-God series on 5→6.
- Levels 5 to 12 beyond the survey, Doc's items, sniper locks that need long chains, bosses,
  minigames, movies, combat damage to the hero.

