# mdk2-coop: agent instructions

This mod is built with the Co-op Anything playbook, cloned at `~/source/coop-anything`
(upstream `cranebeaker/coop-anything`). Follow its `AGENTS.md`. These are the parts that
apply to every session here.

## Start of every session
1. `git -C ~/source/coop-anything pull --ff-only`
2. Check for others' work on this game:
   `gh issue list -R cranebeaker/coop-anything --search "MDK2" --state all` and
   `gh pr list -R cranebeaker/coop-anything --search "MDK2" --state all`. Read open MDK2 PRs.
3. `git worktree list` and `ListAgents`: if another session is working on this repo, work in
   your own worktree (`git worktree add ../mdk2-coop-<topic> -b <topic>`), never in a shared tree.
   Use separate game copies and ports for parallel game runs.

## While working
- Notes as you go: append to `~/source/coop-anything/games/mdk2-hd/NOTES.md` in the same
  session as the work, as a dated section (what was tried, what happened, evidence such as a
  sweep folder, commit hash or address). Keep observations, derived facts and guesses in
  separate labelled lists, and write `unknown` rather than guessing.
- No private data in NOTES.md or anything that may become a PR: no game files, no real names,
  no emails, and no paths with a username (`<game dir>`, `%LOCALAPPDATA%`).
- A lesson another co-op modder would want (a technique, a trap, a QA method) goes in
  `~/source/coop-anything/playbook/lessons.md` (numbered, with the game as evidence), or in the
  playbook page it belongs to.

## Giving back (end of a phase or a big fix, not only at the end)
- Prepare the contribution on branch `game/mdk2-hd` of the coop-anything clone: NOTES.md,
  `games/mdk2-hd/README.md` (from `templates/game.md`), the `data/directory.yaml` entry plus
  `python tools/build_directory.py`, and lessons.
- Publishing goes out under Tom's GitHub account. Show Tom the diff and get his OK before
  `gh issue create`, pushing to the fork or `gh pr create --draft`. PRs are always drafts, one
  game per PR, with the owner-review box left for Tom.

## Testing
Background mode only (`MDK2_BACKGROUND=1`, set by `tools/qa.sh` and `tools/sweep.sh`): games on the
2nd monitor, muted, and never taking focus. `mdk_kill` closes only the copies in MDK2_DIR1/2.
Full regression: `tools/sweep.sh` (96 checkpoints, about 55 min; run it in the background).
