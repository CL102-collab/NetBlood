# Rednukem multiplayer

The Multiplayer menu offers Internet and ZeroTier connections. The
[ZeroTier guide](zerotier.md) covers the bundled connection, shared-network game
browser, and password-protected graphical hosting without forwarded ports.

Rednukem uses an authoritative UDP client/server protocol for Duke Nukem 3D,
Redneck Rampage, and Redneck Rampage Rides Again.
The server runs gameplay at 30 ticks per second. Clients send inputs, predict
local movement, and receive
validated world snapshots; they do not run a second copy of the gameplay
simulation. Players can join after a level has started.

This protocol is separate from NBlood's protocol and from the original
lockstep multiplayer protocol. Everyone must use a compatible Rednukem build
and the same base game edition. Duke Nukem 64, NAM/NAPALM, World War II GI,
and Deer Huntin' are outside this multiplayer port's scope. Their existing
offline modes remain available.

The automated live-game checks use Duke shareware assets with generated test
maps and CON scripts. RR/RRRA state paths have been reviewed
in source, but their commercial game assets were unavailable for runtime
validation. The same limitation applies to Atomic Edition's episode 4 intro.

## Hosting and joining

The same executable provides both a graphical host and a dedicated server:

```sh
# Dedicated co-op server, up to eight players.
./rednukem -dedicated -server 8 -gametype coop -v1 -l1 -port 23513

# Dedicated team game with a score limit and map rotation.
./rednukem -dedicated -server 8 -gametype teams -scorelimit 30 \
  -maprotation E1L1,E1L2,E1L3 -port 23513

# Graphical host. The host uses one of the eight player slots.
./rednukem -server 8 -gametype deathmatch -map arena.map

# Join a running server.
./rednukem -client 192.168.1.10 -port 23513

# Password-protected host and matching client.
./rednukem -dedicated -server 8 -password example-password
./rednukem -client 192.168.1.10 -password example-password
```

On Windows, use `rednukem.exe`. Select your installed game and addon with the
usual Rednukem resource options, such as `-j`, `-g`, `-game_dir`, `-x`, and `-h`.
`-v1 -l1` selects episode 1, level 1; `-map arena.map` selects a custom map.
`-connect` is an alias for `-client`; an address can include its port.
The default UDP port is **23513**, and server capacity is **1–16** players,
defaulting to 16. `-dedicated` enables hosting without requiring `-server`.

The main menu's **Join Game** screen accepts a server address, port, and optional
masked password. Protected connections, including Discord invitations and URI
links, open a password prompt when needed. Submit the password to retry, or press
Escape to cancel. The console equivalent is `password "example password"`;
`password ""` clears it. Passwords support up to 128 bytes and travel over the
game connection without encryption; they are never embedded in join links.

A dedicated server loads the required game assets but creates no game window
or audio device. It reserves no player slot and keeps running when everyone
disconnects. A joining player verifies the content, loads the map, and receives
a complete snapshot before entering play. The server continues while they load.

Forward the server's UDP port when required by your network. STUN discovers a
public endpoint; it does not relay traffic or bypass restrictive NAT/CGNAT.

## Match rules and map rotation

Game types are `coop`, `deathmatch` (also `dukematch`), and `teams`. The server
controls these settings; clients cannot override them. Boolean arguments accept
`on`/`off`, `yes`/`no`, or `1`/`0`.

| Argument | Values and default |
| --- | --- |
| `-gametype` | `coop` (default), `deathmatch`, `teams`. |
| `-friendlyfire` | `on`/`off`; default `off`. Applies to co-op and teammates. |
| `-scorelimit`, `-fraglimit` | 0–65535; default 0, meaning no score limit. |
| `-timelimit` | 0–10080 minutes; default 0, meaning no time limit. |
| `-difficulty` | 0–4; default 2. |
| `-autoaim` | `on`/`off`; default `on`. |
| `-weaponsstay` | `norespawn`, `permanent` (default), `respawn`, `markers`. |
| `-itemsstay` | `norespawn` (default), `respawn`, `markers`. |
| `-monsters` | `none`, `spawn`, `respawn`; defaults to `spawn` in co-op and `none` otherwise. |
| `-respawninventory` | `on`/`off`; default `off`. |
| `-losekeys` | `on`/`off`; default `on`; `-keepkeys` sets the inverse. |
| `-holsterweapon`, `-chaseview` | `on`/`off`; default `off` for dedicated deathmatch. Co-op, teams, and graphical hosts permit both. |
| `-lives` | 0–255; default 4. Positive limits apply to dedicated co-op; 0 permits unlimited respawning. |
| `-maprotation` | Comma-separated maps, for example `E1L1,E1L2,arena.map`. |
| `-maplist` | A local text file containing one map per line; `#` and `;` start comments. |

