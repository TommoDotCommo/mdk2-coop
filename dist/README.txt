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
   - It asks: host or join? If you join, it asks for the host's address.
3. Start MDK2 HD from Steam as usual.

Run Install.cmd again at any time to switch between hosting and joining, or to
change the address.

If Windows shows "Windows protected your PC", click "More info" > "Run anyway".
(The installer is a plain PowerShell script: mdk2-coop.ps1, readable in Notepad.)


How to host
-----------
1. Install and answer "host" (port 27900 unless you have a reason to change it).
2. Forward UDP port 27900 on your router to your PC. Look up your router's model
   plus "port forwarding" for the steps. Only the host needs this.
3. Give your friends your public IP address: search "what is my IP" in a browser.
   (On the same home network, give them your PC's local address instead, which
   looks like 192.168.x.x: run "ipconfig" in a command prompt.)
4. Start MDK2 HD. When Windows Firewall asks about MDK2 HD, click "Allow access"
   (tick both Private and Public networks).
   Missed the prompt? In an administrator command prompt:
     netsh advfirewall firewall add rule name="MDK2 co-op" dir=in action=allow protocol=UDP localport=27900
5. Start a New Game (or load a save). Players who join follow you into your level,
   and you can start before they arrive.

How to join
-----------
1. Install and answer "join", then type the host's address and port (27900).
2. Start MDK2 HD once the host's game is running. You land in the host's level
   next to them. Allow MDK2 HD if Windows Firewall asks.

Playing
-------
- Up to 16 players. Everyone sees the others, enemies are shared, and pickups and
  doors work for everyone.
- When you would die, you come back next to another player instead.
- Space jumps (and works the jetpack and parachute), right mouse toggles sniper mode.

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

Known issues
------------
- The function keys are still developer shortcuts in this build. Avoid them: the host's
  F6 / F7 / F8 jump everyone to the next / previous checkpoint or the next level, F1 and
  F5 spawn enemies, F3 hurts you, F9 opens the developer menu.
- Direct connection only: the host needs port forwarding until Steam networking is in.
- With many players the host uploads a lot (about 20 Mbit/s with 8 in a busy level).
- Nobody has finished the whole campaign in co-op yet. Please report what breaks.

MIT licensed (LICENSE.txt). MDK2 HD itself is not included.
