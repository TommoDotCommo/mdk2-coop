# Enemy hp on clients (next release)

- **A client follows an enemy's hp when the game changes it without a hit.** The host
  told clients an enemy's hp only when it spawned and in the message of each hit its
  damage handler applied. The game also changes hp by itself (a downed grunt that stands
  up again at 60, a script that tops up a turret), and a client kept the old number until
  the next hit; a mirror left at 0 hp counts as a corpse to the game's own checks, so a
  client could see an enemy the host was still fighting as dead. The host now compares
  each enemy's hp with what clients were told, on every enemy update, and sends an
  hp-only message (no damage, no OnDamage script on the client) when they differ.
  `tools/qa_hpsync.sh` (level 2 checkpoint 4, the client far from the host) changes one
  enemy's hp on the host only: with the fix the client holds 33 and then 61 within
  3 s; with the old behaviour (`MUTANT=nosync`) it stays at 97.
