# Item uses reach every player (next release)

- **What one player does with an item now moves the level's puzzle for everyone.** A room's
  `OnItemUsed` script ran only in the game of the player who used the item, and the script
  state it changes is never synced. On level 3 that made Doc's leak unplayable in co-op:
  when one player used the duct tape at the pillar, their game plugged the leak and made
  the cord item, while the other players' games kept the leak open and had no cord, so
  their puzzle state diverged for good. The game that handles the use now tells the
  others (a new reliable message, which the host also keeps for players who join later),
  and their copy of the room's handler runs with the user's puppet as the player; this
  covers the leak (`l3_w_hub00`) and the bay's fishbowl (`l3_bay`). The airlock's handler
  (`l3_air00`) dresses the user's own body in boots and a fishbowl, so it stays local
  until it is made safe for a puppet. `tools/qa_itemused.sh` (level 3 checkpoint 6, the
  client on the bridge room over 300 units from the host) uses the tape on the host: with
  the fix the client's water flag becomes 1 and both state dumps hold the same cord item;
  with the old behaviour (`MUTANT=noitemused`) the client's flag stays 0 and the cord
  exists on the host only. The same script checks that a door lock a client shoots dead
  (level 2, room 2's exit door) frees the door in both games: that already worked,
  so the bot's "door stayed shut for the client" report was not a replication gap there.
