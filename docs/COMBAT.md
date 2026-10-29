# Shared combat feedback

Enemy hits are confirmed by the host and shown to every connected player. The presentation is
glanceable rather than a log: in a fast fight a scrolling list is unreadable, so there is none.
What you see, all drawn from the same confirmed events and the same counters:

- **Floating damage numbers** above the enemy, larger than the old text, outlined and faded over
  under a second. Rapid hits by one player on one enemy (within 0.7 s of each other, for at most
  2.5 s) merge into a single growing number rather than a spray of small ones. The colour is the
  attacking player's slot colour (the same as the team panel, chat and pings); players on one
  enemy are offset side by side so their numbers do not overprint. Numbers of 25 or more use the
  larger font. A killing blow joins the number it finishes, which turns whiter with a thicker
  outline, a `!` and the enemy's name underneath.
- **Hitmarker**: a small cross of four diagonal strokes at the screen centre when the local
  player's hit is confirmed (0.17 s), larger, red and longer (0.42 s) for a kill.
- **Kill ticker**: at most three short lines under the damage panel, newest first, such as
  `Hank > Doganboy x3`. Kills of one enemy kind by one player in a row merge into `xN`; a line
  fades after six seconds. No ids appear anywhere.
- **Damage panel**: a compact corner panel on the right screen edge, at most 15 percent of the
  screen width and 20 percent of its height (positions are fractions of the window, text sizes
  follow the window height, so it scales with resolution; the mod has no separate UI scale).
  It is *collapsed by default* to one thin line, `DPS  You 30  Team 108`. Ctrl+M expands it:
  a `Damage` header with the fight timer, then one bar per player, longest first, with the
  player's name in their colour, a small damage total and a large DPS number. The bar is the
  player's share of the top player's fight damage. DPS is the fight average, which holds still
  after the last hit instead of falling to zero five seconds later; the five-second rolling
  figure stays in the dump. The panel shows from the first hit until the fight ends (eight
  seconds idle) and then fades out over two seconds; toggling it also shows it for three seconds
  between fights. Players beyond the rows that fit are not drawn (the dump has them all).

The positions of the panel and ticker avoid the team panel (top left), chat (bottom left), the
game's own health and ammo readouts and the mic and cutscene boxes (bottom strip), the centred
status line and the screen centre. The game's own HUD position is an assumption confirmed by
one screenshot of level 1, not read from the engine. Below about 1280x720 the panel's fixed 15
percent width gets narrow and long names are clipped; this is untested territory.

Enemy names come from a small table of enemy kinds read from the running game (Bif, Smite,
Grunt, Conehead, Hoser, Doganboy, Birdbrain, Turret, Bad Max, BFB, Ultra Dog, Fire Smite,
Poopsy, Cloak Grunt, Doom Sphere, Civilian); any other kind shows as `Enemy`. Weapon and
ability names are not shown: the damage hook exposes only a projectile type or a damage channel,
not the selected weapon, so any label would be a guess. Positions are target origins, rather
than collision contact points. Off-screen hits show in the panel and ticker but have no number.

Hold Ctrl with these keys (chat entry suppresses these shortcuts):

| Key | Action |
| --- | --- |
| M | Expand or collapse the damage panel |
| R | Reset local DPS/totals; preserve history and duplicate protection |
| D | Dump the bounded history and counters to `combat.txt` in the mod data folder |

Ctrl+PgUp, Ctrl+PgDn and Ctrl+L (history scrolling and meter paging) are removed with the feed.

The newest 512 events stay available until the level epoch changes. A dirty history also
writes `combat.txt` at most once per second, replacing the previous snapshot. Thus disk use
is bounded; this is a recent history, not an unlimited lifetime archive. The file contains
numeric event IDs, target IDs, source IDs, flags and world positions as well as counters; the
ids stay in the file and never on screen. Its header's `meter` field is 1 when the panel is
expanded (0 collapsed, the default); the scroll and page fields are kept as zero.
The Lua commands `coop_combat_dump()` and `coop_combat_reset()` provide the same actions.
A meter reset is local; it does not erase another player's counters.

