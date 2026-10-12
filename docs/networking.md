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

The port uses UDP. Clients connect directly to the server address. The server
checks protocol, content and map identity.

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

The current wire protocol is version 5; clients and servers must use the same
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
