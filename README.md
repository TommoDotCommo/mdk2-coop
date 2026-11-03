# mdk2-coop

Online co-op for **MDK2 HD**. One player hosts, everyone else joins, and you play the campaign together:
shared enemies, doors, pickups and progress, with going down and being revived instead of dying.

**Status: public alpha.** Every checkpoint of the campaign loads and plays in automated two-player tests,
but nobody has finished the whole campaign in co-op yet. Expect rough edges, and please
[report what breaks](#reporting-a-problem).

No game files are in this repo. Every player needs their own copy of MDK2 HD on Steam.

[![Three players in the first level, with the players panel and name tags](docs/showcase/players.jpg)](docs/SHOWCASE.md)

**[See the showcase](docs/SHOWCASE.md)** for screenshots of every feature.

- [What you get](#what-you-get)
- [Install](#install)
- [Play together](#play-together)
- [Controls](#controls)
- [Settings](#settings)
- [Couch co-op](#couch-co-op-one-pc-2-4-players)
- [Known issues](#known-issues)
- [Help test](#help-test)
- [Reporting a problem](#reporting-a-problem)
- [Uninstall](#uninstall)
- [For developers](#for-developers)
- [Code signing policy](#code-signing-policy)
- [Privacy](#privacy)

## What you get

- **2 to 16 players** (tested with up to 8), over Steam friends-only lobbies or direct UDP.
- **The whole campaign in co-op**: everyone follows the host from level to level; enemies, their health,
  doors, pickups and shots are the same for everyone. Pickups give their item, ammo or healing to every connected player.
- **What one player does happens for everyone**: when anyone walks into a new room or picks something up, the
  level's scripts run for all of you, and a boss starts when anyone gets near it. If a door closes behind them,
  anyone left outside the area, or on the far side of the door, is brought to them (with a note saying so), like a
  modern co-op game. Everyone can take their own tutorial grenades.
- **Down, not dead**: a hit that would kill you puts you down, and a teammate can stand you up. Bleed out and
  you watch a teammate until your area is clear or the next checkpoint.
- **Cutscenes together**: they start when everyone is ready, skipping is a vote, and nobody moves on until
  everyone has seen the end. The minigames show the other players as ghosts.
- **Co-op saves** every few minutes, so a crash or a dropped connection costs nothing: the same player comes
  back where they were.
- **Players on screen**: a panel with everyone's health and name tags over their heads.
- **Text chat, pings and proximity voice chat.**
- **Switch hero at any time** (Kurt, Max or Doc) and an **emote wheel**.
- **Difficulty presets** (Story, Normal, Hard, Nightmare) or your own mix.
- **Modern controls** (WASD, Space to jump) or the game's originals, a controls page that draws every key,
  and **gamepad** support.
- **A launcher** before the game starts: host, join or play solo, your name, every setting, and a save
  browser that remembers who you played with.
- **Couch co-op**: 2 to 4 players on one PC, one window and one gamepad each.

## Install

You need MDK2 HD: the **Steam** version (build 3902) or the **Overhaul Games** (Beamdog) version. Setup reads the
game's executable (it never runs it) and tells you what it found before you install: *Found: MDK2 HD Steam build 3902*,
*Found: MDK2 HD Overhaul Games build*, or *Found: unknown build (SHA-256 ...)*. For an unknown build Setup warns that
co-op will stay off and installs only if you say so. If you do start such a game, a message box says the same in plain
words and what to do (in Steam: Properties > Installed Files > Verify integrity of game files, and Properties > Betas >
None), and `coop\unsupported-build.txt` keeps the text and the exe's SHA-256 for a bug report.

1. Download **MDK2-Coop-Setup.exe** from [Releases](https://github.com/TommoDotCommo/mdk2-coop/releases). It is one
   file: there is nothing to unzip. (The zip on the same page is for a manual install, below.)
2. Double-click it. It finds MDK2 HD (Steam libraries or the Overhaul Games folder; browse
   to the folder if it cannot), then choose **Install / Repair**. If the game folder is protected, Windows asks
   for administrator permission. Host or Join, Steam or direct IP
   and the address are chosen in the launcher at every start.
3. Choose **Play MDK2** or **Close** when setup finishes. You can also start MDK2 HD as usual
   from Steam or the Beamdog client. The co-op launcher opens first.

Already installed? Open setup and click **Play MDK2** to launch the game. Use **Create desktop shortcut** to add a shortcut that opens the game and its co-op launcher directly. Both buttons are also available after installing. Keep Steam or the Beamdog client running.

**"Windows protected your PC"?** Setup is not code-signed yet, so SmartScreen warns about any new unsigned program:
choose **More info > Run anyway**. Some antivirus programs also dislike that Setup starts PowerShell to run its installer
scripts. Nothing is hidden: `MDK2-Coop-Setup.exe /extract <new folder>` writes the scripts (`installer-gui.ps1`,
`mdk2-coop.ps1`, `install-state.ps1`) and the other files to a folder you can read in Notepad first, and every release
lists its SHA-256 (`MDK2-Coop-Setup.exe.sha256`). Setup unpacks its own files, after checking them against a SHA-256 stored in the
exe, into a fresh folder under `%LOCALAPPDATA%\mdk2-coop\setup\<version>`; it never writes into an existing folder or
through a link. If you opened Setup from inside the zip without extracting it, it says so: extract the whole zip first, or
download the single `MDK2-Coop-Setup.exe`.

### Manual install (from the zip)

Use this if you would rather not run Setup, or on a PC where it will not run. Unzip the release, close the game, then
copy two files into the game folder (the one with `mdk2hd.exe`):

| From the zip | Into the game folder |
|---|---|
| `dinput8.dll` | `<game folder>\dinput8.dll` |
| `coop\coop.lua` | `<game folder>\coop\coop.lua` (make the `coop` folder if it is not there) |

Nothing else is needed: at its first start the mod adds its one loader line to `%LOCALAPPDATA%\MDK2HD\settings.lua`
by itself, and the mod keeps its settings, logs, saves and update backups in `%LOCALAPPDATA%\mdk2-coop`: nothing is
written into the game folder while you play. If another mod's `dinput8.dll` (an ASI loader, a widescreen fix) is
already there, rename it to `dinput8.chain.dll`: the co-op DLL loads it after itself, so both keep working. This is
also how mod managers (Vortex, Mod Organizer 2) install it; `coop-mod.json` in the zip lists the files with their
SHA-256, Setup then shows "Installed by a mod manager" and the in-game updater leaves the update to the manager. The
other files in the zip (`Couch.cmd`, `mdk2-couch.ps1`) are for couch co-op.
The mod does not patch the game executable. Manual uninstall: delete those two files (rename `dinput8.chain.dll` back
to `dinput8.dll` if there was one); optionally delete the `-- >>> mdk2-coop >>>` ... `-- <<< mdk2-coop <<<` block in
`%LOCALAPPDATA%\MDK2HD\settings.lua` (it does nothing without the mod) and the `coop` folders.

### Steam Deck and Linux (Proton): untested

MDK2 HD runs through Proton, and the mod is a `dinput8.dll` proxy. Wine and Proton have their own built-in
`dinput8`, so Steam must be told to load the mod's file first. Untested by the project so far (see Known issues).

1. Install the game in Steam (desktop mode on the Deck) and run it once.
2. Copy the files in: unzip the release on the Deck, open a terminal in the unzipped folder and run
   `sh install-linux.sh install "<game folder>"`, where the game folder is the one with `mdk2hd.exe`, for example
   `~/.local/share/Steam/steamapps/common/"MDK 2 HD"`. It copies the two files, keeps another mod's `dinput8.dll` as
   `dinput8.chain.dll` (any other file that was there as `<file>.coop-original`), never writes through a link, records
   what it did in `coop/install-linux.record`, and writes `"dinput8"="native,builtin"` for `mdk2hd.exe` into the game's
   Proton prefix (`steamapps/compatdata/213350/pfx/user.reg`, backed up first), so no launch option is needed.
   `sh install-linux.sh status --json` says what is installed and whether that override is in place.
3. Only if it said there is no Proton prefix yet (the game never ran): in Steam, right-click MDK2 HD > Properties >
   General > Launch Options and enter exactly `WINEDLLOVERRIDES="dinput8=n,b" %command%` (or start the game once and
   run step 2 again).
4. Start the game from Steam. The co-op launcher opens first. `bridge.log` (or `unsupported-build.txt`) in the prefix's
   `%LOCALAPPDATA%\mdk2-coop\coop` says what happened if it does not.

To remove it: `sh install-linux.sh uninstall "<game folder>" --yes` removes exactly the files it installed (a file you
changed since is left alone), puts the original `dinput8.dll` back, takes its DLL override out of the prefix again, and
refuses to do anything without its record. Clear the launch option if you set one. Both scripts (`install-linux.sh`
and `MDK2-Coop-Setup.exe`) take `status --json`, `install <game folder> --yes` and `uninstall <game folder> --yes`
with exit codes 0 done, 1 failed, 2 usage, 3 unsupported game build, 4 the game is running. Please report whether it worked, with the Proton version, in an
[issue](https://github.com/TommoDotCommo/mdk2-coop/issues).

## Play together

### The launcher

When you start the game, the launcher opens: choose **Host a game**, **Join a friend** or **Play solo**
(plain MDK2 HD, co-op off for that start), your name, Steam or direct UDP, and press **Enter** (or the big
button) to start. The **Saves** tab lists your co-op saves with who played and how far you got, and lets the host
start a new game at any level and checkpoint. The **Settings** tab has every option. **Help** collects what a bug
report needs in one click (see [Reporting a problem](#reporting-a-problem)). **Esc** quits.

Don't want the launcher? **Settings > Display > Show launcher at start: Off**. Hold **Shift** while the game
starts to bring it back. Accepting a Steam invite skips it and joins straight away.

### Over Steam (recommended)

**Host:** start the game, then **Options > Co-op > Session > Invite Steam friends**, or share the numeric
**Lobby ID** shown there. Start a new game or load a save; your friends follow you into your level.

**Join:** start the game, then accept the host's Steam invite, use **Join Game** in Steam's friends list, or
enter their lobby ID in **Options > Co-op > Session > Join lobby by ID**. Start the game before accepting an
invite. Once joined, wait for the host: your game loads automatically. **Esc** cancels joining;
**Session > Join host again** retries. You do not need to choose New Game.

No port forwarding, public IP or extra downloads are needed. Everyone needs Steam online and the same mod
version. If the Steam overlay is off, share the lobby ID instead.

### Over direct UDP (LAN, or manual internet setup)

The host listens on **UDP port 27900** (changeable in the launcher). On a LAN, joiners enter the host's local IP
address. Over the internet, the host forwards that port on their router and shares their public IP address.

In the lobby, **Save address** adds the host address and port to **Saved addresses** without leaving the lobby. Saving an existing address keeps its name and notes. **Join** in Saved addresses takes you straight to the connected lobby.
Allow MDK2 HD through Windows Firewall when asked.

### While you play

- The host decides where everyone goes: level changes, checkpoints and loading saves follow the host.
- **Going down**: hold **R** next to a downed teammate for 3 seconds to stand them up. Down for 30 seconds and you
  are out: you watch a teammate (left click: the next one) and come back when your area is clear of enemies or at
  the next checkpoint. Lava, black holes and falling out of the world are straight out. If everyone is down, the
  checkpoint restarts for everyone.
- **Cutscenes**: press **Backspace** (or Esc) to vote to skip; a majority skips for everyone, and pressing again
  takes your vote back. Press any other key to see the time left and the votes in the bottom right corner. When
  you finish a cutscene first, you wait for the others.
- **If someone's game crashes or their connection drops**, the others wait and that player comes back where they
  were. If the host's game crashes, the host can **Resume co-op** from the Load Game menu and everyone follows.

## Controls

The default is the **Modern** preset. Change it, or rebind any key, in **Options > Co-op > Controls**, which
draws the keyboard and mouse with every key labelled. **Original** gives you the game's own keys.

| Action | Modern preset |
|---|---|
| Move | W A S D |
| Jump, jetpack (Max), parachute (Kurt) | Space |
| Fire | Left mouse |
| Sniper mode (Kurt) | Right mouse |
| Previous / next item | Q / E, or the mouse wheel |
| Open the inventory | F (Tab is the player list now) |
| Text chat (Enter sends, Esc cancels) | T |
| Ping what you aim at | V or middle mouse |
| Revive a downed teammate (hold) | R |
| Push to talk (hold) | B or the mouse side button |
| Emote wheel (hold, point, let go) | H |
| Player list (hold): ping, state, mute, volume, watch | Tab |
| Watch the previous / next player when you are out of the fight | [ and ] (left mouse works too) |
| Character wheel (hold, point, let go) | J |
| Vote to skip a cutscene | Backspace |
| Hold for the Mod Quick Guide (launcher or in game) | F1 |
| Put a marker in the logs (for bug reports) | F2 |

The **emote wheel** offers each hero the animations they have; its last slot, **All...**, lists every
animation, a page at a time. The **character wheel** switches you to Kurt, Max or Doc on the spot, keeping your
share of health; **Level's own** goes back to the hero the level gave you.

A **gamepad** (Xbox style) works too: left stick moves, right stick looks, right trigger fires, left trigger is
sniper mode, A jumps, X revives, Y opens the inventory, Back pings, the bumpers change item, B or Start pauses.

## Settings

In the launcher's **Settings** tab, or in game under **Options > Co-op** (on the title screen, or Esc > Options), whose pages are named like the launcher's sections:

- **Co-op rules** (was Co-op / Gameplay; the host's choice counts): friendly fire, difficulty, how often co-op saves are made,
  back to the level's hero after a level you switched hero in, controls preset, gamepad, developer keys.
- **Difficulty**: Story, Normal, Hard, Nightmare, or each setting on its own: enemy health, extra enemy health per
  player, damage you take and deal, time before you bleed out and time to revive.
- **Players and interface** (was Interface; your own): players on screen, player outlines through walls (off by default), the cutscene
  timer, friends in minigames, and the emote wheel, character wheel, text chat and pings, each of which can be
  switched off.
- **Voice and sound**: voice chat on or off, its volume, the hearing range (host), and the game's effects, music
  and speech levels.
- **Display**: windowed or fullscreen, resolution, show the launcher at start.
- **Session** (in game; the list of Players is part of it): your name, host or join and the connection type (both next start), Steam lobby status,
  invites, joining by lobby ID, or the UDP host's address. On the host: Resume co-op and Save now.

## Couch co-op (one PC, 2-4 players)

After installing, double-click **Couch.cmd** in the release folder. It starts one game window per player,
tiled on one screen, each played with its own gamepad (player 1 = first pad). Player 1 starts the game from the
title screen and the others follow. Only player 1 has sound, and couch games have no voice chat. Options:
`Couch.cmd -Players 3`, `-Monitor 2`, `-Stop`.

Each window is a whole game (about 370 MB of memory each), so four players need a strong PC. Couch games play
over the local network inside your PC and cannot be combined with online play.

## Known issues

- **Steam networking** has passed automated tests and a real lobby test, but **invites between two separate Steam
  accounts and long internet sessions have not been tested yet**. Most testing so far used direct UDP.
- **No host migration**: if the host leaves, the session ends for everyone. A host who restarts creates a new
  lobby; join its new invite or ID, then the host resumes.
- **The host needs upload bandwidth**: about 1.3 Mbit/s with 8 players in a busy level, less with fewer.
- **Nobody has finished the whole campaign in co-op yet.** Every checkpoint loads and plays in automated
  tests, but those don't solve puzzles or beat bosses.
- Some cutscenes started by events inside a level have no countdown until they have been watched once.
- Real gamepads, microphones and 3-4 couch players have only been tested with simulated input.
- **Steam Deck / Linux (Proton) is untested.** The launch option and `install-linux.sh` are tested against fixture
  folders only; nobody has run the game that way yet.
- **An unsupported game build** (for example a Steam beta branch) keeps co-op off with a message; only Steam build 3902
  and the Overhaul Games build are known.
- Setup is **not code-signed yet**: Windows SmartScreen warns before the first run.

## Help test

This is an alpha, and some things can only be tested by real players. These help the most:

- **Steam invites between two different accounts**, and joining by lobby ID.
- **Long sessions over the internet**: an hour or more, with players far apart.
- **Finishing levels together**: puzzles, boss fights and the end of each level.
- **A real gamepad, a real microphone (voice chat), and couch co-op with 3 or 4 players.**

When a session goes well, that is worth hearing too: open a
[playtest report](https://github.com/TommoDotCommo/mdk2-coop/issues/new?template=playtest.yml) with what you
played and how it went.

## Reporting a problem

Ideas are welcome as a [feature request](https://github.com/TommoDotCommo/mdk2-coop/issues/new?template=feature.yml).

Open a [bug report](https://github.com/TommoDotCommo/mdk2-coop/issues/new?template=bug.yml); the form asks for
what's needed:

1. What happened, and what you expected. Press **F2** the moment something goes wrong: it puts a numbered
   marker in every player's log, saves a screenshot (`coop_mark<N>.tga` alongside the log), and every game saves
   the last minute it saw (frame rate, where everyone was, ping and lost packets) next to its log.
2. The diagnostics bundle, from everyone if you can, the host's first. Start the game, and in the launcher open
   **Help > Send diagnostics** (it works after a crash too: start the game again and use it). A few seconds later
   a zip appears on your desktop with the logs of recent runs, your settings and a summary of your PC and network.
   Your Windows and computer names, IP addresses and Steam IDs are replaced before it is zipped; it holds no saves
   and no game files. Attach it to the report. Every run keeps its own log, so nothing is overwritten by playing
   again (the newest 200 MB are kept; a run that crashed is never deleted).
   If the launcher is switched off or won't start, the logs are in `%LOCALAPPDATA%\mdk2-coop\coop\logs`
   (older installs and test copies: the `coop` folder in the game folder); `bridge.log` is the last run.
3. Which player you were (host or which client), your mod version (launcher's About tab), how you connected
   (Steam or UDP), and the level and checkpoint.

## Uninstall

Open **MDK2-Coop-Setup.exe** and choose **Uninstall** (installed by hand or on Linux: see the manual steps under Install). It removes owned mod files, restores verified original files and removes the settings loader while keeping current player options. Co-op settings, logs and saves in `%LOCALAPPDATA%\mdk2-coop\coop` stay;
delete that folder if you like. Older installs may also have data in the game folder's `coop` subfolder.

What the installer changes: `<game folder>\dinput8.dll` (another mod's earlier one is kept as `dinput8.chain.dll`,
which the co-op DLL loads; an older copy of this mod as `dinput8.dll.coop-original`),
`<game folder>\coop\` (the mod's script and data-location marker), and one loader line in
`%LOCALAPPDATA%\MDK2HD\settings.lua`, backed up first as `settings.lua.coop-backup`. The game rewrites that file
when you change its options, and the mod puts the line back at the next start. The installer copies existing
co-op data to `%LOCALAPPDATA%\mdk2-coop\coop`, without replacing data already there.
Setup lists file status and refreshes on activation or Refresh. Hash receipts beside deployed files protect
modified payloads and original backups. Review legacy backups before migrating older installations.
See [launcher maintenance](docs/LAUNCHER-MAINTENANCE.md) for recovery and verification limits.

## For developers

- [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md): how the mod works (the dinput8.dll bridge, synchronisation,
  cutscenes, saves, the launcher), the test harness and building a release.
- [docs/TESTING.md](docs/TESTING.md): the automated regression ladder, what it covers and its limits.
- [routes/README.md](routes/README.md): recording and replaying progression routes.

Built with the [Co-op Anything](https://github.com/cranebeaker/coop-anything) playbook; the research notes for
this game live in its `games/mdk2-hd/NOTES.md`.

## Code signing policy

Windows release files (`MDK2-Coop-Setup.exe`, `dinput8.dll`, `install-policy.exe`) are being moved to signing
through the SignPath Foundation program for open-source projects (application in progress):
Free code signing provided by [SignPath.io](https://about.signpath.io/), certificate by
[SignPath Foundation](https://signpath.org/). Signed builds are made by GitHub Actions from this public repository;
nothing built on a private machine is signed. The [release workflow](.github/workflows/release.yml) builds a public
release tag and waits for approval of two requests: first `dinput8.dll`, `install-policy.exe` and the zip's Setup,
then the single-file Setup containing that signed payload. Every binary has ProductName **MDK2 Co-op** and the same
ProductVersion. PowerShell scripts and archives are not signed; the download assets have SHA-256 sidecars.

Until the application is approved and signing is configured, workflow artifacts are explicitly unsigned. Signed runs
create a **draft** release for the maintainer to review and publish by hand. See the
[signing setup and release procedure](docs/LAUNCHER-MAINTENANCE.md#code-signing).

- Authors and committers: [TommoDotCommo](https://github.com/TommoDotCommo)
- Reviewers: [TommoDotCommo](https://github.com/TommoDotCommo) (changes from other contributors are reviewed before merge)
- Approvers (each signed release is approved by hand): [TommoDotCommo](https://github.com/TommoDotCommo)

## Privacy

The mod does not collect or send personal data.

- Update check: the launcher asks GitHub's public releases API for this repository at most once a day. No account,
  identifier or usage data is sent. Turn it off with the Auto-check setting on the About page.
- Multiplayer: game and voice traffic go only to the players you choose to host or join (directly, or through
  Steam when you use a Steam lobby).
- Diagnostics: **Send diagnostics** writes a zip to your own Desktop, with your Windows and computer names, profile
  folder and IP addresses replaced first. Nothing is uploaded; you decide whether to attach it to a bug report.

## License

MIT, see [LICENSE](LICENSE). It covers this repo's code only; MDK2 HD itself is not included.

### Shared combat feedback

Floating damage numbers (coloured per player, merging rapid hits into one growing number), a
hitmarker and a kill ticker show host-confirmed enemy HP loss. A small damage panel at the right
edge starts as one line (your DPS and the team's); Ctrl+M expands it to a bar per player with
DPS, totals and the fight timer, and it fades out after the fight. Ctrl+R resets local counters;
Ctrl+D dumps recent history to `combat.txt` in the mod data folder (the newest 512 events are
also saved automatically).
See [combat feedback](docs/COMBAT.md) for format, authority, limits and verification status.
