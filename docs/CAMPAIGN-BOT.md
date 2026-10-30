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
  as sniper mode's camera is not steered yet).
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
  (the owner shot the bay's turrets from the air) counts as flown. Not yet shown live: the runs of this round
  stalled at exactly these hatches (room 5 at 58 units, room 9 at 455).
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
- `tools/campaign_run.py` runs checkpoint-to-checkpoint segments in parallel (flight segments at the game's own speed: the tank is per frame), one pair of copies each.
- `tools/test_campaign_bot.py` checks the planner without the game (walking rules, landings,
  reachability, portal route).
- Ideas not yet built: record a person's
  playthrough with the route recorder and follow its positions closed-loop (robust to drift, unlike
  timed replay); decode the room meshes (`.mod`) for an offline navigation mesh; sniper aim through
  mouse look calibrated against the camera.

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

**Jetpack findings (flight work in progress).** A full tank is about 10.5 s of thrust at `timescale` 1 but
only about 5.5 s of game time at 2: fuel and climb are spent per frame, so flights are planned and tested
at x1. Only a refueller within a few units fills the tank (measured away from one: no refill). A hero that
lands on a ledge with the jump key still down does not take off again (the engine wants a new press): the
pilot's height hold now lets go of the key for a tick when thrust does not move him. The follower's thrust
estimate (`coop.jet_ms`) undercounts on a loaded machine (ticks are further apart than its 50 ms step); the
owner's 5 to 6 recording uses 9.6 s of a 10.5 s tank, so a follower that wastes thrust on hovering or
failed hops cannot repeat it on one fill.

## Status

Level 2 on the Overhaul build, God mode on, two players, the owner's recordings, flights at the game's own speed
(`campaign_run.py` forces `timescale` 1 for flight segments). Last full run (2026-10-31, with the fuel assist named
above on every segment): **1→2 (186 s), 3→4 (412 s) and 7→8 (155 s) pass**, and the worlds compared at their ends
differ in nothing. The others stop at:

| segment | reached | where it stops |
|---|---|---|
| 2→3 | 81 of 236 | room 1: the alarm `l2_alert03` sits behind the wall `l2_r1_90tun01`, which absorbs the aimed shots; the leader is sent back by the objective logic |
| 4→5 | 201 of 286 | both heroes land 6 units below the recording at (419, 998) and cannot get back; the worlds differ only in the flying mantas (46 and 53 units apart) |
| 5→6 | 47 of 115 | the bay at 46: its turrets were never shot (the gunner does not take turrets: all alive after 30 s); with fire held and the hero turned on each, three to four of four die in 45 s, but the climb that follows still fails at the hatch |
| 6→7 | 78 of 326 | the pad refill ran Max off the ledge (fixed after the run: `mount_refueller` uses the pilot), then the first hops; not rerun on the fix |
| 8→9 | 46 of 187 for the leader | the leader stopped 18 units short of point 46 with nothing logged; the other player followed all 187 points |
| 9→10 | 240 of 276 | past the lifts and pumps, then the hatches of the upper bays; the fixes for the bays came after the run |

Room 9's lifts are now ridden first try (see Moving lifts), a flight that was lost to them in earlier rounds.
Not done this round: the whole level chained in one run, the bot without a recording (autonomous), levels 3 to 12,
Doc's items, sniper locks, bosses, minigames, movies, combat damage to the hero. The one tank still cannot repeat
the person's flights without the assist.
