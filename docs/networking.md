# Authoritative multiplayer

NBlood uses a UDP client/server protocol. The server runs the world at 30 ticks
per second and owns combat, damage, enemies, pickups and map transitions. Clients
send input commands, predict local movement and immediate weapon presentation,
then reconcile with acknowledged server snapshots. Remote players and moving
geometry interpolate between snapshots. Prediction never applies damage or
runs weapon callbacks on a client.

All participants need this protocol and matching game content. Missing or
different maps are downloaded automatically. Older lockstep multiplayer clients
cannot connect. Legacy demo playback remains available.

## Hosting and joining

The same executable supports a graphical listen server and a dedicated server.
Use the Multiplayer menu to host or join, or launch from a terminal:

```sh
# Dedicated co-op server, up to eight players, default map E1M1.
./nblood -dedicated -server 8 -gametype coop -port 23513

# Co-op with six lives per player for each level (default: four).
./nblood -dedicated -gametype coop -lives 6

# Dedicated team server with teammate damage enabled.
./nblood -dedicated -gametype teams -friendlyfire on -port 23513

# Dedicated Bloodbath server allowing holstering and chase view.
./nblood -dedicated -gametype bloodbath -holsterweapon on -chaseview on

# Join it, including after the level has already started.
./nblood -client 192.168.1.10 -port 23513

# Graphical host; the host occupies one of the eight player slots.
./nblood -server 8 -gametype bloodbath -port 23513
```

On Windows, substitute `nblood.exe` for `./nblood`. Supply `-j=/path/to/Blood`
when the game assets are elsewhere. Game types are `coop`, `bloodbath`, and
`teams`. `-map E1M2` selects an initial map. The dedicated server needs game
assets but creates no window or audio device, reserves no player slot, and
continues running when everyone disconnects. Players may join and rejoin while
the server runs. A joining client loads the map and receives a complete snapshot
before entering play; the server continues while that client loads.

Dedicated servers accept `-friendlyfire on|off`, defaulting to `off`. It controls
damage between teammates in team games and between players in co-op; self-damage
is unaffected. The setting persists across map changes and cannot be changed by
connected clients. `net_status` reports the effective value as `friendly_fire`.

When a dedicated server's score or time limit is reached, the match ends with a
**15-second pause** before the next map loads. All clients automatically see the
final scoreboard and a countdown at the bottom of the screen. Movement, combat,
AI and level timers stop during this pause; network traffic and joining remain
available. The results include the winning frag and stay fixed if players join
or leave while the countdown runs. A player joining during the pause sees the
same results and the time remaining.

At the deadline, the server advances once to the next map in its rotation,
wrapping after the last entry. Without a rotation, Bloodbath and team games
reload the current map; co-op retains its episode progression. The next match
starts with a fresh scoreboard. Active player votes are canceled, and new votes
cannot interrupt the pause. The operator can still change maps explicitly.
Vote- and operator-triggered map changes outside this pause remain immediate;
the pause applies to automatic score/time limit endings. Packet demos preserve
the final results and the countdown; legacy input demos remain unchanged.
`net_status` reports `NET intermission`
with its active state, remaining milliseconds and frozen level time.

Open chat with the **Send Message** control. Normal chat shows **`say:`**.
In team games and co-op, a **Tab key icon** appears before the prompt: press
Tab to switch to **`team say:`**, or press it again to return to normal chat.
Switching keeps your draft; Enter sends it and Escape cancels it. Each new
chat starts in normal mode. Bloodbath offers normal chat only.

Team messages appear as **`[TEAM] Name: message`** and reach only players on
the sender's team, including the sender. In co-op, all players are teammates.
The server selects recipients from its current team assignments. Packet demos
retain only the chat delivered to their recorded player; opposing-team chat
is not included.

