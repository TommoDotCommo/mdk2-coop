mdk2-coop @VERSION@ - online co-op for MDK2 HD
==============================================

Play MDK2 HD's campaign together: one player hosts, the others join and follow
the host from level to level. Alpha: expect rough edges.
Project page and bug reports: https://github.com/TommoDotCommo/mdk2-coop

You need MDK2 HD on Steam (the current Steam build, 3902). Every player needs their own copy.


Install
-------
1. Unzip this folder anywhere (not inside the game folder).
2. Double-click Install.cmd.
   - It finds MDK2 HD in your Steam libraries (or asks for the folder) and checks the
     game version. If the version is different it stops and changes nothing.
   - Choose steam (the default for new installs) or udp, then host or join.
3. Start MDK2 HD from Steam as usual. The co-op launcher opens first.

The launcher
------------
Before the game starts you choose Host a game, Join a friend or Play solo (plain
MDK2 HD, co-op off for that start), Steam or direct UDP, the host's address or
lobby ID and your name. The Saves tab lists your co-op saves (who played, how far)
and lets the host start at any level. The Settings tab has every option, in four
sections: Co-op, Interface, Voice and sound (including the game's own effects,
music and speech levels), Display. The About tab has the version and links.
Enter starts the game, Esc quits.
Don't want it? Settings > Display > Show launcher at start: Off. Hold Shift while
the game starts to bring it back. Accepting a Steam invite skips it and joins
straight away.

You can also run Install.cmd again to switch between hosting and joining, or to
change the transport or address. Existing installs keep their previous transport.

If Windows shows "Windows protected your PC", click "More info" > "Run anyway".
(The installer is a plain PowerShell script: mdk2-coop.ps1, readable in Notepad.)


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
Errors and lobby status appear on the Session page and in coop\bridge.log.

Direct UDP (optional)
---------------------
Choose "udp" at install for LAN/testing or direct internet connections. The host
forwards UDP 27900 (or their chosen port) on their router for internet play and
shares their public IPv4 address and port. On a LAN use their local IPv4 address.
Joining players enter that address and port. Allow MDK2 HD through Windows Firewall.
Older coop.ini files without a transport key continue to use UDP.

Playing
-------
- Up to 16 players. Everyone sees the others, enemies are shared, and pickups and
  doors work for everyone.
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
- Backspace votes to skip a cutscene. F2 puts a marker in the logs when you want to
  report something.
- A gamepad works too: left stick move, right stick look, right trigger fire, left
  trigger sniper, A jump, X revive, Y inventory, Back ping, bumpers change item.

Uninstall
---------
Double-click Uninstall.cmd. It removes the mod's DLL and coop.lua from the game
folder and restores your game settings from the backup made at install. The game
folder's "coop" subfolder (co-op settings, logs, co-op saves) stays; delete it if
you like.

What the installer changes
--------------------------
- <game folder>\dinput8.dll (the mod; any earlier dinput8.dll is kept as
  dinput8.dll.coop-backup and put back on uninstall)
- <game folder>\coop\coop.lua and coop\coop.ini
- one loader line at the end of %LOCALAPPDATA%\MDK2HD\settings.lua (backed up first
  as settings.lua.coop-backup). The game rewrites that file when you change options;
  the mod puts the line back at the next start.

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
  plays in automated tests, but those don't solve puzzles or beat bosses. Please
  report what breaks: what happened, your coop\bridge.log (it has the players' names
  and, over UDP, their IP addresses), and press F2 when it happens.

MIT licensed (LICENSE.txt). MDK2 HD itself is not included.
