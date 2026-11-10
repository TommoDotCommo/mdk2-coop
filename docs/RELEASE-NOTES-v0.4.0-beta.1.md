# MDK2 HD Co-op v0.4.0-beta.1

The first public beta. It follows v0.3.5-alpha and holds everything since: joining a friend from a list instead of
typing an address, a lobby that says how friends can reach you, the launcher in the game's own look, co-op from a
controller, a one-file Setup, and a round of fixes from two-player playtests (Doc's leaf blower, the other players'
parachute, jetpack and blower, item puzzles, enemy hp and flying enemies on clients).

**Install:** download `MDK2-Coop-Setup.exe` and double-click it. It also updates an older install; a game that already
has v0.3.x offers this version on the launcher's About tab.

**Everyone needs this version.** The network format changed, so a v0.4.0-beta.1 game and a v0.3.x game cannot play
together; the Join list shows a host on another version greyed out, with its version.

**Supported game builds:** MDK2 HD on Steam (build 3902) and the Overhaul Games build.

## Playing together

### Doc's leaf blower works for every player

- **Blowing enemies as the client moves them.** A client's Doc blew only the copies of the enemies on the client's own
  screen; the real ones are the host's, so on the host nothing moved, and on the client the enemies leaned and flipped
  in place while their copies were put back. Herding the aliens into the plant on level 3 only worked for the host.
  The host now gives the client's Doc the game's own push (the volume the game builds on Doc when he takes the blower
  up), switched on while that player blows: the host's game pushes the enemy, with its walls and the enemy's own
  reaction, and every player sees it move. The blower moves enemies and objects, not the other players. A client's
  blower no longer pushes its own copies of the enemies either: that gave them the game's physics, and a copy blown
  against the lounge's doorway stayed there on the client's screen while the real enemy walked away. Checked by
  `tools/qa_blower_live.sh` against a Doganboy whose AI is running.

### Doc's items in their place

- **Starting at a Doc checkpoint of level 3**, the items the game gives Doc (toaster, duct tape, pipes) were drawn in
  the middle of the screen for the whole level, one floating over Doc's head, instead of stacked above his hands. They
  were given in the level's first frames, before the inventory display was ready; they now come a second later and
  stack above the hands as when picked up (`tools/qa_doc_items.sh`).

### You see the other players' parachute, jetpack and leaf blower

- **You see the other players' parachute, jetpack and leaf blower.** Each is a
  separate object the game keeps on the character's body, and a copy of a remote
  player (a puppet) ran none of the code that makes them:
  - **Max's jetpack** is made when Max picks the jetpack up (a model attached to
    his torso). A puppet never picked one up, so other players saw Max without a
    jetpack. The jetpack is now built on the puppet with the game's own calls and
    follows the owner's animation, idle and thrusting.
  - **The jetpack's flame comes on with the thrust.** The flame is two parts of
    the jetpack model that Max's own thrust code shows and hides; on a puppet,
    which never runs that code, they followed up to a second late, so a short
    burst showed almost no flame. Which parts of the item are shown is now carried
    from the owner: every peer's flame changes within about 100 ms of the owner's.
  - **Doc's leaf blower** is made when Doc takes it up (the item on his back and
    the puffs of air it throws). It is now built on the puppet the same way and
    blows when the owner's does.
  - **Kurt's parachute** exists from the start, but its copy on another player's
    screen carried the game's hidden bit (the bit the game also uses for things in a
    room nobody is awake in) while the canopy was open on its owner, when the two
    players were in different rooms. The canopy's shown bit is now carried from
    the owner and written every frame, after the game's room pass and before the
    draw.
- Players need the same version: the player and relay packet magics changed
  (`MDKV`, `MDKW`) because every snapshot now carries the three visuals.
- Checked by `tools/qa_attached.sh`: three peers on the player's (Overhaul)
  build, each in turn the owner, the players at least 300 units apart; each peer
  reads the visual from its own gob table (alive, on the owner's body and bone,
  shown, in the owner's animation, its model's parts shown as on the owner's side
  and changed within 200 ms of the owner's, on the pose of the bone the owner's
  game puts it on, gone when the owner's goes). Before the
  change the jetpack and the blower did not exist on the other peers
  (`MUTANT=gear` reproduces that) and the peers' copy of an open canopy carried
  the game's hidden bit when the owner was in another room (`MUTANT=shown`
  reproduces that); after it they follow the owner. `MUTANT=subs` (the flame
  left to the copied animation) and `MUTANT=gearbone` (the item on another bone)
  must fail too. This is process state, not
  a picture: the hidden bit is what the game reads to skip drawing a gob, and
  the bridge writes the value after the game's room pass and before the draw.

### Item uses reach every player