Players can propose **`callvote restart`** in co-op, Bloodbath and team games.
A passed vote restarts the **current map with fresh scores and starting
equipment**, preserving the game mode, server settings, team assignments and
map rotation position. Restart voting works on dedicated and listen servers,
with or without a map rotation.

In team games, players can also propose **`callvote shuffle`** in the console.
Players answer with **`vote yes`** or **`vote no`**; **`vote_status`** shows the
current ballot and **`vote_maps`** lists available vote commands and maps.
Team shuffling is available without a map rotation. Bloodbath and co-op reject
shuffle requests, and `-mapvote off` disables map, restart and shuffle votes.

If the shuffle passes, the server randomly divides all currently connected
players into two teams whose sizes differ by at most one, then restarts the
**current map with fresh scores and starting equipment**. Players respawn with
their new team's color and spawn points. The map rotation position and all
server settings stay the same; the new teams persist into subsequent maps.
Connection ages and vote cooldowns are retained. Packet demos record the vote,
restart and resulting team assignments.

Map, restart and shuffle votes share the same rules: a vote runs for **30 seconds**, the
caller automatically votes yes, and at least **51% of the players eligible at
the start** must vote yes. Late joiners can watch but cannot vote in that ballot;
departures do not lower its threshold. Only one vote can run at once, and each
player has a shared **two-minute cooldown** between accepted vote calls.
Votes cannot interrupt the end-of-match pause. Server rules, including
the game type, remain under the server operator's control.

In dedicated Bloodbath games, holstering weapons and chase view are disabled by
default. Enable either with `-holsterweapon on` or `-chaseview on`; both options
accept `on|off`. These restrictions do not affect co-op, team games, graphical
hosts, single player, or legacy demo playback. The settings are sent to joining
clients and retained across map changes, and clients cannot override them.
`net_status` reports the effective permissions as `holster_weapon` and
`chase_view`. These options restrict the player controls; normal weapon switching
and weapon lowering caused by gameplay still work.

Dedicated co-op servers accept `-lives N`, from 0 to 255, defaulting to **4 lives
per player per level**. Four lives include the initial life and three respawns.
Use **`-lives 0`** to retain legacy unlimited respawning, without kneeling,
teammate revival, life-based spectating or automatic team-wipe restarts.
The option requires co-op; other game modes and graphical hosts keep their
existing respawn behavior.

With a positive life limit, lethal damage puts a player into a kneeling state.
A living teammate can approach and press **Use** to revive them with **50 health**,
preserving their current life and inventory. Revival requires line of sight and
close range. Kneeling players cannot fight or move and remain safe from further
damage while waiting. To give up instead, hold **Use for three seconds**;
releasing it cancels the hold. The
screen shows the bound key and hold progress. Only giving up deducts a life;
press Use again after the death animation to respawn if any lives remain.

Players with no lives left can only spectate surviving teammates. The **See
Co-op View** control cycles between them; the HUD shows the target and bound key.
Remaining lives appear only on the co-op scoreboard for every player, with `DOWN`
or `OUT` labels for incapacitated or eliminated players. Once every connected
player is out of lives, the server fully restarts the **current map**: all player
keys, weapons, ammo, armor, inventory and powerups reset, monsters and pickups
return, and everyone receives a fresh life
budget. The usual starting pitchfork is provided. An empty server does not loop
through restarts. New levels also replenish lives; a successful co-op level exit
retains the normal inventory-carryover rules.

Late joiners receive their own full budget. Reconnecting with the same address
and player name during a level retains remaining lives and kneeling/eliminated
status. Lives and revival state are authoritative, reported by `net_status`, and
included in packet demo recording/playback. Legacy input demos remain unchanged.

The port uses UDP. Clients connect directly to the server address. The server
checks protocol, content and map identity.

### Join links on Windows and Linux

Register this installation once for your current OS user:

```sh
./nblood -register-uri        # Linux (requires xdg-utils)
```

```bat
nblood.exe -register-uri
```

