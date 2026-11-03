# Invites and finding friends

This page describes the game-free core that lets a joiner find a host without typing an address.
It covers invite codes, the one paste box, the clipboard check, the LAN/Tailscale/recent-host probe
and the recent-hosts store. The launcher does not use it yet: wiring it into the Join page is a later
change. MDK2 and The Witness share these files (see [Vendoring into The Witness](#vendoring-into-the-witness)).

| File | What | Platform |
|---|---|---|
| `bridge/coop_invite.h` | Invite code v1, the paste box recogniser, the clipboard finder, invite wording | portable C99 / C++11 |
| `bridge/coop_discover.h` | Probe v2 (CPQ2/CPS2) and CPQ1 answers, reply rate limit, candidate addresses, result rows, refresh policy, route choice for invites, `tailscale status --json` parser, recent-hosts store | portable C99 / C++11 |
| `bridge/coop_discover_win.h` | Windows adapters, running `tailscale.exe`, the UDP prober | Windows (Winsock) |

The two portable headers include no platform headers and do no heap allocation. Every input has a
size limit and every parser is transactional or clears its output on error. The Windows adapter
allocates only the adapter list `GetAdaptersAddresses` returns and one attribute list for `CreateProcess`.

## Invite code v1

A code is the game prefix, `-`, and the payload in Crockford base32, in groups of five separated by `-`:

```
MDK2-236TP-D0JZH-P040E-0N00H-80K48-000M3-G
```

| Bytes | Field |
|---|---|
| 0 | version (high 4 bits, = 1) and flags (low 4 bits: bit 0 has a Steam lobby, bit 1 invite only; bits 2-3 reserved, written 0, ignored on read) |
| 1-4 | lobby secret, u32 little-endian |
| 5-6 | port, u16 little-endian (1-65535) |
| 7 | number of routes, 0-4 |
| 8.. | per route: kind (1 LAN, 2 Tailscale or other VPN, 3 internet) and an IPv4 address (4 bytes, network order) |
| .. | if it has a Steam lobby: the lobby id, u64 little-endian |
| last | CRC-8 (polynomial 0x07, initial value 0, no reflection) over every previous byte |

- The host lists the routes best first. The decoder keeps them in that order.
- Base32 uses `0123456789ABCDEFGHJKMNPQRSTVWXYZ`. Decoding ignores case, reads `I` and `L` as `1` and `O`
  as `0`, and skips `-` and white space. Only the canonical length is accepted, and the padding bits must be zero.
- The prefixes are `MDK2` (MDK2) and `WIT` (The Witness), case-insensitive. The text before the first `-` is the prefix.
- The payload is 9 to 37 bytes, so a code has 15 to 60 characters after the prefix.

Errors and their wording (`ci_error_text`):

| Case | Text |
|---|---|
| CRC wrong, too short or long, bad route kind, port 0, padding not zero, bad character | This code is incomplete. Ask for the whole invite again. |
| Another known game's prefix | That invite is for The Witness. (or MDK2) |
| Version above 1 with a valid CRC (not set by the standard) | This invite is from a newer version of the co-op mod. Update to join. |
| Nothing the box recognises (wording not set by the standard) | Not recognised. Paste an invite, a link, a Steam lobby number or an address. |

The copied invite (`ci_message`, C3) reads `Fox is hosting MDK2. Join: https://<join page>/#<CODE>`.
The display name is cut to 32 bytes of printable UTF-8 with no control characters.

## The paste box (B7)

`coop_recognise(text, length, game, &result)` trims white space and tries these in order:

| Order | Recognised | Example | Result |
|---|---|---|---|
| 1 | link with the code in the fragment, or the `<mod>-coop://` handler | `https://<join page>/#MDK2-...`, `mdk2-coop://join/MDK2-...` | `CR_LINK` + decoded invite |
| 2 | invite code: a known prefix, `-`, and at least five code characters (so a computer named `wit-pc` stays a name) | `MDK2-236TP-...` | `CR_INVITE` |
| 3 | Steam lobby number: 17 to 19 digits | `109775241234567890` | `CR_STEAM_LOBBY` |
| 4 | IPv4, optional `:port`, optional `/CODE` (The Witness's current invite form). Leading zeros, port 0 and ports above 65535 are rejected | `192.168.1.20:27900`, `10.0.0.5:47001/AB12` | `CR_ADDRESS` |
| 5 | Tailscale name: ends in `.ts.net`, or a single label (MagicDNS short name), with optional `:port` | `fox-pc`, `fox-pc.tail1234.ts.net` | `CR_TAILSCALE` |
| 6 | host name with at least one dot (or `localhost`), optional `:port` | `example.org:28000` | `CR_HOST` |

`cr_describe` gives the line shown before Join, for example `Invite code: 2 routes, port 27900.`
or `Tailscale name fox-pc.`. IPv6 literals are not recognised.

## The clipboard (B8)

When the launcher gets focus, `ci_find_invite(text, length, game, &invite, name)` looks through the
clipboard text (at most 64 KB) for this game's code. The code must start at a word boundary. Trailing
text joined with `-` is cut back group by group until the code decodes. Codes for other games are ignored.
If the line says `<name> is hosting`, the name is returned for the prompt
`Fox's invite is on your clipboard. Join?`. Without a name the prompt is
`An invite is on your clipboard. Join?`. The clipboard text is read only for this check. It is never
stored, logged or sent.

## Probe v2

UDP on the lobby/game listener's port. All integers are little-endian.

Query, 13 bytes: `"CPQ2"`, nonce u32, game id (4 ASCII: `MDK2`, `WIT `), flags u8 (0, reserved).

Reply, 19 to 97 bytes:

| Offset | Field |
|---|---|
| 0 | `"CPS2"` |
| 4 | nonce (copied from the query) |
| 8 | game id (copied) |
| 12 | state: 0 in lobby, 1 in game, 2 reachable only |
| 13 | players |
| 14 | player limit (1-255, players never above it) |
| 15 | flags: bit 0 invite only, bit 1 version mismatch possible |
| 16 | version length (1-47) then version, printable ASCII without spaces |
| .. | name length (0-32) then the host's display name, printable UTF-8; always 0 when invite only |

- A host answers only queries with its own game id (`cd_answer`). It also keeps answering 8-byte
  `CPQ1` queries with the v1 `CPS1` reply that saved addresses use today. The reply is byte-for-byte
  what `lp_answer` sends, with the limit capped at 16.
- Replies are rate limited per source (`cd_rate_allow`: 10 per second, burst 10, 32 sources tracked)
  and in total (100 per second). A flood of spoofed sources cannot make a host send more than that.
- The joiner (`cd_parse_reply`) accepts a reply only with its own nonce and game id, the exact
  length and valid fields. A reply flagged invite-only that carries a name is rejected.

## Where queries go

A scan builds a candidate set (`cd_candidates`, at most 512 address:port pairs) and sends one query
with a fresh random nonce to each candidate from one socket:

- **LAN and layer-2 VPNs (B1):** `cd_add_adapters` adds the directed broadcast address
  (`address | ~mask`) of every up IPv4 adapter, on the default port and the next three. It is never
  255.255.255.255, which Windows sends out of one adapter only. Skipped: loopback, prefixes shorter
  than /8 or longer than /30 (no broadcast address), and addresses in 100.64.0.0/10 (Tailscale's own
  adapter, whose peers are probed directly). Link-local (169.254/16) subnets are kept for two PCs
  joined by one cable.
- **Tailscale (B2):** each online peer's first IPv4 Tailscale address, on the same four ports.
  Shared-in nodes count. Offline peers, peers without IPv4 and every peer while the backend is not
  `Running` are skipped.
- **Recent hosts (B4):** each recent's address on its remembered port. Host names are resolved by the
  adapter before they are added.
- **Invite routes (C4):** every route in the code, on the code's port, tagged with one group number.

A reply is kept only if it comes from an address somebody asked (`cd_match`): the same address and
port as a directed candidate, or an address inside a broadcast candidate's subnet on that port. When
several candidates match, the best route wins, so a recent host found on the LAN shows as LAN.

**Rows (B5):** `cd_seen` merges sightings into one row per host. Rows match on the same invite
group, or on the same non-empty name, version and port. Each row keeps every route it was heard on
and shows the best fresh one: LAN > Tailscale > Steam friend > Internet > Recent. A route not heard
for 12 s (two refreshes and a scan window) is dropped by `cd_expire`, and a row with no route left
goes. A host running another version gets `mismatch` set and stays listed. Invite-only hosts are
listed only for their invite (B9).

**Refresh (B6):** `cd_refresh_due` asks for a scan as soon as the list is shown and then every 5 s.
It asks for none while the list is hidden, so nothing is sent while nobody looks. One scan listens
for 1.2 s.

**Joining by invite (C4):** all routes are tried at once. `cd_he_choice` takes the first answer, but
waits up to 300 ms after it for a better route (LAN > Tailscale > internet). It decides at once when
the best route tried has answered.

## Tailscale

`cdw_tailscale_status` runs `tailscale status --json` with `CREATE_NO_WINDOW`, stdin and stderr on
`NUL`, and only the pipe handle inherited. It allows 2 s; after that the child is ended through its
own process handle. `tailscale.exe` is looked for in each `PATH` folder, then
`%ProgramW6432%\Tailscale` (the 64-bit Program Files, which a 32-bit process does not see as
`%ProgramFiles%`), then `%ProgramFiles%\Tailscale`. The current folder is never searched.

`cd_ts_parse` reads `BackendState`, `Self` and the `Peer` map, and from each node `TailscaleIPs`,
`HostName`, `DNSName`, `Online`, `ShareeNode` and `OS`. Unknown fields are skipped at any depth up to 32.
Input is limited to 4 MB, and 128 peers are kept (all are counted). Truncated or malformed text
clears the result. `Self.DNSName` without its trailing dot is this PC's Tailscale name for the
lobby panel (C1). The status text is parsed in memory and discarded.

## Recent hosts store (B4)

Text, one header line and one row per host, tab-separated:

```
COOP-RECENTS	1
<label>	<name>	<host>	<port>	<route>	<when>	<pinned>	<notes>
```

`label` is the player's rename (empty = show `name`). `name` is the host's display name when last
joined. `host` is an IPv4 address, a host name or, for the Steam route, the Steam id. `route` is one
of `lan`, `tailscale`, `steam`, `internet`, `address`. `when` is Unix seconds, and `pinned` is 0 or 1.
At most 32 rows, unique by host (case-insensitive) and port, 16 KB in all. `cd_recent_add` refreshes
an existing host:port or adds one, evicting the oldest unpinned row when full. It refuses when every
row is pinned. Rename, pin and forget work on single rows.

`cd_recents_decode` is transactional and also reads the saved-addresses store (`COOP-PROFILES` v0 and
v1, `profiles.tsv`). Each saved address becomes a pinned recent with its saved name as the label,
route `address`, its notes and its last-used time. Writing always produces `COOP-RECENTS 1`.

## Privacy: what is sent where

- The only thing a joiner sends is the 13-byte query: magic, a random nonce, the game id and a zero
  flags byte. It carries no name, address or version.
- Queries go only to directed broadcast addresses of this PC's own subnets, online Tailscale peers,
  recent hosts the player joined before, and the routes of an invite the player pasted. Routers do
  not forward directed broadcasts by default. Nothing is sent to 255.255.255.255, a directory
  server or any other address.
- Scans run only while the Join list is shown.
- A host's reply goes only to the address that asked, at most 10 per second per source. An
  invite-only host's reply carries no name.
- Tailscale peer names stay on this PC. Clipboard text is never stored, logged or sent.
- Recent hosts are kept in the mod's user data folder (as `profiles.tsv` is today), never uploaded.

## Windows adapter

- `cdw_adapters` loads `iphlpapi.dll` from the system folder at run time, so the DLL gains no new
  import. It returns every unicast IPv4 address of every adapter that is up, with its on-link
  prefix length: Ethernet, Wi-Fi, TAP/OpenVPN, WireGuard, Tailscale (Wintun) and Hyper-V switches.
  Only loopback is left out.
- `cdw_scan` opens one non-blocking UDP socket with `SO_BROADCAST`, turns off `SIO_UDP_CONNRESET`
  (an ICMP "port unreachable" must not end the scan), sends the query to every candidate and
  collects matching replies for the window. Pass `bind_ip` 0 in production. The tests bind loopback.

## Testing

`python3 tools/test_coop_discovery.py` is the `discovery-core` gate of `tools/qa_all.py offline`
(`--quick` for fewer fuzz rounds). `make -C bridge test-discovery` runs the same gate.

- `bridge/tests/coop_invite_test.c` and `bridge/tests/coop_discover_test.c` are built with
  AddressSanitizer and UndefinedBehaviorSanitizer and fuzzed (200,000 and 100,000 rounds).
  - Invites: random invites round-trip. Every single-character change is rejected, and so is every
    wrong CRC value. Multi-character corruption is counted (under 0.5 % may decode, as CRC-8 allows).
    Random bytes go into every entry point.
  - Discovery: replies, status JSON and recents are fuzzed. The test also has a model PC with
    several adapters, in which 255.255.255.255 reaches only the first adapter's subnet.
- 41 source mutants must fail the suites. Among them: CRC not checked, prefix not checked, route
  order lost, broadcast only to 255.255.255.255 (loses the second adapter's host), reply accepted
  with a wrong nonce, game id not checked, invite-only host's name in the reply, no rate limit, any
  source accepted, stale routes kept, traffic while hidden, no preference window, offline Tailscale
  peers probed, migration dropped and pinned rows evicted.
- The portable headers are compiled as C++ (g++ and MinGW g++), and the Windows header as 64-bit MinGW C++. Both suites are also built with 32-bit
  MinGW and run on Windows.
- `bridge/tests/coop_discover_win_test.c` (`make -C bridge test-discover-win`) runs on Windows:
  - It lists the adapters and the broadcast candidates and runs the real `tailscale status --json`,
    printing counts only.
  - It probes fake hosts it starts on 127.0.0.1-127.0.0.5: the right game, a second address, the
    wrong game, an invite-only host and one that answers from another address. It also checks the
    invite-route choice and the rate limit over real sockets.
  - Stand-in `tailscale.exe` programs check the fixture output, the 2 s timeout, the
    `%ProgramW6432%` lookup and that the current folder is not searched. Three adapter mutants must fail.
- `coop_discover_win_test.exe --adapters` also binds a fake host on every adapter address and scans
  by directed broadcast. Binding a non-loopback address can make Windows Firewall ask about the test
  program, so this mode is not in the automated gate. Run it at the desk.

## Vendoring into The Witness

The Witness copies `coop_invite.h`, `coop_discover.h` and `coop_discover_win.h` unchanged into its
own tree and records the MDK2 commit it copied. Its C++ code includes them directly; they compile as
C++11, and the gate checks that. Its game id is `WIT ` and its invite prefix `WIT`.

Changes are made here first, with tests and mutants, and then copied again. The copy is never edited
on its own. The Witness may copy `tools/test_coop_discovery.py` and the tests with them;
`coop_discover_test.c` also needs `launcher_profiles.h`, but only for its CPQ1 compatibility check.
The files are MIT-licensed like the rest of this repository.
