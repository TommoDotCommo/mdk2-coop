# Release notes: draft lines for the next version

Not final: edit the version, merge into the release's notes file and delete this file when the release is tagged.

## Doc's leaf blower works for every player

- **Blowing enemies as the client moves them.** A client's Doc blew only the copies of the enemies on the client's own
  screen; the real ones are the host's, so on the host nothing moved, and on the client the enemies leaned and flipped
  in place while their copies were put back. Herding the aliens into the plant on level 3 only worked for the host.
  The host now gives the client's Doc the game's own push (the volume the game builds on Doc when he takes the blower
  up), switched on while that player blows: the host's game pushes the enemy, with its walls and the enemy's own
  reaction, and every player sees it move. The blower moves enemies and objects, not the other players. A client's
  blower no longer pushes its own copies of the enemies either: that gave them the game's physics, and a copy blown
  against the lounge's doorway stayed there on the client's screen while the real enemy walked away. Checked by
  `tools/qa_blower_live.sh` against a Doganboy whose AI is running.

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

## Co-op from a controller (Steam Deck)

- **The View button is the co-op layer.** Tap View (Back) to ping. Hold it for the player list, and while it is held:
  **D-pad up** the emote wheel, **LB** the character wheel, **RB** the quick chat wheel (Ready, Wait for me, Over here,
  Come look at this, Thanks); the wheels stay open while View is held, the right stick picks and letting go of View
  chooses. **D-pad down** opens chat, **D-pad left / right** watch the previous / next player, **Y** shows the quick
  guide. While View is held the D-pad, LB, RB and Y do nothing in the game (the D-pad used to walk you).
- **Typing without a keyboard.** Chat from the pad opens Steam's floating keyboard when Steam can show it (Big Picture,
  Steam Deck), otherwise a keyboard on screen: D-pad or left stick, A types, X deletes, Y space, LB shift, Menu sends,
  B cancels. The button that sent or cancelled the line does not reach the game.
- **Voice on a pad.** New Voice mode setting (launcher Settings > Voice and sound, and Options > Co-op > Voice and sound):
  Push to talk, Open mic, or **Automatic** (the default): push to talk after keyboard input, open mic with the voice
  gate after pad input. Push to talk on the pad: hold the right stick click (or the left one, or none: Push to talk on
  a pad).
- **Hints follow your hands.** After pad input the quick guide, the player list, the wheels and "Hold X to revive" show
  pad buttons (Xbox names: View, Menu, A, B, X, Y, LB, RB); after a key they show keys again, at once. The Controls
  page's Gamepad tab, the in-game quick guide and the launcher's guide list the View layer from one table.
- **Fix:** with a real pad (not couch co-op), co-op buttons such as X (revive) and Back (ping) now reach the co-op
  features; before they only reached the game.

New gates: `controller-core` now covers the View layer from recorded pad states (with a mutant whose D-pad still walks)
and the glyph switch; `coop-keys` checks the guide's pad lines; live: `pad-layer` (`tools/qa_padlayer.sh`).

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

## Mod managers

- **Vortex and Mod Organizer 2.** Every release carries a Vortex game extension (`mdk2hd-vortex-extension.zip`) and an
  MO2 game plugin (`game_mdk2hd.py`) for MDK2 HD, so the managers install the release zip with no extra step and start the
  game with the co-op launcher. README, Install > Mod managers. The MO2 side is untested under its virtual file system.

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

## Menu review follow-ups

- **Session page: Copy invite and Leave.** The host's Options > Co-op > Session has Copy invite (the same text and code
  as the launcher's button, from the lobby the game started from) and, in a game, everyone has Leave, which asks the same
  one question as Esc > Quit: "Everyone returns to the lobby" for the host, "The others keep playing" for a guest.
- **DPS meter is Off / Line / Full** in the launcher and in game (`[hud] dps`, off by default). Ctrl+M writes the new
  value back, so the chooser always agrees with the screen.
- **Developer keys** show in game only when developer mode is on, last in Players and interface (they are no longer among
  the host's Co-op rules). The switch stays in the launcher's Settings > Launcher.
- **The launcher's F1 guide reads the in-game key table.** Its co-op lines are generated from it
  (`tools/gen_guide_keys.py` writes `bridge/guide_keys.h`; `tools/test_coop_keys.py` fails when it is stale), so hold Tab
  (player list) and [ ] (watch) appear and the keys stay in step.
- Checks: `tools/qa_session_menu.sh` (two games; Copy invite decoded by `tools/invite_decode.py`, Leave asks once, Ctrl+M
  write-back, with a mutant), plus mutants in `tools/test_ui_layout.py` and `tools/test_coop_keys.py`.

## The whole launcher in the MDK2 look

- **Buttons in the game's steel.** Every button other than the big orange one (Mute me, Deafen, Mute, Kick, Leave
  lobby, Send, Copy invite, Open to my networks, Save address, Check now, Send diagnostics, Try again, Rename, Pin,
  Forget...) now has the game's diamond-plate steel with a raised edge and its label in the game's menu font, sized to
  fit the button. The one under the mouse or the keyboard gets an orange edge; the host's "Waiting for <names>" is
  darker until everyone is ready. The short orange buttons (Install update, Join, Save) keep their label inside.
- **Choosers, switches, text boxes and the Join list** use the same steel.
- **Who plays whom.** In the host's lobby each player's row shows the face of the hero everyone starts the chosen
  level as (Kurt, Max or Doc), cut from the game's own comic panels, next to the player's colour.
- **The title art stays in view** behind a lighter lobby panel, as on Play. The lobby's key hints use the same size as
  on the other pages; body text, hints and chat stay in the plain readable face.
- **Guests see the hero portraits too.** The host's roster now names the level it chose, so every player's lobby rows show the
  same face as the host's. An older host sends no level (a newer guest then draws no portrait, as before); an older guest
  ignores the extra field. No protocol change: the roster's first line only gains a number at its end.
- **Saves cards show the hero of the save's level** beside their text; without `mdk2.tar` they look as before.
- All of it is read from your own `mdk2.tar` when the launcher opens (in one pass over the archive); nothing from
  the game is shipped. Without the archive the launcher looks as before.
- Checks: `launcher-look-core` (labels fit their button or fall back, the heroes per level; a mutant whose labels never
  shrink must fail), new `tools/test_ui_layout.py` checks with two mutants, and `tools/qa_launcher_look.sh` (two games:
  every page in the game's look with no label wider than its button, the portraits on the host, the guest and the Saves cards, and the same pages with
  `[test] launcher_art=0` drawn as before).

## Crash guards
- **A position set on a missing object** (a script or test command racing a level change, `mdk2hd.exe+0xdda8`) is
  ignored instead of crashing the game.
- **A stale object handle in the per-frame scene pass** (a shot-up scenery object, `mdk2hd.exe+0x570bd`) is skipped
  the way the pass's first walk already skips it, instead of crashing the host.
- Both checks verify the game's code bytes first and stay off on an unrecognised build. Checks: `tools/qa_setpos_guard.sh`
  and `tools/qa_scene_guard.sh`, each with an unguarded mutant that must crash at the same site.
