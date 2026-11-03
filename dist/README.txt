mdk2-coop @VERSION@ - online co-op for MDK2 HD
==============================================

Play MDK2 HD's campaign together: one player hosts, the others join and follow
the host from level to level. Alpha: expect rough edges.
Project page and bug reports: https://github.com/TommoDotCommo/mdk2-coop

You need MDK2 HD on Steam (the current Steam build, 3902). The Overhaul Games (Beamdog client) build is also
accepted but untested. Every player needs their own copy.


Install
-------
Easiest: download the single MDK2-Coop-Setup.exe from the release page (no zip, nothing to extract) and
double-click it. If you have this zip instead:
1. Unzip the whole folder anywhere (not inside the game folder). Do not run Setup from inside the zip viewer:
   it says "Extract the whole zip first" because the files beside it are missing.
2. Double-click MDK2-Coop-Setup.exe and choose Install, Update or Repair as offered by its status check.
   - It finds MDK2 HD in your Steam libraries or the Overhaul Games folder (or asks for the
     folder) and shows what it found: "Found: MDK2 HD Steam build 3902", "Found: MDK2 HD Overhaul Games build" or
     "Found: unknown build (SHA-256 ...)". An unknown build gets a warning that co-op will stay off, and is
     installed only if you confirm. Windows asks for administrator permission if the game folder is protected.
   - That is all it asks: host or join, Steam or a direct address are chosen in the launcher.
3. Choose Play MDK2 or Close when setup finishes. You can also start MDK2 HD from
   Steam or the Beamdog client as usual. The co-op launcher opens first.

If the game starts but co-op is off and a message says the game build is not known: in Steam, right-click MDK2 HD >
Properties > Betas > choose None, then Installed Files > Verify integrity of game files. The message (and the exe's
SHA-256 for a bug report) is also saved as %LOCALAPPDATA%\mdk2-coop\coop\unsupported-build.txt.

