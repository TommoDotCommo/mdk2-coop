# mdk2-coop: agent instructions

This file is the shared source of truth for every agent working in this repository
(`CLAUDE.md` imports it). Read optional checkout instructions in `local/AGENTS.md`
when present before starting work. Keep shared workflow changes here so agents stay in sync.

## Session setup

- Read `docs/DEVELOPMENT.md` and `docs/TESTING.md` as needed.
- Check `git status` and `git worktree list` before editing. Preserve unfinished work.
  Use a separate worktree if another agent is actively editing this checkout.
- Read existing evidence and handover notes under `local/`. Record observations,
  derived facts and guesses separately, with evidence; write `unknown` when unsure.
- The [Co-op Anything playbook](https://github.com/cranebeaker/coop-anything)
  provides research and testing methods.

## Repository map

- `README.md`: the players' guide (features, install, controls, settings, known issues).
- `docs/DEVELOPMENT.md`: architecture, engine hooks, the dev harness, manual playtests and releases.
- `docs/TESTING.md`: the regression ladder (`tools/qa_all.py`), its gates, coverage and limits.
- `bridge/`: C code for the 32-bit Windows `dinput8.dll` proxy. `bridge.h` holds
  shared declarations and engine addresses; `dinput8.c` handles proxy startup;
  `frame.c` drives the per-frame update; `lua.c` exposes the Lua bindings.
  Networking and gameplay features live in the other subsystem `.c` files.
- `lua/coop_dev.lua`: in-game co-op logic, menus, and developer harness; installed
  as `<game dir>/coop/coop.lua`.
- `dist/`: the Windows player installer and uninstaller, the couch launcher, the release `README.txt` and an
  example configuration.
- `tools/`: Bash automation, Python state comparison, and Windows PowerShell
  helpers in `tools/win/`. `tools/mdk.sh` defines game-copy and port configuration.
- `local/`: ignored QA evidence, session logs, sweeps, and release output.

## Build and validation

- Build the DLL: `make -C bridge` (requires `i686-w64-mingw32-gcc`).
- Install into configured test copies: `MDK2_BACKGROUND=1 tools/install.sh`.
  This builds first and closes games running from those copies.
- Basic two-player regression: `tools/qa.sh`.
- Every suite in order, with leases and reports: `python3 tools/qa_all.py full --campaign-shards 4`
  (see `docs/TESTING.md`); a single suite: `tools/qa_<name>.sh`. Check each script's header for
  instance counts, prerequisites and its `MUTANT=` variants. A new `tools/qa_*.sh` must be registered in
  `tools/qa_all.py` (an offline test fails otherwise).
- Checkpoint sweep: `tools/sweep.sh [from] [to]` (for example, `1-1 1-3`).
  Reports and evidence go under `local/qa/` or `local/sweeps/`.
- Package a release: `tools/release.sh <version>`; the zip goes under
  `local/release/`. Follow the publishing rules below for any publishing step.

Testing rules:

- Work only on disposable game copies. Never change an owner's install or saves.
- Configure paths and resource allocation privately; see `docs/DEVELOPMENT.md`.
  Keep game copies, ports, process lists and test output separate between sessions.
- Automated game tests must use `MDK2_BACKGROUND=1`: muted, without taking focus or
  desktop input. Stop only processes belonging to the configured test copies.
  These harnesses need Windows process interop and isolated copies of the supported game.
- Prefer automated state checks. Turn manually discovered bugs into regression
  tests and verify that deliberately broken variants fail where practical.
- Run checks appropriate to the change; report only validation actually performed.
  Documentation changes need link/path checks and `git diff --check`, not games.

## Compatibility constraints

- Engine addresses and hooks target MDK2 HD Steam build 3902. Preserve build
  detection and expected-byte checks when changing native hooks.
- The game embeds **Lua 3.1**: no `local function`, no `getn`, no `true`; closures use `%upvalues`;
  `cos` / `sin` take degrees.
  Follow the existing script's syntax rather than modern Lua conventions.
- Keep the `settings.lua` loader small: the game reads it into an 8 KB stack
  buffer. Put mod logic in `lua/coop_dev.lua` and the bridge.
- Preserve the host's authority over world state, damage, and level changes;
  account for level epochs when changing synchronization.

## Privacy, commits and publishing

- Keep machine names, personal paths, other local projects, account details,
  credentials, game files, dumps and private session notes out of tracked files.
  Use `<game dir>` and `%LOCALAPPDATA%` in public examples.
- Check the diff and commit identity before committing. Use the repository's
  configured public identity and applicable checkout instructions.
- Follow explicit publishing permissions in the session or private instructions.
  Without authorization, prepare a reviewable local diff before asking to publish.
- Never force-push or use `push --all` / `--mirror`. New public contributions must
  follow the destination's conventions and contain no private workstation data.
