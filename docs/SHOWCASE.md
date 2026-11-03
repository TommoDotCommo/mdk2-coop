# mdk2-coop showcase

Online co-op for **MDK2 HD**: one player hosts, the others join, and you play the campaign together.
These are real screenshots from the current build, taken from automated test runs: the player names are the test harness's, and the lobby picture
has this PC's addresses and invite code hidden. [Download](https://github.com/TommoDotCommo/mdk2-coop/releases)
· [Player guide](../README.md)

![Three players in the first level: the players panel at the top left, name tags over the others](showcase/players.jpg)

## Play together

Everyone follows the host from level to level, and enemies, doors, pickups and shots are the same for everyone.
The panel at the top left shows each player's health, hero, distance and ping, with name tags over their heads.

![Level 2 as Max with two teammates](showcase/max-level2.jpg)

## A launcher before the game

The launcher opens first. **Play** has your name and three choices: **Host a game**, **Join a friend** and
**Play solo**, with one big button that says what it will do (here **Open lobby**). The **Saves** tab lists your
co-op saves with who played and how far you got, and lets the host start at any level and checkpoint.

| Play | Saves |
|---|---|
| ![The launcher's Play tab: Host a game, Join a friend, Play solo, and the Open lobby button](showcase/launcher.jpg) | ![The Saves tab with two co-op saves and the new game chooser](showcase/saves.jpg) |

## Find a friend, or paste their invite

**Join a friend** looks for hosts by itself: it lists the lobbies it finds on your network (and on Tailscale, and
Steam invites), each with its state, players, the way it was found and the ping. Pick one and press **Join**. A box
above the list takes an invite code or link, an address, a Steam lobby number or a host name. When an invite is on
your clipboard as the launcher comes to the front, it offers to join.

| The list finding two hosts | An invite on the clipboard |
|---|---|
| ![Join a friend: two lobbies listed with LAN and ping](showcase/launcher-join.jpg) | ![The launcher offering to join the invite on the clipboard](showcase/launcher-invite.jpg) |

## The lobby

The host's lobby lists the players with their hero, ready state and ping, with chat beside it. **Friends can join
by** checks each way in (Steam, same network, Tailscale, the internet, Windows Firewall) and says what it found and
how to fix it; **Copy invite** puts one line with a code on the clipboard (the code shows under the button, hidden
here). The host chooses **Open to my networks** or **Invite only**, and **Start game** lights up when everyone is
ready.

![The host's lobby with two players, Friends can join by and the Start game button](showcase/lobby.jpg)

## Down, not dead

A hit that would kill you puts you down. A teammate holds **R** next to you to stand you up; bleed out and you
watch the others until your area is clear or the next checkpoint.

| Down | Reviving |
|---|---|
| ![Doc Hawkins is down: get next to them to revive](showcase/downed.jpg) | ![The revive bar filling over a downed teammate](showcase/reviving.jpg) |

## Talk and point

Text chat (**T**) shows over the sender's head and in the corner, with dots while someone types. Pings (**V** or
middle mouse) mark a spot for everyone, and proximity voice chat (push to talk **B**, or the mouse side button) lets
nearby players hear you.

![A chat bubble over a teammate's head: over here, the door is open](showcase/chat.jpg)

## Switch hero, strike a pose

Hold **J** for the character wheel and play as Kurt, Max or Doc at any time. Hold **H** for the emote wheel, with
the animations your hero has and an "All..." list of the rest.

| Character wheel | Emote wheel |
|---|---|
| ![The character wheel: Kurt, Max, Doc and the level's own hero](showcase/character-wheel.jpg) | ![The emote wheel with Ready, Throw, Knockdown and All...](showcase/emote-wheel.jpg) |

## Gear your friends carry

What a player uses, the others see: Max's jetpack on his back, and Doc's leaf blower once he has taken it up.
(Kurt's parachute is shared the same way; it has no picture here yet.)

| Max's jetpack | Doc's leaf blower |
|---|---|
| ![Max with his jetpack, seen by the host](showcase/jetpack.jpg) | ![Doc with the leaf blower, seen by the host](showcase/leafblower.jpg) |

## Cutscenes and minigames together

Cutscenes start when everyone is ready, skipping is a vote (**Backspace**), the time left and the skip key show in
the corner, and the next part waits for everyone. In the skydives and torpedo runs the other players fly beside you as
ghosts.

![A comic-book cutscene with the time left and the skip key in the corner](showcase/cutscene.jpg)

| Skydive | Torpedo run |
|---|---|
| ![A skydive towards the city](showcase/skydive.jpg) | ![The other player's ships as ghosts in the level 9 torpedo run](showcase/torpedo.jpg) |

## In the game: Options > Co-op

The game's own **Options** menu has a **Co-op** entry. Its **Session** page shows who you are and how you are
connected, and has **Save now** (host), **Copy invite** and **Leave**.

![Options > Co-op > Session: name, role, the port, Start at level, Save now, Copy invite and Leave](showcase/session.jpg)

## Your controls, your rules

Modern (WASD) or the game's original keys, with a page that draws the keyboard and mouse and labels every key, and
Xbox-style gamepads. Difficulty presets or your own mix, and a **Players and interface** page to switch each feature
on or off.

![The Controls page drawing the keys on the keyboard](showcase/controls.jpg)

| Gamepad | Difficulty | Players and interface |
|---|---|---|
| ![The gamepad page](showcase/gamepad.jpg) | ![The difficulty page: enemy health, damage and revive timers](showcase/difficulty.jpg) | ![The Players and interface page with a switch for each feature](showcase/interface.jpg) |

## On a gamepad

**View** (Back) is the co-op button. Hold it for the player list; while it is held, D-pad up opens the emote wheel and
**RB** the quick chat (Ready, Wait for me, Over here, Come look at this, Thanks): point with the right stick and let
go of View to choose. A tap of View pings what you aim at.

| Hold View: the player list | View + RB: quick chat |
|---|---|
| ![The player list opened with View, with Mute and volume](showcase/pad-list.jpg) | ![The quick chat wheel with Ready picked](showcase/pad-quickchat.jpg) |

---

This is a beta in progress: see [Help test](../README.md#help-test) for what real players can check that the automated
tests can't, and [report what breaks](../README.md#reporting-a-problem).
