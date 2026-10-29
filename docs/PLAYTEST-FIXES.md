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

The reported animation-dispatch crash has not been reproduced. Repeated
room-8 split/combat saves at 60 and 165 FPS completed without it. This update
captures fault registers, readable stack words and the specific animation
arguments at the known dispatcher fault sites. It does not claim to fix that
crash. `make -C bridge test-crash-context` checks the decoder and its mutants.

A separate resume defect was reproduced on the prior build: native attack tasks
lost their function reference after an instant load because the original native
function was kept only inside the co-op table. It now also has a named global
alias which the game's save resolver can find. `tools/qa_save_scripts.sh` checks
both clients' restored Poopsy and Bif tasks and rejects the missing-alias mutant.