A fight starts on the first confirmed enemy HP loss and ends after eight seconds without
another confirmed hit. The next hit starts fresh fight counters; reset totals remain
cumulative. Fight DPS freezes using the last hit's time after the idle timeout. Durations
below one second use a one-second denominator. Rolling DPS sums 100 ms buckets over five
seconds, with at most 100 ms quantisation at the boundary. Killing blows count remaining HP,
not overkill. Healing, friendly fire, blocked hits and environmental damage are excluded.

## Authority and wire format

The existing host applies enemy damage. A client's locally owned attacks report HIT; mirrored
projectiles and puppet attack replays are already suppressed by the damage hook. The source's
client reports its hit exactly once and the host runs the real enemy damage handler. Only
measured positive before/after HP loss becomes a new `EV_COMBAT` confirmation. The client's
report is a request, never proof of damage. No client changes its meter on prediction or APPLY.

HIT keeps its existing damage/type/body-part/name fields and adds: `netid` = host target
handle, `objtype` = source ID, `extra[1]` = target kind, `extra[3]` = source-local monotonic hit
ID, `pos` = locally observed target origin. The host stamps the attacker from the authenticated
link, resolves the target, requires a living sender puppet and target, and compares target ID,
kind and position with its world. This preserves existing non-enemy HIT interactions, which do
not enter the enemy telemetry ledger.

`EV_COMBAT` uses a 32-byte pointer-free payload in the event's 32-byte `name` field:

| Offset | Type | Meaning |
| --- | --- | --- |
| 0 | uint32 | Host confirmation ID, monotonically increasing, skipping zero |
| 4 | uint32 | Host enemy handle (target ID) |
| 8 | uint16 | Level epoch |
| 10 | uint16 | Actual HP lost |
| 12 | uint16 | Weapon/ability source ID |
| 14 | uint16 | Target OBJ kind (200..259) |
| 16 | uint8 | Attacker slot, zero based |
| 17 | uint8 | Flags: 1 kill, 2 crit (reserved for a proven crit source) |
| 18 | uint16 | Reserved, must be zero |
| 20 | float[3] | Host target world position before damage |

The existing reliable 128-byte envelope supplies ordering, retries and its own per-link ID.
The payload's ID survives fan-out, whose envelope IDs differ per recipient. This is the
existing little-endian Win32 protocol, not a new transport or a 32-byte UDP packet.
All participants must run the same mod version. Reconnects begin observing new events;
there is no replay of past combat history to newly joined players.

Source IDs 400..499 identify the actual projectile OBJ when the damage callback supplies a
projectile. For direct character callbacks, `2048 | damage_type` identifies the ability/damage
channel. The current hook exposes neither selected inventory weapon nor a reliable critical
hit bit. It therefore leaves crit unset and reports the channel instead of inventing a weapon
name. Exact inventory weapon attribution and crit detection remain unverified engine work.

Validation lives in the portable `combat_core.c` used by the DLL and T0 tests:

- Reports are stamped from their source link; confirmations are accepted only from host link 0.
- Envelope and payload epochs must match the currently loaded level.
- IDs and target IDs are nonzero; target kind, source ID, flags and reserved bits are bounded.
- Amounts outside 1..32767 are rejected before narrowing. Reports above 10000 are clamped
  before host difficulty scaling; confirmations retain the full measured HP loss.
- All coordinates must be finite and within ±1000000 units. Enemy reports must name the
  actual live target and kind, within 64 units per axis of its host position.
- A 64-ID replay window rejects repeats and stale IDs while admitting reordered deliveries
  and 32-bit wrap. Resetting the meter retains this guard; a level change clears it.
