# Attached visuals of the other players (next release)

- **You see the other players' parachute, jetpack and leaf blower.** Each is a
  separate object the game keeps on the character's body, and a copy of a remote
  player (a puppet) ran none of the code that makes them:
  - **Max's jetpack** is made when Max picks the jetpack up (a model attached to
    his torso). A puppet never picked one up, so other players saw Max without a
    jetpack. The jetpack is now built on the puppet with the game's own calls and
    follows the owner's animation, idle and thrusting.
  - **The jetpack's flame comes on with the thrust.** The flame is two parts of
    the jetpack model that Max's own thrust code shows and hides; on a puppet,
    which never runs that code, they followed up to a second late, so a short
    burst showed almost no flame. Which parts of the item are shown is now carried
    from the owner: every peer's flame changes within about 100 ms of the owner's.
  - **Doc's leaf blower** is made when Doc takes it up (the item on his back and
    the puffs of air it throws). It is now built on the puppet the same way and
    blows when the owner's does. Its push stays the host's, as before.
  - **Kurt's parachute** exists from the start, but its copy on another player's
    screen carried the game's hidden bit (the bit the game also uses for things in a
    room nobody is awake in) while the canopy was open on its owner, when the two
    players were in different rooms. The canopy's shown bit is now carried from
    the owner and written every frame, after the game's room pass and before the
    draw.
- Players need the same version: the player and relay packet magics changed
  (`MDKV`, `MDKW`) because every snapshot now carries the three visuals.
- Checked by `tools/qa_attached.sh`: three peers on the player's (Overhaul)
  build, each in turn the owner, the players at least 300 units apart; each peer
  reads the visual from its own gob table (alive, on the owner's body and bone,
  shown, in the owner's animation, its model's parts shown as on the owner's side
  and changed within 200 ms of the owner's, on the pose of the bone the owner's
  game puts it on, gone when the owner's goes). Before the
  change the jetpack and the blower did not exist on the other peers
  (`MUTANT=gear` reproduces that) and the peers' copy of an open canopy carried
  the game's hidden bit when the owner was in another room (`MUTANT=shown`
  reproduces that); after it they follow the owner. `MUTANT=subs` (the flame
  left to the copied animation) and `MUTANT=gearbone` (the item on another bone)
  must fail too. This is process state, not
  a picture: the hidden bit is what the game reads to skip drawing a gob, and
  the bridge writes the value after the game's room pass and before the draw.