Registration exits without loading game assets and needs no administrator/root
access. Windows registers `netblood` under `HKEY_CURRENT_USER\Software\Classes`.
Linux installs `netblood.desktop` in `$XDG_DATA_HOME/applications` (normally
`~/.local/share/applications`) and uses `xdg-mime` to associate
`x-scheme-handler/netblood`. Run the command again after moving the executable,
or to switch which installation opens links.

Websites and chat messages can now link to `netblood://server.example:23513`.
The browser may ask whether to open NBlood. Hostnames, IPv4, and bracketed IPv6
are supported, for example `netblood://192.0.2.10:23513` and
`netblood://[2001:db8::10]:23513`. Omit the port to use **23513**. A single
trailing slash is accepted. Credentials, query strings, fragments and map paths
are not supported; the server selects the map and game rules.

You can also open a link directly with
`./nblood 'netblood://server.example:23513'` or
`./nblood -connect-uri 'netblood://server.example:23513'` (`nblood.exe` on
Windows). Link launches accept exactly one URI and **no additional arguments**;
use ordinary `-client`/`-port` launching when you need other command-line options.
Malformed links stop with an error before game options or files are processed.

A link starts a new game process and joins automatically, skipping intros and
the usual startup dialog once a configuration exists. The initial working
directory is the executable's directory, so a browser's directory cannot
change which portable game files are found. Normal user-profile settings and
Steam/GOG asset discovery still apply. Set up the installation and its game
assets normally before opening links. The existing Windows prompt for an
already-running copy still applies; links do not replace an active session.

Discord can also display the current session and let friends join or invite
players through its desktop client. See [Discord Rich Presence](discord.md) for
Application ID configuration and launch registration.

Hosted games can use [public STUN discovery](stun.md) to find their mapped UDP
endpoint for connection links and Discord invitations. Google's public servers
are the default; `-stunserver off` disables discovery. STUN does not guarantee
reachability through restrictive NAT/CGNAT.

### Client version compatibility

On connection, the server advertises its required network protocol version and
build label before admitting a player or transferring game content. The current
required protocol is **7**. Build labels identify the executable for diagnostics;
compatibility is determined by the network protocol, not by Git revision age.

An older client receives an error showing its version and the server requirement,
with instructions to update the game and reconnect. A newer client connecting to
an older server is told to ask the server administrator to update or use a
matching game version. Updated clients keep this message on the connection
screen; no map or gameplay state is loaded after an incompatible advertisement.

The version announcement uses a stable connection format separate from gameplay
packets. A diagnostic rejection also uses the connecting client's advertised
protocol so earlier clients can read the explanation through their existing
error handling. Gameplay packets and packet demos still require an exact
protocol match; legacy input demos are unchanged.

### Automatic map downloads

When a joining client's installed map is missing or has a different checksum,
the client first checks its download cache, then requests the server's current
map if necessary. This also works when the server changes maps. A progress
indicator appears while downloading; the client enters play only after the
complete map passes checksum and format validation. Other players and the
server continue running during the transfer.

Downloads use the existing reliable UDP connection in bounded windows of four
16 KiB chunks. Maps are limited to 16 MiB. The server serves only its current
map, and clients cannot request arbitrary server files. Interrupted transfers
are discarded; a later connection starts a fresh transfer.

Validated maps are stored in `.nblood-map-cache/` under the game's working
configuration directory (`-usecwd` uses the current directory). Generated cache
filenames use the map identity, never the server's path. Installed maps are not
overwritten. The cache is checked again before reuse; corrupted files trigger a
fresh download. If downloading or saving fails, joining stops with an error in
the console. The cache can be removed while the game is closed to reclaim space.

Only the `.MAP` file is transferred. Base game archives, artwork, sounds, and
other mod assets must already match. Optional companion files such as map
metadata (`.DEF`), music and map hacks are not downloaded. Maps that require
these resources should be distributed with their mod as usual.

## Missing or different mod assets

