# Hosting: the lobby that explains itself

When you host over direct UDP, the launcher's lobby checks every way a friend can reach you, says
what it found and how to fix it, and makes one invite that carries every route that works. This page
describes that host side. The join side (the paste box, the list of games found) is in
[DISCOVERY.md](DISCOVERY.md).

| File | What | Platform |
|---|---|---|
| `bridge/launcher_host_core.h` | NAT-PMP and UPnP-IGD messages, the router's WAN address class (CGNAT), firewall rule verdicts, invite routes, the hello's lobby key, kicks, refusals, probe visibility, "Waiting for", pad actions | portable C99 (also C++11) |
| `bridge/launcher_host_win.h` | UPnP-IGD then NAT-PMP mapping with renewal and removal, the read-only Windows Firewall reader (`INetFwPolicy2`) | Windows (Winsock, COM loaded at run time) |
| `bridge/launcher_host.c` / `.h` | The worker thread per lobby, the panel rows, the invite text, admission, the pad | Windows, in the DLL |
| `bridge/launcher.c` | Draws the lobby and the pages; calls the above | Windows |

## The lobby (standard D1-D3)

The lobby takes the whole window, in three columns:

1. **Players**: colour, host mark, speaking light, ready state and ping, Mute, and for the host **Kick**.
   **Leave lobby** is always one click.
2. **Chat**.
3. **Friends can join by** (host only), then **What to play** (the host's save or level; the Saves tab
   chooses it). The host's big button reads **Waiting for Fox and Bob** until everyone is ready, then
   **Start game**. A joiner's button reads **Ready** or **Not ready**.

The tabs stay usable while a lobby is open; the Play tab reads **Lobby**.

## Friends can join by (C1)

| Row | Check | When it passes | The fix it shows |
|---|---|---|---|
| Steam | Not used by a UDP lobby | (always "-") | Choose Connection: Steam on Play; friends use Join game in Steam |
| Same network | The lobby is asked (CPQ2) on this PC's LAN address and must answer | "This PC: 192.168.1.20, port 27900" | Another program may hold the port: pick another in Settings > Network |
| Tailscale | `tailscale status --json` (see DISCOVERY.md) | "This PC: fox-pc.tailnet.ts.net (100.x.y.z)" | Not installed: optional. Installed but not connected: start it |
| Internet | The router mapping below, and the router's WAN address | Mapped and the WAN address is public | Turn on UPnP or NAT-PMP, or forward the port yourself, or use Steam or Tailscale; for CGNAT: "Your internet provider shares your address with other customers ..." |
| Windows Firewall | Inbound UDP rules for the game exe, on the Private and Public profiles | Allowed (or the firewall off) on both | Setup can add the rule; or allow MDK2 when Windows asks, on Private and Public networks |

The checks run on one worker thread per lobby, in that order of cost: adapters and the self-probe (about
a second), the firewall rules, Tailscale (at most 2 s), then the router (at most a few seconds). The rows
say "Checking..." until each one is known. Every finding is also logged (`host: ...` lines in `bridge.log`).

## The router port (C2)

While a lobby is open, the host asks the router to forward its UDP port to this PC:

1. **UPnP-IGD**: an SSDP search (multicast, TTL 2) for an InternetGatewayDevice, its description over HTTP,
   then SOAP `AddPortMapping` on `WANIPConnection:2`, `:1` or `WANPPPConnection:1` for this PC's address,
   with a one-hour lease, and `GetExternalIPAddress`. A router that only keeps permanent mappings (error 725)
   is asked again with lease 0.
2. Otherwise **NAT-PMP** (RFC 6886) to the default gateway: the external address, then a UDP mapping.

The mapping is renewed at half its lease. **Leave lobby** and quitting the launcher remove it
(`DeletePortMapping`, or a NAT-PMP mapping with lifetime 0). When the lobby starts the game, the game takes
over the port and the mapping is kept and renewed while the game runs; when the game closes, the router drops
it when its lease ends (at most an hour later). A router that only takes permanent mappings keeps that one
until the host's next lobby removes it.

A WAN address in 100.64.0.0/10 is carrier-grade NAT; a private one means another router in front. Either way
the internet route is left out of invites and the panel says to use Steam or Tailscale.

Setting: **Settings > Network > Open the router port while hosting** (on by default; `[network] upnp`).

## The firewall check