Another mod's dinput8.dll (an ASI loader, a widescreen fix) keeps working: Setup keeps it as dinput8.chain.dll, which the
co-op DLL loads right after itself (the game's DirectInput goes through it when it is a full dinput8 proxy). Uninstall
puts it back as dinput8.dll. Setup from before this version kept such a file as dinput8.dll.coop-original, where nothing
loaded it; the next Install or Update moves it to dinput8.chain.dll.

Nothing is written into the game folder while you play: settings, logs, diagnostics and the updater's backups live in
%LOCALAPPDATA%\mdk2-coop. Files an older version left in <game folder>\coop (coop.ini, saved addresses, ...) are moved
there once at the first start; old dinput8.dll.previous backups too.

Mod managers (Vortex, Mod Organizer 2) and installing by hand
-------------------------------------------------------------
The zip's top folder mirrors the game folder for the two files the game needs: dinput8.dll and coop\coop.lua. Copying them
into the game folder is a complete install, deleting them a complete uninstall; the mod adds its loader line to
%LOCALAPPDATA%\MDK2HD\settings.lua by itself at the first start (the line does nothing without the mod and may stay).
coop-mod.json (at the zip root and in its top folder) says what the package is and lists those files with their SHA-256.
    dinput8.dll       ->  <game folder>\dinput8.dll
    coop\coop.lua     ->  <game folder>\coop\coop.lua     (create the coop folder if needed)
If another mod's dinput8.dll is already there, rename it to dinput8.chain.dll first: the co-op DLL loads it.
Setup recognises files a mod manager put there (the hashes of a release, without Setup's receipts) and shows "Installed by
a mod manager" with Play; it does not change them. The in-game updater then says "Update it in your mod manager" (in
Vortex: download the new version and enable it) with a link to the release page, and does not replace those files.

Setup from scripts
------------------
MDK2-Coop-Setup.exe (and mdk2-coop.ps1, which it runs; install-linux.sh on Linux takes the same words):
    MDK2-Coop-Setup.exe status --json [<game folder>]     what is installed and who installed it (JSON; without a folder:
                                                           every MDK2 HD found)
    MDK2-Coop-Setup.exe install "<game folder>" --yes      install or update, no questions
    MDK2-Coop-Setup.exe uninstall "<game folder>" --yes    take out exactly what Setup put in, put back the originals
Exit codes: 0 done, 1 failed, 2 usage, 3 unsupported game build (add -AcceptUnknownBuild to install anyway), 4 the game
is running from that folder. Setup is a Windows program: to get its exit code from a prompt, run it with
start /wait (cmd) or Start-Process -Wait -PassThru (PowerShell), and redirect its output to a file for the JSON.
The older form still works: powershell -File mdk2-coop.ps1 -GameDir "<game folder>" -StatusOnly -Yes

Steam Deck / Linux (Proton) - untested
--------------------------------------
Proton has its own built-in dinput8, so the game must be told to load the mod's file first. In this unzipped folder run:
    sh install-linux.sh install "<game folder>" --yes
(the game folder is the one with mdk2hd.exe, e.g. ~/.local/share/Steam/steamapps/common/"MDK 2 HD"). It copies the two
files, keeps another mod's dinput8.dll as dinput8.chain.dll (any other file that was there as <file>.coop-original), notes
what it did in coop/install-linux.record and, when the game's Proton prefix exists (it does after the first start), writes
the DLL override for mdk2hd.exe into steamapps/compatdata/213350/pfx/user.reg (a backup first): no launch option to type.
Without a prefix it prints the launch option instead; you can also set it by hand: right-click MDK2 HD > Properties >
General > Launch Options:
    WINEDLLOVERRIDES="dinput8=n,b" %command%
    sh install-linux.sh status --json ["<game folder>"]      what is installed (and whether the override is in place)
    sh install-linux.sh uninstall "<game folder>" --yes      removes only the files it installed, and its override
Exit codes as for Setup above. Not tested on a real Steam Deck yet: please report how it went at
https://github.com/TommoDotCommo/mdk2-coop/issues

The launcher
------------
Before the game starts you choose Host a game, Join a friend or Play solo (plain
MDK2 HD, co-op off for that start), Steam or direct UDP, the host's address or
lobby ID and your name. The Saves tab lists your co-op saves (who played, how far)
and lets the host start at any level. The Settings tab has every option, in four
sections: Co-op, Interface, Voice and sound (including the game's own effects,
music and speech levels), Display. The About tab has the version and links; the Help tab
makes the diagnostics zip for a bug report. Holding F1 in the launcher or in game
opens the full-screen 3-column Mod Quick Guide (keybinds, co-op features and how co-op works).
Enter starts the game, Esc quits.
Don't want it? Settings > Display > Show launcher at start: Off. Hold Shift while
the game starts to bring it back. Accepting a Steam invite skips it and joins
straight away.

Join over UDP and Host first open a lobby: everyone who has connected is listed with
their name, ready state and ping, and the host presses Start when the roster looks right.
Use Save address inside a lobby to keep the host address and port for next time.
Saved addresses preserve your names and notes; Join opens the connected lobby directly.
Leave lobby returns to connection selection.

If Windows shows "Windows protected your PC", click "More info" > "Run anyway": Setup is not code-signed yet.
(The installer uses PowerShell scripts, readable in Notepad: MDK2-Coop-Setup.exe /extract <new folder> unpacks them.)


Couch co-op (2-4 players on one PC)
-----------------------------------
After installing, double-click Couch.cmd (or run it with -Players 3, -Monitor 2, -Stop). It starts one
game window per player on one screen, each played with its own Xbox-style gamepad (player 1 = first pad).
Player 1 starts the game from the title screen; the others follow. Only player 1 has sound.
Left stick move, right stick look, right trigger fire, left trigger sniper, A jump, X hold to revive,
Y inventory, Back ping, bumpers change item, B/Start pause. Each window is a full game: 4 players need a strong PC.

How to host (Steam)
-------------------
1. Install, choose "steam", then "host". Keep Steam running and online.
2. Start MDK2 HD from Steam. Open Options > Co-op > Session > Invite Steam friends.
   Or share the numeric Lobby ID shown on that page.
3. Start a New Game or load a save. Friends follow you into your level.

How to join (Steam)
-------------------
1. Install, choose "steam", then "join". Start MDK2 HD from Steam.
2. Accept a Steam invite, choose Join Game on the host in Steam's friends list,
   or use Options > Co-op > Session > Join lobby by ID.

No port forwarding or public IP is needed. Everyone needs Steam online, the Steam
transport, and a compatible mod version. If the overlay is disabled, share the
lobby ID. Start the game before accepting invites. The mod uses MDK2 HD's own
Steam connection and AppID; do not replace steam_api.dll or add an AppID override.
Errors and lobby status appear on the Session page and in %LOCALAPPDATA%\mdk2-coop\coop\bridge.log.

Direct UDP (optional)
---------------------
Choose "udp" at install for LAN/testing or direct internet connections. The host
forwards UDP 27900 (or their chosen port) on their router for internet play and
shares their public IPv4 address and port. On a LAN use their local IPv4 address.
Joining players enter that address and port. Allow MDK2 HD through Windows Firewall.
Once joined, wait for the host; the game loads automatically. Esc cancels joining.
Session > Join host again retries. Only the host needs to choose a game.
Older coop.ini files without a transport key continue to use UDP.

Playing
-------
- Up to 16 players. Everyone sees the others, enemies are shared, and pickups and
  doors work for everyone.
- What one player does happens for everyone: when anyone walks into a new room or picks something
  up, the level's scripts run for all of you, and a boss starts when anyone gets near it. If a door
  closes behind them, anyone left outside the area, or on the far side of the door, is brought to
  them (a note says so). Everyone can take their own tutorial grenades.
- When you would die you go down instead: a teammate can stand you up by holding R
  next to you for 3 seconds. After 30 seconds down you are out: you watch a teammate
  (click for the next one) and come back when your area is clear of enemies or at
  the next checkpoint. Lava, black holes and falling out of the world are straight
  out. If everyone is down, the checkpoint restarts for everyone.
- Cutscenes start when everyone is ready. Backspace (or Esc) votes to skip: a majority
  skips for everyone, pressing again takes your vote back. Press any other key to see
  the time left and the votes in the bottom right corner. If you finish a cutscene
  first, you wait for the others.
- If someone's game crashes or their connection drops, the others wait and they come
  back where they were. If the host's game crashes, the host picks Resume co-op in
  the Load Game menu and everyone follows.

Keys (the Modern preset, the default; change them in Options > Co-op > Controls)
--------------------------------------------------------------------------------
- WASD move, Space jump (jetpack, parachute), left mouse fire, right mouse sniper mode,
  Q / E or the mouse wheel change item, F opens the inventory.
- T chat (Enter sends, Esc cancels), V or middle mouse ping, hold R next to a downed
  teammate to revive, hold B to talk.
- Hold H for the emote wheel (its last slot, All..., lists every animation), hold J
  for the character wheel (switch to Kurt, Max or Doc at any time). Point and let go.
- Backspace votes to skip a cutscene. Hold F1 (in the launcher or in game) for the Mod
  Quick Guide. F2 puts a marker in the logs when you want to report something.
- A gamepad works too: left stick move, right stick look, right trigger fire, left
  trigger sniper, A jump, X revive, Y inventory, Back ping, bumpers change item.

Uninstall
---------
Open MDK2-Coop-Setup.exe and choose Uninstall. It removes owned mod files, restores verified originals
and removes the loader while keeping current player options. Co-op
settings, logs and saves in %LOCALAPPDATA%\mdk2-coop\coop stay; delete that folder
if you like. Older installs may also have data in the game folder's coop subfolder.

What the installer changes
--------------------------
- <game folder>\dinput8.dll (the mod; another mod's dinput8.dll is kept as dinput8.chain.dll,
  which the co-op DLL loads, an older copy of this mod as dinput8.dll.coop-original; either is put
  back on uninstall)
- <game folder>\coop\coop.lua and the per-user-data marker; replaced originals
  use .coop-original and deployed files have .coop-receipt.json hash receipts
- %LOCALAPPDATA%\mdk2-coop\coop (settings, logs and co-op data; existing game-folder
  data is copied here on install without replacing files already present) and
  %LOCALAPPDATA%\mdk2-coop\update (the in-game updater's backups for Roll back)
- one loader line at the end of %LOCALAPPDATA%\MDK2HD\settings.lua (backed up first
  as settings.lua.coop-backup). The game rewrites that file when you change options;
  the mod puts the line back at the next start.

Setup refreshes status on activation or Refresh. Modified files and damaged backups
block replacement. Uninstall keeps files without ownership receipts. Legacy
.coop-backup files are preserved and need review before replacement. Keep backups
and receipts until uninstall completes. Recovery is per file; a failed operation
may need repair before the installation is complete.

Settings in the game
--------------------
Options > Co-op (on the title screen, or Esc > Options while playing):
- Players: who is in the game, with ping.
- Session: your name, startup role and transport (restart to apply), Steam lobby
  status/ID, invites and joining, or the UDP host address. On the host: Resume co-op
  and Save now. Change lobbies from the title screen. If a Steam lobby closes,
  clients can accept a new invite or press Esc on the waiting screen to join by ID.
- Gameplay (the host's settings count): friendly fire, difficulty (Story, Normal, Hard,
  Nightmare, or enemy health, damage and revive times one by one), how often co-op saves
  are made, back to the level's hero after a level you switched hero in, and developer
  keys (leave off: the F-keys are developer shortcuts that change the level for
  everyone; F2 always puts a marker in the logs).
- Interface (your own): players on screen (names over heads and a panel with
  everyone's health), outlines of the other players through walls (off by default),
  the cutscene timer, friends in minigames, and the emote wheel, character wheel,
  text chat and pings, each of which can be switched off.
- Controls: pick Original (the game's own keys), Modern (WASD, Space jumps, right mouse
  snipes, Q / E cycle items, F opens the inventory) or Custom. A picture of the keyboard and
  mouse shows every key and what it does, one page per situation: on foot, Kurt, sniper
  mode, Max, Doc, minigames, co-op (chat, ping, revive, push to talk, the wheels, skip
  cutscene), gamepad, menus and system. Select an action and press a key to change
  it; the first change starts Custom as a copy of the preset you were on. Delete clears an
  action. Your Custom keys are kept separately, so Original and Modern are always there.
- Voice chat: hold B (changed on the Controls page) and players near you hear you. Turn it
  off, set its volume, and on the host choose how far voices carry.

Known issues
------------
- Steam networking passed offline regression tests and a real lobby create/leave
  smoke test with the original game DLL. Live two-account invites, relay connectivity
  and gameplay still need validation. Existing campaign QA used UDP.
- Host migration is not supported. A restarted host creates a new lobby; join its
  new invite or ID to resume. Transient P2P failures reconnect within the same lobby.
- The host uploads about 1.3 Mbit/s with 8 players in a busy level (less with fewer):
  check the host has that much upload to spare.
- Nobody has finished the whole campaign in co-op yet. Every checkpoint loads and
  plays in automated tests, but those don't solve puzzles or beat bosses.
- Steam Deck / Linux (Proton) is untested: the launch option and install-linux.sh were only
  checked against fixture folders.
- A game build the mod does not know (for example a Steam beta branch) keeps co-op off, with a message.
- Setup is not code-signed yet, so Windows SmartScreen warns before the first run.

Help test
---------
This is an alpha. These help the most: Steam invites between two different accounts,
long sessions over the internet, finishing levels together (puzzles, bosses), and a real
gamepad, microphone or 3-4 couch players. A session that went well is worth a playtest
report too.

Reporting a problem
-------------------
Open a bug report (or a feature request): https://github.com/TommoDotCommo/mdk2-coop/issues/new/choose
- Press F2 the moment something goes wrong: a numbered marker goes in every player's
  log, with a screenshot (coop_mark<N>.tga alongside the log), and every game saves the
  last minute it saw (frame rate, positions, ping, lost packets) next to its log.
- Attach the diagnostics bundle, from everyone if you can, the host's first. In the
  launcher open Help > Send diagnostics: a few seconds later a zip is on your desktop
  with the logs of recent runs, your settings and a summary of your PC and network. It
  also works after a crash: start the game again and use it. Your Windows and computer
  names, IP addresses and Steam IDs are replaced before it is zipped; it holds no saves
  and no game files. Every run keeps its own log, so playing again does not overwrite
  anything (the newest 200 MB are kept; a run that crashed is never deleted).
  Without the launcher the logs are in %LOCALAPPDATA%\mdk2-coop\coop\logs (older
  installs and test copies: the coop folder in the game folder); bridge.log is the last run.
- Say which player you were (host or client), the mod version (launcher's About tab),
  Steam or UDP, and the level and checkpoint.

MIT licensed (LICENSE.txt). MDK2 HD itself is not included.

Combat feedback: coloured floating damage numbers (rapid hits merge), a hitmarker, a kill
ticker and a damage panel at the right edge. The panel starts as one line; Ctrl+M expands it to a
bar per player. Ctrl+R resets local counters, Ctrl+D dumps combat.txt (also saved automatically).
Recent history is bounded to 512 events.

Already installed? Open setup and click Play MDK2. Create desktop shortcut adds
a shortcut to the game and its co-op launcher. Keep Steam or the Beamdog client
running. The launcher shows the result of a successful update check.