Before downloading a map or entering the game, clients check the server's
required asset manifest. A failed check shows each affected filename, its
expected size in bytes, and its expected MD5 checksum. The report distinguishes
missing files, different contents (including changes with the same file size),
and correct files that are present but were not loaded by the selected mod.
Use Page Up and Page Down to browse the error screen. The full report is also
written to the game log and `net-content-mismatch.txt` in the current game
configuration directory. For example:

```text
NET asset missing: EXAMPLE.DAT | size=0 bytes | md5=d41d8cd98f00b204e9800998ecf8427e
```

The values describe the server's required file, so they can be compared with
the mod package using `md5sum` or another MD5 utility. Install and select the
same mod on both machines, then restart; the game freezes its content inventory
at startup. Mod assets are not downloaded automatically.

The inventory covers selected RFF archives, ART files, external resource
overrides, loaded definition scripts and their tracked asset dependencies.
Maps use the separate map-transfer check. If files match but effective gameplay
definitions differ, the error advises checking extra overrides and load order.
File diagnostics require updated clients and servers. Earlier authoritative
clients retain their original fingerprint check and receive a shorter error.
MD5 identifies file contents here; it is not a signature or an authenticity check.

## Console controls

The dedicated server accepts console commands on standard input. The graphical
game uses its usual in-game console. Settings can also be placed in
`autoexec.cfg`.

| Command or setting | Meaning |
| --- | --- |
| `net_status` | Session, player, scoreboard, snapshot, prediction and UDP statistics. |
| `net_packetcopies 3` | Total identical UDP copies per transmission; range 1–8, default 3. |
| `net_snapshotrate 20` | Server snapshots per second; range 1–30, default 20. Simulation stays at 30 Hz. |
| `net_lagometer 1` | Enable the network graph; `0` hides it. |
| `net_disconnect` | Leave the current network session or packet playback. |
| `changelevel 1 2` | Server changes to episode 1, level 2. |
| `map E1M2` | Server loads the named map. |
| `restartmap` | Server restarts the current map. |
| `quit` | Exit; the dedicated server closes its session normally. |

`net_packetcopies` counts the original send: `3` means three datagrams in total,
not one original plus three extra copies. Copies have the same ENet sequence,
fragment and acknowledgement identifiers, so they do not execute input or
presentation events repeatedly. Extra sends increase bandwidth use. ENet still
provides ordered reliable control messages and retransmission, while command
and snapshot updates use separate channels. Multiple recent commands are also
included in input messages to recover ordinary packet loss.

Only the server chooses maps and match rules. Clients can change presentation
settings without changing authoritative gameplay.

## Lagometer

The graph sits at the right edge, just above the blue team flag, leaving both
team scores visible. Its position stays fixed when the game view is reduced.

The graph follows the presentation used by Quake III Arena:

- The upper graph shows snapshot timing: blue while interpolating, yellow when
  updates are late and the client has exhausted its interpolation interval.
- The lower graph shows round-trip latency in green, delayed updates in yellow,
  and missing snapshots in red.
- A connection interruption indicator appears when updates or input
  acknowledgements stop arriving.