Map lists support up to 256 entries; list files are limited to 128 KiB. A
rotation wraps after its last entry. Without a rotation, co-op advances through
the episode and competitive modes reload the current map. Reaching a score or
time limit opens a **15-second intermission** with the final scoreboard and a
countdown. Gameplay stops during the intermission while networking continues.

Use `server_rule NAME VALUE` in the server console to change an exposed rule,
for example `server_rule friendlyfire on` or `server_rule timelimit 20`.
`maplist` displays the rotation, `nextmap` advances it, and `restartmap`
restarts the current map with starting equipment. `changelevel 1 2` selects
episode 1, level 2; `map arena.map` loads a custom map.

With a positive dedicated co-op life limit, lethal damage leaves a player
downed. A living teammate can approach and press **Use** to revive them with
up to 50 health, preserving equipment and the current life. Revival requires
close range and line of sight. A downed player can instead hold **Use for three
seconds** to spend a life; releasing the key cancels the hold. Release and press
Use again to respawn when lives remain. Eliminated players spectate surviving
teammates using **See Co-op View**. When everyone is eliminated, the intermission
leads to a fresh restart of the current map.

Life budgets reset on a new level. Changing `server_rule lives` during play also
resets everyone's budget and returns downed or eliminated players to play.
Reconnecting with the same address and name
during a level retains the remaining lives and downed/eliminated state. Normal
co-op progression carries health and equipment into the next level and clears
the old level's keys; restarts reset equipment. Graphical hosts retain unlimited
co-op respawning. Supported episode and RRRA cinematics are coordinated across
players; a dedicated server never renders them.

## Chat, voting, scoreboard, and lagometer

Open **Send Message** to chat. In team games, Tab switches between public and
team chat while keeping the draft. The console equivalents are `say MESSAGE`
and `say_team MESSAGE`. The server chooses team-message recipients from the
current team assignments.

Players can propose `callvote map MAP`, `callvote next`, `callvote restart`,
or, in team games, `callvote shuffle`. Answer with `vote yes` / `vote no`, or
the displayed F1/F2 controls when those keys are unbound. Votes last 30 seconds;
the caller votes yes automatically. At least 51% of the original eligible
players must approve. Late joiners cannot vote in an existing ballot, and
departures do not lower its threshold. Accepted vote calls have a two-minute
per-player cooldown. Votes cannot interrupt the match intermission.

`sv_autoteambalance 1` enables automatic balancing in team games so team sizes
differ by at most one; it is enabled by default. A passed shuffle vote assigns
balanced random teams and restarts the current map.

Hold the **Show DukeMatch Scores** control or use `scores` to toggle the
scoreboard. It shows server-supplied names, teams, scores, lives, ping,
connection time, and country flags. Country lookup uses the bundled offline
database; private and unknown addresses have no country flag. Empty names are
assigned an available **Duke00–Duke99** name.

`cl_lagometer 1` (alias `net_lagometer 1`) enables the network graph. It displays
snapshot timing, round-trip latency, missing updates, and connection trouble.
`net_socialstatus` prints scoreboard and voting information to the console.

## Maps and addons

Host an addon using its normal resource selections. The server advertises the
loaded base archives, addon files, load order, compiled CON identity, and current
map. Clients must supply their own matching base game data; base archives are
not downloaded. Missing or different required files are reported with their
sizes and content checksums.

Missing or different custom maps download automatically over the game
connection. They must pass content and map-structure checks before loading.
Binary Build map versions 7–9 are supported, up to 16 MiB, including validated
TROR links. Text-format version 10 maps are not supported by this transfer path.
Addon resources are also downloaded when necessary. Verified downloads live in
`.rednukem-addon-cache/` under the game's working directory, separated by content
identity. Installed files are not replaced, and archives stay packed.