- Source-side reports, host acceptance and publication are limited to 64 events per attacker per one-second bucket.
  Confirmations received over a reliable link are not limited by arrival time: retries can
  deliver a burst after loss, and an arrival limiter would make clients disagree.

At saturation, remote hit requests are rejected; host-origin damage can still occur above the
telemetry publication limit. Such host hits are absent from every ledger. Existing transport
outbox overflow can also lose events. Saturation is a performance limit, not an exact audit
mode. Normal combat below these limits is checked against process HP state by the live suite.

## Verification

T0, without Windows execution or game files:

```sh
python3 tools/test_combat.py
python3 -B -m unittest discover -s tests -p '*_test.py'
make -C bridge
bash -n tools/qa_combat.sh
```

The native core/adapter harness runs with undefined-behaviour sanitisation. It checks guards,
reordered/duplicate/lost deliveries with retry, fight/rolling totals and history pooling. Every
validation protection has a mutation twin. The presentation core (`bridge/combat_hud.c`, no
engine dependencies) has its own tests, `tools/test_combat_hud.c`: merge rules for rapid hits
(same player and enemy, window, cap, kills), number life and style, hitmarker, kill ticker,
ranking, fade after the fight, number and time formats, friendly names without ids, and the
layout of the panel, ticker and hitmarker at 1280x720, 1920x1080, 2560x1440, 3840x2160 and
2560x1080 (21:9), collapsed and expanded, with every row and ticker count. The layout assertions
are the size limits (15 percent wide, 20 percent tall), containment, no overlap between the
panel, the ticker and the hitmarker, no overlap with the reserved zones above, disjoint columns,
and the worst-case strings fitting their boxes under a deliberately wide glyph model. 32 mutation
twins (no merge, merge across players or enemies, wrong colour, no cap, overlap, too wide,
too tall, wrong zone, overlapping columns, ...) must each fail an assertion, and the adapter
twins check that confirmations feed the numbers, that only the local hit raises the hitmarker,
that the toggle shows the panel and that a level change clears it. At runtime text is
measured and clipped with the actual HUD font. Rectangle checks do not prove real font
rendering or crowded team-panel and world-tag placement; `tools/combat_showcase.sh` saves
in-game screenshots (floating numbers, hitmarker, collapsed and expanded panel, ticker, the
fade) for a manual look, and nothing asserts on them.

A verifier with Windows interop and three configured disposable game copies should run, in
order (the normal QA runner obtains its configured resources):

```sh
python3 tools/qa_all.py features --gate combat-startup
python3 tools/qa_all.py features --gate combat
# Under a three-copy test allocation, after configuring the same copy variables:
MDK2_BACKGROUND=1 MUTANT=deaf bash tools/qa_combat.sh
# The command above MUST fail on the first all-process ledger comparison.
python3 tools/qa_all.py features --gate lossy-combat
python3 tools/qa_all.py features --gate hud --gate native-projectiles --gate level-epochs
```

The T1 `combat-startup` gate checks Lua registration, dump and reset state in one process.
The combat suite asserts a host hit of 20 HP and a client hit of 30 HP against the level-1
`doganboys07` fixture (default 100% dealt damage; level 1's `r2_dog01` is immune until its glass sequence), comparing actual host enemy HP differences,
per-player totals and complete event ledgers across all three processes, within one HP.
It then asserts five-second DPS expiry, unchanged idle totals, keyboard toggle edges (the panel starts collapsed, `meter` 0; Ctrl+M makes it 1 and again 0) and reset.
`coop_combat_test(1)` is a background-test-only deaf receiver mutant, inert in player mode.
The oracle has game-free tests proving missing events, wrong attribution, inflated counters,
missing participants, malformed dumps and nonfinite positions fail.

T1–T3 execution, actual glyph layout, projectile source attribution, target-origin marker
placement, large-party overlay coexistence, reconnects and host migration require live evidence.
No screenshots or log matches are used as the combat suite's correctness oracle.
