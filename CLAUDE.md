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
- Publishing goes out under the owner's GitHub account. Show the owner the diff and get their OK before
  `gh issue create`, `gh pr create --draft`, or anything new on GitHub. PRs are always drafts, one
  game per PR, with the owner-review box left for the owner.
- Pre-approved by the owner (2026-10-21), no need to ask: pushing commits to this repo
  (`git push origin master:main`) and adding commits to the open PR #95 (`git push fork game/mdk2-hd`),
  provided every pushed commit has the identity below and no real names, emails, tokens, game files
  or usernamed paths (check `git log --format='%an %ae %cn %ce' origin/main..master` and the diff).
  Never force-push, and never `push --all` / `--mirror`.
- Match the hub's conventions: look at recent `gh issue list` / `gh pr list` titles first. Claims are
  titled "Claim: <Game> (<year>) - <approach>", PRs "<Game>: <what it adds>".
- Already published: claim issue cranebeaker/coop-anything#94 and draft PR #95 (branch `game/mdk2-hd`
  on the fork, remote `fork`). Add to that PR while it is open (commit + `git push fork game/mdk2-hd`)
  instead of opening a new one.

## Public repo and identity
- This repo is public at github.com/TommoDotCommo/mdk2-coop (MIT; `origin`, branch `main` there,
  `master` here). Push with `git push origin master:main`.
- Commits must carry only the GitHub identity: `TommoDotCommo <15873245+TommoDotCommo@users.noreply.github.com>`
  (set in this repo's and the coop-anything clone's local git config; check `git config user.email`
  in any new worktree or clone). No real names, emails or usernamed paths in files or commit messages.

## Testing
Background mode only (`MDK2_BACKGROUND=1`, set by `tools/qa.sh` and `tools/sweep.sh`): games on the
2nd monitor, muted, and never taking focus. `mdk_kill` closes only the copies in MDK2_DIR1/2.
Full regression: `tools/sweep.sh` (96 checkpoints, about 55 min; run it in the background).
