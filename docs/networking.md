# Authoritative multiplayer

NBlood uses a UDP client/server protocol. The server runs the world at 30 ticks
per second and owns combat, damage, enemies, pickups and map transitions. Clients
send input commands, predict local movement and immediate weapon presentation,
then reconcile with acknowledged server snapshots. Remote players and moving
geometry interpolate between snapshots. Prediction never applies damage or
runs weapon callbacks on a client.

All participants need this protocol and matching game content. Older lockstep
multiplayer clients cannot connect. Legacy demo playback remains available.

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
checks protocol, content and map identity; it does not distribute missing game
assets or maps.

## Console controls

The dedicated server accepts console commands on standard input. The graphical
game uses its usual in-game console. Settings can also be placed in
`autoexec.cfg`.

| Command or setting | Meaning |
| --- | --- |
| `net_status` | Session, player, snapshot, prediction and UDP statistics. |
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

Relevant code is in `source/blood/src/network.cpp`, `netprotocol.h`,
`netsnapshot*.cpp`, `netpredict.cpp`, `netevents.cpp`, `netdemo.cpp` and
`netlag.cpp`. The GNU build and Windows project include these files.

Protocol and real-socket tests are under `tests/network/`. The integration
runner starts real game processes and exercises delayed joins, movement,
impairment, disconnect/rejoin, map changes and packet recording/playback.
Legacy fidelity is checked separately with `tests/demo-validation/run.py`.