Changing the active addon requires an **automatic restart and reconnect**.
The restart retains the server address, accepted password, and local startup
settings. An already active matching addon does not need a restart. The cache
is checked before reuse; transfer, verification, or activation failures stop
joining with a diagnostic. Addons requiring another executable or external
installation steps still require manual installation. Keep cached maps and the
matching addon available for packet-demo playback.

Gameplay sounds and taunts are relayed by the server. Dedicated servers keep
sound timing without opening an audio device; active loops are included when
a player joins or starts a packet recording.

## STUN, Discord, and connection links

`net_stunserver` defaults to `auto`. Use `net_stunserver off` to disable public
endpoint discovery, or set a STUN `host[:port]`. The command-line equivalent is
`-stunserver VALUE`. `net_stunstatus` displays discovery status and the available
`rednukem://` join link.

Discord integration requires the local Discord desktop app and a numeric
Discord Application ID; none is supplied by default. Configure it in the
graphical game's console:

```text
discord_appid YOUR_NUMERIC_APPLICATION_ID
discord_enabled 1
discord_status
```

The ID is the application's public identifier, not a token or password.
`-discord-appid ID` is the startup equivalent. Presence advertises the map,
mode, session time, and party size, and supplies Discord Join/invitation data.
It registers a per-user Discord launcher; `discord_register` retries that
registration. Dedicated servers do not connect to the Discord desktop app.
Use `discord_server_address rednukem://public-host:23513` (or
`-discord-server-address URI`) to configure an explicit advertised address.
`discord_enabled 0` disables the integration.

Register ordinary connection links once for your current OS user:

```sh
./rednukem -register-uri
./rednukem -connect-uri rednukem://server.example:23513
```

Registration is implemented for Windows and Linux; Linux requires `xdg-utils`.
It exits without loading the game. Run it again after moving the executable.
A bare URI, such as `./rednukem rednukem://server.example:23513`, also works.
Links accept hostnames, IPv4, and bracketed IPv6 addresses; the omitted port
defaults to 23513. A link contains only the endpoint: passwords, map paths,
queries, and fragments are not supported. Link launches accept no additional
arguments; use ordinary `-client` launching when extra startup options are
needed. Configure the local game installation before using external links.

## Demos and compatibility

Record a live session from a client or host with:

```text
net_record match.rnpd
net_stoprecord
net_disconnect
net_playback match.rnpd
```

A dedicated server needs an active player's viewpoint when recording begins.
Packet demos store accepted authoritative snapshots and presentation messages,
including changing rosters and map transitions. Recording can start during a
match. Playback uses the snapshot path, without rerunning gameplay or requiring
a live server. It requires a compatible protocol build and the matching game,
addon, and maps; the recording does not embed their files.

Legacy `.dmo` playback and the existing `.edm` input-demo recorder/player retain
their offline paths.
Original lockstep demos are not converted to packet demos, and their fixed
roster format cannot represent hot joins. Existing lockstep multiplayer clients
cannot connect to the authoritative server. A `NETCODE=0` build retains offline
play and legacy demos but cannot host or join this multiplayer protocol.

## Diagnostics and transport settings

Use the graphical game's console or dedicated-server standard input. On
Windows, the dedicated server accepts commands from a terminal or a
standard-input pipe.

| Command or setting | Purpose |
| --- | --- |
| `net_status` | Session role, map generation, server tick, players, rules, life states, and snapshot diagnostics. |
| `net_disconnect` | Leave the session or packet-demo playback. |
| `net_audiostatus` | Logical sound instances, loops, and received/playback counts. |
| `net_packetcopies 3` | Total copies per UDP transmission; range 1–8, default 3. More copies consume more bandwidth. |
| `net_snapshotrate 20` | Server snapshots per second; range 1–30, default 20. Simulation remains at 30 Hz. |
| `quit` | Exit the game or dedicated server. |

The wire format encodes numeric fields explicitly in little-endian order.
Full/delta snapshots use the shared LZ4 codec, with acknowledged baselines and
full-state recovery. Snapshot application validates world references and
rebuilds sprite lists before presenting the state. Native pointers, savegame
loading, and server credentials are not part of world snapshots.

The implementation is in `source/rr/src/rnet*.cpp`. Tests under
`tests/rednukem-network/` cover the snapshot codec, content, gameplay rules,
packet demos, and real game processes; Rednukem social and URI tests also live
under `tests/network/`.