- **What one player does with an item now moves the level's puzzle for everyone.** A room's
  `OnItemUsed` script ran only in the game of the player who used the item, and the script
  state it changes is never synced. On level 3 that made Doc's leak unplayable in co-op:
  when one player used the duct tape at the pillar, their game plugged the leak and made
  the cord item, while the other players' games kept the leak open and had no cord, so
  their puzzle state diverged for good. The game that handles the use now tells the
  others (a new reliable message, which the host also keeps for players who join later),
  and their copy of the room's handler runs with the user's puppet as the player; this
  covers the leak (`l3_w_hub00`) and the bay's fishbowl (`l3_bay`). The airlock's handler
  (`l3_air00`) dresses the user's own body in boots and a fishbowl, so it stays local
  until it is made safe for a puppet. `tools/qa_itemused.sh` (level 3 checkpoint 6, the
  client on the bridge room over 300 units from the host) uses the tape on the host: with
  the fix the client's water flag becomes 1, both state dumps hold the same cord item and, when the host collects it, both games move the puzzle on and lose the item;
  with the old behaviour (`MUTANT=noitemused`) the client's flag stays 0 and the cord
  exists on the host only. The same script checks that a door lock a client shoots dead
  (level 2, room 2's exit door) frees the door in both games: that already worked,
  so the bot's "door stayed shut for the client" report was not a replication gap there.

### Enemy hp on clients

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

### Flying enemies on clients

- **Flying enemies stay where the host has them.** The level 2 mantas (and other
  flyers registered the same way) sit on a bone of an animated scenery model, a
  flock flying its loop. While a game object is attached
  to a bone the engine moves it back onto that bone in every scene update, over
  the position the co-op snapshots set, and a client's copy never runs the AI that
  takes the host's flyers off the flock. A client's mantas therefore flew the
  flock's circuit while the host's attacked, up to 230 units apart, so shots,
  hits and targeting on the client used the wrong positions. A client now takes
  its mirrored enemies off the bone and places them by the host's snapshots alone.
  `tools/qa_flyers.sh` (level 2 checkpoint 4, 20 s, the client far from the host)
  measured at most 97.0 units of drift with the old behaviour (`MUTANT=attached`)
  and at most 3.3 with the fix (91.8 and 9.0 at twice the game speed).

### Level 3's boss on a client

- **A client's scripts no longer stop at the level 3 boss.** The boss picks where its psionic blast lands in its own
  AI, which only the host's game runs. On a client the blast's script had no target: a Lua error at every blast, no
  blast on that player's screen, and in one run, after a few of them and two checkpoint restarts, that game's Lua
  answered every call with "too many nested blocks" through levels 4 and 5. The client now reads the target off where
  the boss is. Found by leaving the checkpoint sweep at the boss for two minutes instead of seven seconds; checked by
  `tools/qa_boss3.sh`, whose mutant (no target) fails with the same Lua error. Whether v0.3.5-alpha had it was not
  tested.

### Level 8's opening on a slower client

- **A joining player whose game loads level 8 later than the host's no longer crashes there.** The level opens by
  scaring the cone creatures, and each one removes itself about two seconds later. When the host's removals arrived
  before the client's own copy of that script had run, the client's script called into a creature that was already
  gone (`mdk2hd.exe+0xd055`, reading 0x84). The client now skips a creature that is gone. Seen once, in a checkpoint
  sweep on a busy machine; checked by `tools/qa_cones.sh`, whose mutant crashes at the same address. Other level
  scripts with the same shape have not been searched for.

### Crash guards

- **A position set on a missing object** (a script or test command racing a level change, `mdk2hd.exe+0xdda8`) is
  ignored instead of crashing the game.
- **A stale object handle in the per-frame scene pass** (a shot-up scenery object, `mdk2hd.exe+0x570bd`) is skipped
  the way the pass's first walk already skips it, instead of crashing the host.
  The guard was written against the Overhaul build's address; the Steam build has the same code 0x20 higher
  (`+0x570dd`) and now gets the guard too (before, it was silently off there).
- Both checks verify the game's code bytes first and stay off on an unrecognised build. Checks: `tools/qa_setpos_guard.sh`
  and `tools/qa_scene_guard.sh`, each with an unguarded mutant that must crash at the same site.

## Finding and joining a game

### Joining without typing an address

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
- **Fix: joining an invite-only game no longer logs a refusal first.** When a guest's game was up before the host's,
  the host's first packet from it could be a player packet instead of the invite's key: the host refused it once and
  the guest's log said "This lobby is invite only" before it was let in. The key now goes ahead of every player
  packet until the host answers (`tools/qa_host.sh`, `MUTANT=latekey` must fail).

### Hosting that explains itself

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

### The whole launcher in the MDK2 look

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

## Keys, controller and menus

### Shared co-op keys, the player list and menu names

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

### Co-op from a controller (Steam Deck)

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

### Menu review

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

### Menu review follow-ups

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

## Installing

### Setup that a stranger can run

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
- **Not code-signed yet.** Windows SmartScreen may stop Setup with "Windows protected your PC": click More info, then
  Run anyway, after checking the file's SHA-256 against the `.sha256` file on the release page. The README's
  [code signing policy](../README.md#code-signing-policy) says where signing stands.

### Mod managers

- **Vortex and Mod Organizer 2.** Every release carries a Vortex game extension (`mdk2hd-vortex-extension.zip`) and an
  MO2 game plugin (`game_mdk2hd.py`) for MDK2 HD, so the managers install the release zip with no extra step and start the
  game with the co-op launcher. README, Install > Mod managers. The MO2 side is untested under its virtual file system.

## For developers

### Under the hood

- The game-build test is one shared header (`bridge/build_sigs.h`) used by the DLL at every start and by the installer on
  the exe file on disk.
- The bridge no longer logs a fake "crash" line for the address probes of an unknown build, and removes its exception
  handler when it is unloaded.
- New offline gates: `setup-core`, `linux-install`, `release-signing`, `update-layout`, `release-package`; new live
  gates: the Setup exe checks in `installer` and `build-message`.
