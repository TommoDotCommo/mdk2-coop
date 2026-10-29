# Progression and crash-diagnostic follow-up

The level 2 entrance to the ship room used a single-player animation callback to
lock the previous room's exit as soon as the leading player arrived. A teammate
could remain behind that gate even after the leader returned.

Co-op now defers this specific rear lock until the group has crossed. A downed
teammate remains reachable for revival. The host commits the lock and retains its
reliable result for clients requesting world history. The pending and completed
states survive an instant save; solo play keeps the original one-way behavior.

`tools/qa_rear_gate.sh` checks the actual engine lock bits on both peers, leader
return, authority and retained history. `LEADER=2` exercises a client leading;
`MUTANT=earlylock` restores the premature callback and must fail. Optional
`SAVE_RELOAD=1` exercises an instant save while the lock is deferred. Position
setup uses teleports: full physical traversal is an additional validation step.

`tools/qa_hans.sh` exercises the level 1 final boss's authored task, native
projectile travel, client damage and target-scope restoration with the client as
the only eligible target. This passed on both supported builds. Removing the
script scope or target selection is rejected. No boss AI change is included.

The reported animation-dispatch crash was not reproduced by repeated room-8
split/combat saves at 60 and 165 FPS. The fault registers, readable stack words and
animation arguments captured at the known dispatcher fault sites were kept, and
`make -C bridge test-crash-context` checks the decoder and its mutants.

Later analysis of the crash dumps (see [Crashes fail the run](TESTING.md#crashes-fail-the-run))
found two causes, both reproduced and covered by `tools/qa_animguard.sh`:

- At the dispatcher's fault site the "index" is a pointer to the 38th state of the
  gob's animation controller. The game's animation update stores flagged or weighted
  states in two 20 entry stack arrays, and its limit check lets a 21st entry overwrite
  the first index. Reproduced by making 30 states live on one gob; the bridge now stops
  surplus states at the update's entry. What makes 21 states live in real play is not
  established: 8 rounds of movement, jumping and firing by two players left at most two.
- The bridge asked every gob for every animation id. A model without an animation table
  (the level 2 exits) made the game's lookup read address `<id>`: an access violation at
  the lookup, and on the Overhaul build an error dialog and a dump. Every lookup is guarded.

Two other dumps came from test commands that moved a player gob which did not exist yet
(the title screen); the game's own position setter does not check for a null gob.

A separate resume defect was reproduced on the prior build: native attack tasks
lost their function reference after an instant load because the original native
function was kept only inside the co-op table. It now also has a named global
alias which the game's save resolver can find. `tools/qa_save_scripts.sh` checks
both clients' restored Poopsy and Bif tasks and rejects the missing-alias mutant.
