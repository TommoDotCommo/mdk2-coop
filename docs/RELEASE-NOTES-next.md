# Release notes: draft lines for the next version

Not final: edit the version, merge into the release's notes file and delete this file when the release is tagged.

## Setup that a stranger can run

- **One file.** Download `MDK2-Coop-Setup.exe` from the release page and double-click it. There is no zip to extract:
  the release files are inside, checked against a SHA-256 and unpacked into a fresh folder under
  `%LOCALAPPDATA%\mdk2-coop\setup\<version>` before the installer opens. `MDK2-Coop-Setup.exe /extract <new folder>`
  only unpacks them, so the PowerShell scripts can be read first. The zip stays for manual installs.
- **Run from inside the zip?** The Setup in the zip now says "Extract the whole zip first, or download the single
  Setup.exe" instead of a generic error.
- **The game build, in plain words.** Setup shows what it found before installing ("Found: MDK2 HD Steam build 3902",
  "Overhaul Games build", or "unknown build (SHA-256 ...)"). An unknown build installs only after a warning that co-op will
  stay off. If you start such a game anyway, a message box says why and what to do (verify game files in Steam, opt out of
  betas, which builds are supported) and `coop\unsupported-build.txt` keeps the text. Before, only `bridge.log` said so.
- **Manual install, Steam Deck and Linux.** The README lists exactly which two files go where and how to remove them by
  hand. For Proton (untested so far) the launch option is `WINEDLLOVERRIDES="dinput8=n,b" %command%`, and
  `install-linux.sh` in the zip copies the files into a game folder with backups and a matching uninstall.
- **Ready for code signing.** `tools/release.sh` signs every exe and dll when `SIGN_CMD` is set; the release published
  here may still be unsigned (Windows SmartScreen then warns: More info > Run anyway).

## Hosting that explains itself

