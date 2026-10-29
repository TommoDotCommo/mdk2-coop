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
  from under the open column's ceiling, and the bot picks up the jetpack and refuels first.
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

Survey of 21 segments of levels 1 to 3, two bots each, Overhaul build, God mode, game clock x2:
no Lua errors, no desync reports, no crashes, and ground enemies matched between host and client.

- **Flying enemies drift on clients.** Level 1's `flyer01`-`04` were 25 to 58 units from the host's
  positions and level 2's arena mantas 50 to 66, still apart 1.5 s later, also at normal game speed.
- **A puppet left behind.** On level 3 (checkpoint 2 to 3) the client showed the host's puppet 997
  units from where the host was.
- **A one-way face in level 2's arena** lets a hero cross north onto a strip by the north wall
  (walking, or blown by grenades) and not come back.

## Status

Shown live on the Overhaul build, level 2, God mode on (combat damage to the hero is not yet
managed): from checkpoint 3 the bot crossed the Gutter of Death on its own, breaking the four
scripted walls and the door lock, killing the enemies their deaths released, and walked the tunnels
to the Arena of Death entrance. Not yet: the remaining levels, pickups as objectives, Doc's items,
sniper locks, bosses, minigames, movies, co-op roles, data reports.