`lhw_firewall` reads every rule through `INetFwPolicy2` (read only: nothing is changed). It keeps the
enabled inbound rules for the game's exe, and the rules for every program that name the lobby's port. A
matching block rule wins over an allow rule, as in Windows. A profile with the firewall off passes. TCP-only
rules do not let UDP in. Adding the rule is Setup's job.

## The invite (C3) and visibility (B9)

**Copy invite** (or X on a pad) puts this on the clipboard:

```
Fox is hosting MDK2. Join: https://tommodotcommo.github.io/coop-mod-kit/join/#MDK2-XXXXX-...
```

The code (invite v1, see DISCOVERY.md) carries the lobby's port, a random lobby secret, and the routes best
first: up to two LAN addresses (the one towards the internet first), the Tailscale address when connected,
and the internet address only when the router check passed and **Put my internet address in invites** is on.
The code is also shown under the button so it can be read out.

**Who can see my lobby** (lobby button, and Settings > Co-op rules):

- **Open to my networks**: the lobby answers CPQ1 and CPQ2 probes with its name; anyone who finds it joins.
- **Invite only**: the lobby does not answer CPQ1 (so it is not listed by saved addresses), answers CPQ2
  with the invite-only flag and no name, and admits only a hello that carries the invite's secret.

## The join handshake

A joiner's hello is `MDKL H <id> <ready> <rtt> <ts> <version>[ <key hex>]|<name>`. The key is the invite's
secret; it is sent when the joiner used an invite. Hosts of earlier versions ignore it.

For a new player the host decides (`lh_admit`): refused when kicked from this lobby, when the lobby is invite
only and the key is missing or wrong, or when it is full. A refused joiner gets `MDKL K <reason>`
(1 removed, 2 invite only, 3 full) and shows the words:

- "The host removed you from this lobby."
- "This lobby is invite only. Ask the host for the whole invite."
- "The lobby is full."

A joiner with an invite sends its hello to every route at once and keeps the first that answers; that address
becomes the game's `peer_host`.

**Kick** removes the player at once and refuses them for the rest of that lobby: by launcher id, by address
and port, and by name (a rejoin from the same launcher has a new id and port). Another player on the same PC
is not affected. A new lobby starts with no bans. The lobby is the only gate: the game's own listener does not
check the secret, so a player who skips the lobby (launcher off) can still connect to the game port.

## The pad (K3)

The launcher reads the first connected XInput pad while its window has the focus:

| Pad | Does |
|---|---|
| Left stick or D-pad | Moves the focus (held: repeats after 400 ms, then every 120 ms); left and right change a value |
| A | Presses the focused control |
| B | Back to Play (never quits or leaves) |
| LB / RB | Previous / next tab |
| Start | The primary button (Play, Start game, Ready) |
| X | Copy invite (host's lobby) |
| Hold Y | Quick guide |

The footer shows pad buttons after pad input and keys after keyboard or mouse input.

## Page structure (A1, E, F2, F3)

- Four tabs: **Play, Saves, Settings, About**. Help is part of About: version, updates (check, install,
  roll back), the quick guide, **Report a problem** (Send diagnostics, Report a bug), links and credits.
- An available update shows a banner on Play (**Update**, **What's new**). It never shows over a lobby.
- Settings sections, in order: Co-op rules, Players and interface, Voice and sound, Controls, Display,
  Network (port, find games on my network, find friends on Tailscale, open the router port while hosting,
  put my internet address in invites), Launcher (show at start, check for updates, offer pre-releases,
  developer keys). Network rows are saved when they change (`[network]` in `coop.ini`).

## Testing

- `python3 tools/test_launcher_host.py` (offline gate `launcher-host`): the core test with sanitisers and
  fuzzing, the core compiled as C++, 26 core mutants (among them: the secret not checked, invite only
  ignored, invite only still listed, kicks forgotten or by id only, CGNAT not detected, the internet route
  without its check, block rules not winning, the Public profile not needed, the pad's Start not primary).
  With Windows interop, the router code runs against a fake IGD and NAT-PMP responder on loopback (mapped,
  renewed, permanent-lease fallback, NAT-PMP fallback with a CGNAT address, silence) and four mutants must
  fail, among them **mapping not removed on close**. The firewall reader reads the real rules (read only).
- `tools/qa_host.sh` (feature gate `hosting`, two games): the panel's Same network check, the router asked and
  its answer recorded (and removed on Leave when it mapped), the client joining with the invite code only,
  invite only refusing an address without the code, Kick, the pad from tabs to Start game, and the game
  starting with the client joined. Everything is asserted from `launcher-state.ini`.