- **Friends can join by.** The lobby checks every way in and says what it found: same network, Tailscale (with this
  PC's Tailscale name), the internet and Windows Firewall, each with its fix. Steam stays on Play: Connection: Steam.
- **The router port, by itself.** While your lobby is open it asks the router to open the port (UPnP, else NAT-PMP),
  keeps it open while the game runs, and removes it when you leave or quit the game. If your provider shares your address (CGNAT), the
  lobby says so and points to Steam or Tailscale. Settings > Network > Open the router port while hosting.
- **One invite.** Copy invite puts "Fox is hosting MDK2. Join: <link>" on the clipboard; the code carries every
  route that works (internet only when the router check passed). Paste the code or the link into Join.
- **Invite only.** Choose who can see your lobby: Open to my networks, or Invite only (not listed; the invite's code
  is needed to get in, in the lobby and in the game it starts).
- **The lobby.** Players, chat and "Friends can join by" side by side. Start game waits until everyone is ready
  ("Waiting for Fox and Bob"). The host can Kick a player from the lobby.
- **Four tabs.** Play, Saves, Settings, About. Help moved into About (Report a problem). Settings has Network and
  Launcher sections. An available update shows a banner on Play, never over a lobby.
- **Pad.** The launcher works with a pad: stick or D-pad, A, B, LB/RB tabs, Start, X copies the invite, hold Y for
  the quick guide; the hints switch to pad buttons.

## Under the hood

- The game-build test is one shared header (`bridge/build_sigs.h`) used by the DLL at every start and by the installer on
  the exe file on disk.
- The bridge no longer logs a fake "crash" line for the address probes of an unknown build, and removes its exception
  handler when it is unloaded.
- New offline gates: `setup-core`, `linux-install`, `release-signing`, `update-layout`, `release-package`; new live
  gates: the Setup exe checks in `installer` and `build-message`.

## Shared co-op keys, the player list and menu names

**Keys that moved or are new** (only defaults move; a key you set yourself stays):

- **Hold Tab: player list.** One row per player: colour, number, name, host mark, speaking light, ping, hero, distance and
  state (loading, down, out, watching). With the mouse: Mute, voice volume - / +, and Watch when you are out of the fight.
  The camera and the fire button wait while it is open; letting go of Tab closes it. The strip in the top left stays
  (Options > Co-op > Players and interface > Players on screen).
- **Push to talk: B or the mouse side button.** The side button is new; B stays.
- **[ and ]: watch the previous / next player** when you are out of the fight (left mouse still steps to the next one).
- **Open inventory: F only** in the Modern preset (it lost Tab, which is the player list now). The Original preset keeps the
  game's own keys. A Custom slot that is an untouched copy of the old Modern preset moves off Tab too; any Custom slot
  you changed keeps what you set (and the bridge log says so if Tab is still open-inventory there).
- The shared keys (F1, F2, T, V, middle mouse, B, side button, H, Tab, [ and ]) can no longer be given to another action on
  the Controls page ("Tab is reserved for Player list").
- The quick guide (hold F1) now writes every co-op key line from your current bindings.

**Menu pages renamed, in the launcher's order** (Options > Co-op): *Session* (now includes *Players*), *Co-op rules*
(was Gameplay; Difficulty stays inside it), *Players and interface* (was Interface), *Voice and sound* (was Voice chat),
*Controls*. The launcher's Settings sections are named the same (*Co-op rules*, *Players and interface*) and its
"Voice chat volume" row is now **Voice volume**, as in the game.

**Same words everywhere:** "Fox joined", "Fox left" appear as notices; the waiting screen says "Reconnecting to Fox"; leaving a
co-op game from the pause menu says what happens: "The others keep playing" (client) or "Everyone returns to the lobby" (host).

New gates: `coop-keys` (key table, migration of saved bindings, guide lines, notices; with mutants), `player-list-core`
(the list's state machine; with mutants), watch-key cases in `revive-core`; live: `shared-keys`.

## Joining without typing an address

- **Games near you.** *Join a friend* lists every host the launcher finds on each network this PC is on (Wi-Fi,
  Ethernet, VPN adapters), on your Tailscale devices (Settings > Network > Find friends on Tailscale, on by default)
  and among hosts you joined before: name, in lobby / in game / full, players, LAN / Tailscale / Recent, ping. A
  host with another version is listed greyed with its version. The list refreshes every 5 s while it is shown;
  nothing is sent while it is not. Pick a row and press **Join <name>**.
- **One paste box** for an invite code, an invite link, a Steam lobby number, an address (with or without a port),
  a computer or Tailscale name. It says what it recognised before you join. An invite tries all its routes at
  once and keeps the best one that answers.
- **Invite on the clipboard?** When the launcher comes to the front it offers "Fox's invite is on your clipboard.
  Join?" (Join / Not now). The clipboard is read for that check only, never stored or sent.
- **Recent hosts** replace Saved addresses: every host you join is remembered with the way that worked; rename,
  pin, forget. Your saved addresses move there once, pinned. **Rejoin <name>** shows on Play when the last host is
  online.
- **Open in launcher.** A game started with an `mdk2-coop://` invite link (the join page's button) opens the launcher
  on Join with the invite in the paste box, even when the launcher is set not to show at start.
- **Failures say who and what**, with Try again, Update (another version) or Send diagnostics.
- The Host address and Port fields are gone from Join; the port is in **Settings > Network**, with the two
  Find settings.
- Steam friends are not listed yet: the launcher opens before Steam starts in the game. Steam invites and lobby
  numbers still join.
- Under the hood: the launcher lobby and the game answer the new CPQ2 probe next to CPQ1, at most 10 replies a
  second per address. New gates: `launcher-join` (offline, 24 mutants), `join-list` (live, two games).

## Menu review

- **Host decides.** Every row the host's choice counts for says so in both places: the launcher's hint reads "Host decides"
  and the in-game rows end in "(host decides)" (friendly fire, difficulty, co-op save interval, back to the level's hero,
  hearing range). Options > Co-op > Co-op rules now lists them in the launcher's order.
- **Open lobby.** The big button on Play says "Open lobby" when hosting opens the lobby (it said Host, or Continue when a
  save was picked).
- **Text that fitted badly.** The Settings section picker no longer runs "Players and interface" over its arrows, and
  About keeps a long (development) version clear of its heading.
- **Session page, host.** A host saw "Host address: 127.0.0.1:27900", a client's row; it now says "Listening on UDP port <port>".
- **Quick guide and Controls page.** Lines that were cut off at 1280x720 (emote wheel, custom rules, interface, voice chat,
  watch previous / next, push to talk) are shorter; both guides say Help moved to About and the launcher switch to Settings > Launcher.
- **DPS meter.** Off by default; Players and interface > DPS meter (launcher or in game) turns it on within two seconds
  as one line; Ctrl+M cycles off, line, full. The launcher hint now says so; the README and the combat notes were still
  describing the old always-on line.
- Tests: `ui-layout` now reads the launcher and the menu source (the four tabs, every Settings section above the
  status line, row order and "host decides" tags in launcher and game, the DPS row, the Open lobby label, text-fit
  guards; 10 mutants); `qa_combat.sh` follows the three-step Ctrl+M cycle. `tools/menu_review.sh` captures every
  launcher page, the lobby as host and guest, and every Options > Co-op page at 1280x720 and 1920x1080, and checks the
  DPS switch and Ctrl+M live.
