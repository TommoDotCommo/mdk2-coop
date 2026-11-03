# Doc's leaf blower works from the client (next release)

- **A client playing Doc can blow enemies away.** Doc's leaf blower is a push volume the game builds
  on Doc's own body, so only the game of the player blowing it had one; the enemy it pushed was a
  copy of the host's, which the host never moved, so the enemy snapped back (or stayed in the wrong
  place for the other players for the rest of the level). The client now tells the host where its game
  left an enemy near Doc, and the host moves the real enemy there, so the enemy ends in the same
  place in every game. Hosts blowing was already right.
- Checked by `tools/qa_items.sh`, on the Overhaul build with two games and the other player 348 units
  away: the host's and the client's parachute (canopy opens, the descent is about 2 units a second
  against 27 in free fall, the other game's copy matches), Max's jetpack (a lift of 15 units, the other
  game's copy rises in the flight pose) and Doc's leaf blower (the enemy is blown 17 to 24 units, in
  both games, and ends in the same place). With the old behaviour (`MUTANT=noblowsync`) the client's
  blow fails those checks.
- Not changed: the other players still do not see the jetpack's flame or the blower item on your
  character (only the pose); the equipment visuals are a separate piece of work.
