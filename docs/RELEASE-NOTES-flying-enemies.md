# Flying enemies on clients (next release)

- **Flying enemies stay where the host has them.** The level 2 mantas (and other
  flyers registered the same way) sit on a bone of an animated scenery model, a
  flock flying its loop. While a game object is attached
  to a bone the engine moves it back onto that bone in every scene update, over
  the position the co-op snapshots set, and a client's copy never runs the AI that
  takes the host's flyers off the flock. A client's mantas therefore flew the
  flock's circuit while the host's attacked, up to 230 units apart, so shots,
  hits and targeting on the client used the wrong positions. A client now takes
  its mirrored enemies off the bone and places them by the host's snapshots alone.
  `tools/qa_flyers.sh` (level 2 checkpoint 4, 20 s, the client far from the host)
  measured at most 97.0 units of drift with the old behaviour (`MUTANT=attached`)
  and at most 3.3 with the fix (91.8 and 9.0 at twice the game speed).
