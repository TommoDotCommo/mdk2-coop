# Agent instructions

Read and follow [CLAUDE.md](CLAUDE.md) before starting work in this repository,
including the Co-op Anything `AGENTS.md` it references. `CLAUDE.md` is the shared
source of truth for session setup, research notes, testing, privacy, commit identity,
and publishing permissions. Keep shared workflow changes there so agents stay in sync.

## Repository map

- `README.md`: architecture, supported game build, player setup, and QA commands.
- `bridge/`: C code for the 32-bit Windows `dinput8.dll` proxy. `bridge.h` holds
  shared declarations and engine addresses; `dinput8.c` handles proxy startup;
  `frame.c` drives the per-frame update; `lua.c` exposes the Lua bindings.
  Networking and gameplay features live in the other subsystem `.c` files.
- `lua/coop_dev.lua`: in-game co-op logic, menus, and developer harness; installed
  as `<game dir>/coop/coop.lua`.
- `dist/`: Windows player installers, texture tools, and example configuration.
- `tools/`: WSL Bash automation, Python state comparison, and Windows PowerShell
  helpers in `tools/win/`. `tools/mdk.sh` defines game-copy and port configuration.
- `local/`: ignored QA evidence, session logs, sweeps, and release output.

## Build and validation

- Build the DLL: `make -C bridge` (requires `i686-w64-mingw32-gcc`).
- Install into configured test copies: `MDK2_BACKGROUND=1 tools/install.sh`.
  This builds first and closes games running from those copies.
- Basic two-player regression: `tools/qa.sh`.
- Feature suites: `tools/qa_n.sh`, `tools/qa_save.sh`, `tools/qa_menu.sh`,
  `tools/qa_hud.sh`, and `tools/qa_revive.sh`. Check each script's header for
  instance counts and prerequisites.
- Checkpoint sweep: `tools/sweep.sh [from] [to]` (for example, `1-1 1-3`).
  Reports and evidence go under `local/qa/` or `local/sweeps/`.
- Package a release: `tools/release.sh <version>`; the zip goes under
  `local/release/`. Follow `CLAUDE.md` for any publishing step.

Run game tests in background mode as required by `CLAUDE.md`. These harnesses need
Windows/WSL interop and isolated copies of the supported game. For documentation
changes, check links, paths, and `git diff --check`; game runs are unnecessary.
Report only validation actually performed.

## Compatibility constraints

- Engine addresses and hooks target MDK2 HD Steam build 3902. Preserve build
  detection and expected-byte checks when changing native hooks.
- The game embeds **Lua 3.1**: no `local function`; closures use `%upvalues`.
  Follow the existing script's syntax rather than modern Lua conventions.
- Keep the `settings.lua` loader small: the game reads it into an 8 KB stack
  buffer. Put mod logic in `lua/coop_dev.lua` and the bridge.
- Preserve the host's authority over world state, damage, and level changes;
  account for level epochs when changing synchronization.
