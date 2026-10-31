# Attached visuals of the other players (next release)

- **You see the other players' parachute, jetpack and leaf blower.** Each is a
  separate object the game keeps on the character's body, and a copy of a remote
  player (a puppet) ran none of the code that makes them:
  - **Max's jetpack** is made when Max picks the jetpack up (a model attached to
    his torso). A puppet never picked one up, so other players saw Max without a
    jetpack and without its flame (the flame is the jetpack model's own thrust
    animation). The jetpack is now built on the puppet with the game's own
    calls and follows the owner's animation, idle and thrusting.
  - **Doc's leaf blower** is made when Doc takes it up (the item on his back and
    the puffs of air it throws). It is now built on the puppet the same way and
    blows when the owner's does. Its push stays the host's, as before.
  - **Kurt's parachute** exists from the start, but the game keeps it out of
    sight with the same room bit it uses for everything in a room nobody is
    awake in. On a puppet that bit stayed set whenever the puppet was away from
    the room the puppet was created in, so Kurt's canopy opened without being drawn.
    The canopy's shown bit is now carried from the owner and applied after the
    game's room pass, every frame.
- Players need the same version: the player and relay packet magics changed
  (`MDKV`, `MDKW`) because every snapshot now carries the three visuals.
- Checked by `tools/qa_attached.sh`: three peers on the player's (Overhaul)
  build, each in turn the owner, the players at least 300 units apart; each peer
  reads the visual from its own gob table (alive, on the owner's body and bone,
  shown, in the owner's animation, gone when the owner's goes). Before the
  change the jetpack and the blower did not exist on the other peers
  (`MUTANT=gear` reproduces that) and the peers' copy of an open canopy carried
  the game's hidden bit when the owner was in another room (`MUTANT=shown`
  reproduces that); after it they follow the owner. This is process state, not
  a picture: the hidden bit is what the game reads to skip drawing a gob, and
  the bridge writes the value after the game's room pass and before the draw.