It uses the shared HUD drawing path for software, Polymost, Polymer and Vulkan.
Reference: id Software's
[`CG_DrawLagometer`](https://github.com/id-Software/Quake-III-Arena/blob/master/code/cgame/cg_draw.c).

## Scoreboard

Hold **F11** to show the multiplayer scoreboard. F1 already opens Help, and the
other function keys have existing actions. `Show Scoreboard` is available in
the keyboard controls menu for remapping. An existing custom F11 binding is
preserved. When upgrading an older configuration, an unbound F7 can be used
instead; F7 normally selects chase view. If both keys are occupied, choose a
binding in the controls menu.

Each row shows the player's country flag, name, score, round-trip ping to the
server in milliseconds, and whole minutes connected to the server. The server
supplies this information and refreshes it once per second. Connection time
continues across map changes and resets when a player reconnects. The local
player on a listen server has zero network latency.

Empty multiplayer names receive the first available name from `Caleb00` through
`Caleb99`. The server checks names case-insensitively, including players still
joining, and reuses the lowest available number after a player leaves. This also
applies to the local player on a listen server. Nonempty names are unchanged.

In team games, players appear below separate blue and red team headers.
Each header shows the team's player count and labels the per-player score,
ping, and time columns. Players are ordered by score within their team, and
your row is highlighted. Players still joining appear in a separate neutral
section until the server assigns their team.

Country flags use an offline IPv4/IPv6 country database bundled with the game; no
external lookup service or server configuration is needed. The data describes
the IP network's registration country, which can differ from the player's
physical location, particularly with VPNs, proxies and international ISPs.
Private, loopback and unknown addresses receive a neutral flag. Only the
country code is sent to clients or included in packet demos, not the IP address.
The country data is CC0 and the flag artwork is MIT licensed; source details
and regeneration instructions are in
[`source/blood/data/netcountry/`](../source/blood/data/netcountry/README.md).

## Demos

Record the accepted server stream from a connected client or a host with:

```text
net_record match.nbpk
net_stoprecord
```

After disconnecting, play it with:

```text
net_playback match.nbpk
```

A dedicated server must have an active player when recording starts. It records
from an active player’s viewpoint, changes viewpoint if that player leaves, and
keeps the file open across periods with no players. A listen server records its
local player’s view.

Packet demos begin with the current map/session information and a complete
snapshot, so recording can start midway through a match. Later snapshots are
stored independently of network delta baselines, together with accepted
presentation events and timing. Retransmissions and duplicate datagrams are not
recorded multiple times. Playback uses the authoritative snapshot path and does
not simulate the world again. Hot joins, disconnects and map transitions are
represented by the recorded authoritative state.

Scoreboard updates are also recorded, so playback shows the original names,
scores, pings, connection times and country flags.

Downloaded maps remain in the cache for offline packet-demo playback. Map bytes
are not embedded in the demo: moving a recording to another machine also
requires its maps, or that machine must first join a server to download them.
An unavailable map stops playback with an error instead of trying to download
from an offline server.

The legacy `-record` input recorder and existing demo playback/validation remain
available for offline sessions. Legacy demos have a fixed roster in their
header and cannot describe hot joins; live sessions use packet demos. Original
Blood 1.21 validation demos still exercise the original simulation and RNG
sequence. A packet demo is a recording of observed authoritative state, so it
serves a different purpose from deterministic input-demo validation.

## Protocol and verification

Wire fields use explicit little-endian encodings and bounded lengths, rather
than native C++ structure layouts or pointers. Snapshots encode client-visible
world, collision, player and HUD state, with versioned LZ4 full/delta envelopes.
A delta identifies the exact acknowledged baseline; a missing baseline causes
full-state recovery. Map generations separate sessions across level changes,
and sprite generations prevent interpolation or sounds attaching to a reused
sprite slot. Server AI pointers and event queues remain server-private.

The current wire protocol is version 7; clients and servers must use the same
version. Scoreboard metadata is independent of the deterministic game state.

Relevant code is in `source/blood/src/network.cpp`, `netprotocol.h`,
`netsnapshot*.cpp`, `netpredict.cpp`, `netevents.cpp`, `netdemo.cpp` and
`netlag.cpp`. Scoreboard metadata and country lookup use `netscoreboard.h` and
`netcountry.cpp`. Asset checks and reports use `netcontent*.cpp` and `netassets.h`.
The GNU build and Windows project include these files.

Protocol and real-socket tests are under `tests/network/`. The integration
runner starts real game processes and exercises delayed joins, movement,
impairment, disconnect/rejoin, map changes and packet recording/playback.
Legacy fidelity is checked separately with `tests/demo-validation/run.py`.
