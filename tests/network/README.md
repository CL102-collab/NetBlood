# Authoritative multiplayer regression checks

These tests exercise the production network code. Keep the original
[144 DOS Blood 1.21 regression demos](../demo-validation/README.md) as a separate
gate: snapshot multiplayer clients do not simulate the authoritative world, so
their packet demos cannot replace legacy input-demo determinism checks.

## Standalone codecs and real UDP

```sh
tests/network/protocol-test.sh
SANITIZE=1 tests/network/protocol-test.sh
tests/network/prediction-test.sh
SANITIZE=1 tests/network/prediction-test.sh
tests/network/events-test.sh
SANITIZE=1 tests/network/events-test.sh
SANITIZE=1 tests/network/snapshot-state-test.sh
tests/network/map-storage-test.sh
SANITIZE=1 tests/network/map-storage-test.sh
tests/network/assets-test.sh
SANITIZE=1 tests/network/assets-test.sh
SANITIZE=1 tests/network/server-settings-test.sh
tests/network/teams-test.sh
SANITIZE=1 tests/network/teams-test.sh
tests/network/player-team-test.sh
SANITIZE=1 tests/network/player-team-test.sh
```

The first script needs C/C++ compilers and uses the repository's LZ4 and ENet
sources. It needs no game data, graphics driver, or display. `SANITIZE=1` enables
AddressSanitizer and UndefinedBehaviorSanitizer. The prediction script also
needs SDL2 development headers and `pkg-config`; its movement primitive is a
test stub, while command history, reconciliation, feedback suppression, and
render interpolation run the real prediction module.

The checks include:

- Explicit little-endian wire bytes, exact lengths, invalid fields, truncated
  commands, sequence wrap, stale/duplicate input suppression, a bounded receive
  window, and recovery after a long input outage.
- Full and XOR-delta snapshots, lost/wrong baseline rejection and recovery,
  state growth/shrinkage, checksums, truncation, 16 MiB limits, unchanged output
  on rejected data, and 30,000 deterministic malformed inputs.
- The production canonical-world field parser and graph validator, using a
  synthetic valid world: every truncated prefix, deep mutations, object IDs,
  sprite lists, portal connectivity, and HUD bounds. These checks are separate
  from real-session exact snapshot installation round trips.
- Real loopback UDP sessions passing through a datagram relay. One and three
  copies produce the expected identical datagrams on the wire; reliable,
  unreliable, and fragmented payloads each arrive exactly once. A run discards
  the first two of every three datagrams in both directions. Configured copy
  counts outside the supported range are clamped.
- Packet-demo byte-exact round trips, 30 Hz pacing, multiple packets at one
  tick, tick wrap, map clock resets, exclusive record/playback state, every-byte
  truncation and corruption, trailing data, and invalid metadata/packet limits.
- Lagometer timing/RTT/loss history, interpolation/extrapolation signs, delayed
  snapshot flags, ring wrap, bounded hostile gaps, disconnect state, and reset.
- Scoreboard row limits, unique ordered player slots, names and country codes,
  signed scores, latency bounds, truncated packets, atomic rejection and
  connection-minute boundaries using a clock independent of map resets.
- Presentation event bounds and dispatch, private recipient filtering, predicted
  sound suppression, reused sprite generations, deferred loop creation after its
  corresponding snapshot, loop stop/reset, and hot-join audio baselines. Mixer
  calls are test stubs; the event parser and delivery code are the real module.
- Production BLM 6/7 map validation and cache storage: bounded file structures,
  valid legacy portal quirks, corruption, safe cache paths, symlink rejection,
  and atomic writes that preserve installed maps. The standalone storage test
  needs no game data or engine globals; optionally append paths to real `.MAP`
  files to validate those too.
- Asset-manifest size and entry bounds, exact field lengths, unsafe diagnostic
  text, duplicate identities, every truncated prefix and malformed mutations.
  MD5 is checked against standard RFC vectors and independent Python hashlib
  results, including boundary lengths and streaming reads. Comparison checks
  cover missing and different files, renamed matching archives, and extra local
  assets.
- Server-rule wire bytes, bounded fields, every truncated prefix and unchanged
  output on rejection. Dedicated limit checks run the production handler with
  controlled active-player and team counters, covering unlimited limits,
  precise minute boundaries, empty servers, inactive slots, transition guards,
  hot-join team-score retention, and cooperative normal/secret/end-of-episode routing. The limit script needs
  SDL2 development headers and `pkg-config`, but no game data or display.

## Dedicated server and graphical clients

Build the game first, then run under an existing X11 display (Xvfb is suitable):

```sh
python3 tests/network/run.py \
  --binary ./nblood \
  --assets /path/to/blood-1.21 \
  --output /tmp/netblood-network-evidence
```

The output directory must not exist. The harness compiles a small SDL input
interposer, starts the ordinary executable in isolated directories, and sends
movement/fire key events and normal console commands. It does not inject game
state, shortcut handshake messages, or replace the game's transport.

The dedicated process starts without `DISPLAY`, `WAYLAND_DISPLAY`, or
`XAUTHORITY`, and with deliberately invalid video/audio drivers. Device-creation
interposers fail the test if it opens a window or an SDL audio device. Its clock
must advance before anybody connects and after everybody leaves.

The session covers a first client, a delayed second client, gameplay under
55 ms one-way delay, 15 ms jitter, 8% physical datagram loss, and additional
reordering; server-controlled map changes; all-client disconnect; and a fresh
join into the still-running empty server. Ten seconds of command-bearing
datagram suppression also exercises bounded prediction history and input
resynchronization while other connection traffic continues. It checks authoritative snapshot
hashes against server logs, local input movement and prediction reconciliation,
records an actual client's packet demo, and compares the final authoritative
playback snapshot with that recording. A dedicated-server packet recording also
follows viewpoint changes when its viewed player leaves, pauses during an empty
server, and resumes on the next join; its playback must reproduce the final
snapshot too. A separate graphical listen server accepts a remote client,
changes maps, and accepts a fresh join. Its first client then replays a two-map
recording in the same process, exercising cleanup of previous map registrations.
Screenshots and all process logs remain
in the evidence directory. `result.json` includes the binary hash, commands,
snapshot comparisons, packet-demo inventory, prediction statistics, and actual
relay loss/reordering counters. Any missing gate, timeout, runtime error, or
unclean process exit fails the run.

`--skip-map-change` is available for isolating a failure; a run using it does not
establish map-transition coverage. `--timeout` adjusts startup/join deadlines.
`--renderer` selects software (0, default), Polymost (3), Polymer (4), or Vulkan
(5) for graphical processes; that renderer must be enabled in the tested build.
The input harness uses Linux `LD_PRELOAD` and a FIFO; it is not a Windows runtime
test. Windows compilation and execution must be reported separately.

## Dedicated startup rules

For example, run a two-map Bloodbath rotation with a 15-minute or 30-frag limit:

```sh
./nblood -dedicated -port 23513 -gametype bloodbath -maxplayers 8 \
  -timelimit 15 -scorelimit 30 -autoaim off -weaponsstay on \
  -maplist rotation.txt
```

`rotation.txt` contains one map name per line. Blank lines and `#` or `;`
comments are allowed, including comments after a map name. Only the text
filename is passed to `-maplist`; put it on the game's search path. The maps
must be available to the server. A missing client map uses the existing
automatic download path. The rotation repeats after its last entry.

```text
# Stock-map rotation
E1M1
E1M2
```

For cooperative episode play:

```sh
./nblood -dedicated -port 23513 -gametype coop -maxplayers 4 \
  -episode 2 -difficulty 3 -losekeys off -autoaim on -weaponsstay on
```

Episode numbers start at 1 and must name a cooperative episode in the selected
INI. Normal and secret exits follow that episode's definitions; finishing the
episode starts it again. `-map`, `-maplist`, and `-episode` are mutually
exclusive.

| Parameter | Values and defaults |
| --- | --- |
| `-port` | UDP port 1–65535; default 23513 |
| `-gametype` | `coop` (default), `bloodbath`, `teams` |
| `-maxplayers` | 1–8; default 8; `-server N` remains an alias for the cap |
| `-timelimit` | Integer minutes 0–10080; 0 (default) means unlimited |
| `-scorelimit` | 0–2147483647; 0 (default) means unlimited; Bloodbath or teams only |
| `-autoaim` | `on` (default) or `off`, applied to every player |
| `-weaponsstay` | `on` (default) or `off` |
| `-friendlyfire` | `on` or `off` (default); teammate damage in co-op and teams |
| `-holsterweapon` | `on` or `off` (default); permission to holster in Bloodbath |
| `-chaseview` | `on` or `off` (default); permission to use chase view in Bloodbath |
| `-mapvote` | `on` (default) or `off`; enables player map votes |
| `-maplist` | A `.txt` filename, up to 256 maps and 128 KiB |
| `-episode` | Cooperative episode, numbered from 1 |
| `-difficulty` | Cooperative difficulty 0–4; default 2; `-s` remains supported |
| `-losekeys` | `on` (default) or `off`; cooperative play only |

When a configured time or score limit is reached, the dedicated server advances
the map list. With an explicit cooperative episode, a time limit follows the
normal exit; without a map list or explicit episode, limits restart the current
map. Bloodbath uses individual frag totals; teams uses team totals. The
clock also runs while the server is empty. `net_status` prints effective rules
and each player's effective autoaim/skill. Connected clients cannot change
maps or restart games directly, start games, host another server, or change server-owned
autoaim through their console or menus. Disconnecting explicitly returns to
local play.

The mandatory rule metadata, map-vote, cinematic and scoreboard messages use wire protocol 6. Rebuild the server and all
connected clients together; older network builds are incompatible. Legacy DOS
input-demo recording and playback retain their original formats.

Exercise CLI validation and live enforcement under the same X11/asset setup:

```sh
python3 tests/network/server-settings.py \
  --binary ./nblood \
  --assets /path/to/blood-1.21 \
  --output /tmp/netblood-server-settings-evidence
```

This test rejects malformed/missing values, conflicting launch selections,
invalid rotation files and unavailable maps before the server listens. A
configured Bloodbath server admits two graphical clients, including a late
join, and rejects a third. It checks effective rules on both sides, client
console map/restart/autoaim denial, and a real one-minute map rotation. A
separate cooperative server checks episode, difficulty and retained keys.
Friendly fire is checked with its default disabled value during rotation,
explicitly enabled in co-op and explicitly disabled in teams. Connected clients
must receive each setting and retain it through a server restart or map change;
invalid values and use without `-dedicated` must fail during startup.
Bloodbath disables weapon holstering and chase view by default. The test presses
actual bound keys and checks the equipped weapon on the server and client, plus
the client's camera state. Separate sessions enable each permission independently
and retain it across restarts; co-op and teams retain both controls even when the
Bloodbath-only switches are explicitly off. Invalid/missing switch values and
use without `-dedicated` are also rejected.
Real ENet probes request forbidden autoaim and difficulty values in their
profiles; the authoritative player profiles must keep the configured values.
The output retains exact command lines, process logs, statuses and a binary
hash in `result.json`. It tests console authority enforcement; the separate
menu guards are not exercised by synthetic menu navigation.

## Automatic team balancing

In teams mode, an imbalance of at least two players selects the most recently
joined player on the larger team for balancing. The server waits until that
player dies, then transfers them before their next respawn. It does not move a
living player or substitute an older dead teammate. Revivable kneeling and
revival do not count as a final death. The server reevaluates
the roster each tick: joins or departures that remove the imbalance cancel
the pending move, and changes to the newest eligible player update the choice.
This applies to dedicated and listen servers; Bloodbath and co-op are unchanged.

Join age follows the player's current connection, rather than their player-slot
number. Reconnecting starts a new age; advancing or restarting a map preserves
the connected players' ages and assigned teams. Normal death handling, including
frag/team scoring and carried-flag drops, happens on the original team. The
transfer does not reset scores or respawn the player; their ordinary respawn
uses the new team's color and team spawn points where the map defines them.
The server announces the transfer to connected players; authoritative snapshots
and packet demos retain the resulting team state.

Exercise normal UDP joins, departures, a map change and offline packet playback:

```sh
python3 tests/network/team-balance.py \
  --binary ./nblood \
  --assets /path/to/blood-1.21 \
  --output /tmp/netblood-team-balance-evidence
```

This check needs the same X11 display and assets as the other graphical network
tests. It generates two small valid maps with safe starting ledges facing a
lethal drop, and kills players using ordinary forward movement. A 2:0 imbalance
must wait for the newest player's death, even when their older teammate dies
first. Joining or leaving to restore a one-player difference must cancel the
pending move, including when the former candidate subsequently dies. A map
change must retain team membership and session join age while leaving living
players in place. A reused lower-numbered slot must receive a new join age and
be selected over an older higher-numbered slot. Every client must observe the
dead player's transferred team/color before normal Open-key respawn; death
scoring must affect the original team without resetting scores. Canonical
snapshot hashes, exact snapshot-installation round trips and a two-map packet
recording's final playback snapshot must agree. Separate Bloodbath and co-op
sessions verify that departures do not trigger balancing.
The evidence directory retains logs, launch commands, the recording, its
inventory, and `result.json` with the tested binary's hash.

The standalone `teams-test.sh` checks the production roster-selection rules,
including every roster/team arrangement up to eight players and join-order
ties. `player-team-test.sh` exercises the production player-transfer helper,
including live, dead, kneeling and reviving players, equipment/score preservation
and carried-flag handling. These need no running game or game assets; add
`SANITIZE=1` for sanitizer coverage.

## Player map votes

Dedicated servers enable map voting by default. Add `-mapvote off` to disable
it, or specify `-mapvote on` explicitly. Votes can change only the map; the
server's game mode, time/score limits and other rules remain authoritative.

Players use these console commands:

| Command | Action |
| --- | --- |
| `vote_maps` | List maps available for a Bloodbath or teams vote |
| `callvote map E1M2` | Propose a map from the server's configured rotation |
| `callvote restart` | Propose restarting the current co-op map |
| `callvote next` | Propose the next co-op map |
| `vote yes` / `vote no` | Cast one ballot in the active vote |
| `vote_status` | Show the current vote, counts and time remaining |

Bloodbath and teams votes require a map rotation; a server started without
`-maplist` cannot accept votes to select a map. Names match rotation entries
case-insensitively, with an optional `.MAP` suffix. Co-op accepts only restart
or next, following the normal episode exit (or next rotation entry). A custom
standalone map has no next level, but can be restarted. Listen servers also
allow co-op restart and next votes.

Each vote runs for the full **30 seconds**. The caller automatically votes yes.
The server fixes the electorate to the players present when the vote starts;
late joiners can see the vote but cannot vote in it. Each eligible player has
one ballot and cannot change it. A map changes only if at least **51% of that
electorate** participates and at least **51% votes yes**, rounding the required
count up. For example, three players require two yes votes; two players require
both. Abstentions and disconnected players do not reduce the required count.

Only one vote can run at a time. Each player must wait **two minutes** from
their accepted vote call before calling another; unsuccessful votes also count.
The cooldown survives map transitions. Rejected calls do not consume a
cooldown. Ordinary server-driven map changes cancel any active vote.

Exercise the system using real game clients and real vote deadlines:

```sh
python3 tests/network/map-vote.py \
  --binary ./nblood \
  --assets /path/to/blood-1.21 \
  --output /tmp/netblood-map-vote-evidence
```

The test covers no quorum, majority-no, majority-yes, fixed electorates,
duplicate ballots, concurrent calls, cooldowns across maps, disabled voting,
missing rotations, map-target validation, co-op restart and next. A listen
server also restarts a map: its episode/level registry slot, map CRC and server
rules must remain unchanged. It checks
the resulting map generations and preserved server rules on both sides. A
client recording begun during an active vote must restore its vote/options
baseline and replay the result and map transition to the exact final snapshot.
Six ballots and packet playback take about three and a half minutes, plus process startup and shutdown.
Logs, exact commands, statuses and the tested binary hash are retained in
`result.json`. The asset-free `vote-test.sh` and `server-settings-test.sh`
cover clock boundaries, vote policy and map-route edge cases separately. The
policy unit also checks that restarting a GUI-hosted custom map preserves its
standalone identity without registering its map slot a second time.

Use `--listen-restart-only` to isolate the 30-second listen-server restart
check without repeating the dedicated-server ballots and playback.

## Automatic map downloads

Run the focused download integration test under the same display and asset
setup:

```sh
python3 tests/network/map-download.py \
  --binary ./nblood \
  --assets /path/to/blood-1.21 \
  --output /tmp/netblood-map-download-evidence
```

The harness gives only the dedicated server two uniquely named external maps.
By default it copies `CP01.MAP` and `CP02.MAP` from the supplied assets; use
`--first-map` and `--second-map` for other valid external maps. Clients share
the base game assets but never receive the server's map aliases through their
filesystem. A real client joins through 35 ms one-way delay, 10 ms jitter,
8% physical datagram loss and reordering, downloads the first map, then follows
a server map change that requires a second download. The first server map is
launched with an absolute path; that descriptor must remain an opaque name on
the client even though the test processes share one machine. Complete cached
bytes must match the originals, and installed authoritative snapshots must round-trip
exactly. A two-map client packet recording must replay to its final recorded
snapshot using the downloaded cache.

Additional processes check cache reuse without another transfer, redownload of
a corrupt cache entry, joining with an already installed matching map, and
downloading a different version without overwriting the client's installed
map. Cancelling a transfer must leave no cached or temporary map, and another
client must still be able to join. Offline playback without its maps and a
download whose cache-directory path is a regular file must fail gracefully
before installing snapshots. Processes leave through ordinary console commands
or the F10 quit menu when gameplay has not started and console binds are idle.

The evidence directory keeps every process log, map-cache file, packet demo,
binary hash, download fixture hash and relay counters in `result.json`. This
test requires the base game resources on both sides: automatic map downloading
does not distribute the game's archives or custom art/sound resources.

## Missing or different mod assets

Run the content diagnostic integration test under the same display and asset
setup:

```sh
python3 tests/network/content-mismatch.py \
  --binary ./nblood \
  --assets /path/to/blood-1.21 \
  --output /tmp/netblood-content-mismatch-evidence
```

The server's fixture registers three unused external DAT resources through
`blood.def`, adds a valid ART containing one unused tile, and overrides a loose
weapon sound. A joining client has the same definitions but lacks one DAT, the
ART and the RAW sound. It has different bytes in a second DAT of the same
length and an exact third DAT. The client must
report the missing and different files individually in its log and saved
`net-content-mismatch.txt`, including the server's expected filename, size in
bytes and MD5 digest. The test calculates expected lengths and digests with
Python independently of the game. Matching files must not be reported, no
installed client files may change, and no map download or authoritative state
installation may begin. A second client is missing only the loose RAW sound:
the manifest must reject it even though that lazy sound override is outside
the legacy aggregate CRC. A third client has exact asset bytes but definitions
that do not register them; it must distinguish files that are present but not
loaded from files that are missing. A client with every exact fixture file and
the correct definitions must then join the same server successfully.

The evidence directory keeps the tested executable's hash, fixture sizes and
MD5s, full logs, saved report, process exits and diagnostic rows in
`result.json`. If ImageMagick's `import` command is available, it also saves a
screenshot of the visible error for review. These resources are intentionally
unused by gameplay, so the check can isolate content negotiation from malformed
mod data and missing stock game assets.

Exercise the content negotiation state machine separately:

```sh
python3 tests/network/content-handshake.py \
  --binary ./nblood \
  --assets /path/to/blood-1.21 \
  --output /tmp/netblood-content-handshake-evidence
```

A small ENet peer exchanges real UDP packets with the production dedicated
server and graphical client. The checks cover repeated Hello suppression,
wrong-session, truncated and wrong-CRC Ready messages, a server map change
during manifest negotiation, compatibility with old CRC-only clients, malformed
or truncated manifests, and actionable rejection when a server omits the
required manifest.

## Standalone impairment relay

```sh
python3 tests/network/relay.py --target 127.0.0.1 --target-port 23513 \
  --port 23514 --loss 0.08 --delay-ms 55 --jitter-ms 15 --reorder 0.1 \
  --seed 29 --report /tmp/relay.json
```

Connect one game client to port 23514. Use a separate relay port for each
client. The relay pins the first client's address and never interprets or
acknowledges game messages. Its queue and fingerprint inventory are bounded;
overflow is reported separately from requested loss. The seed fixes impairment
decisions for an identical datagram arrival sequence; it does not make live
network scheduling deterministic. Stop it with SIGINT/SIGTERM to save counters.

These checks establish the tested local scenarios and parser/resource bounds.
They do not establish Internet-scale load capacity, hostile-network security,
cross-platform simulation parity, or exhaustive gameplay/map/mod compatibility.

## Cooperative cinematics

The standalone checks exercise the production packet codec, participant
barrier, skip timer and episode/movie selection, including malformed packets,
clock wrap, focus changes and stock versus custom INI metadata. The OGV
decoder checks generate synthetic Theora/Vorbis fixtures with `ffmpeg`; they
exercise frame timing, color conversion, audio, completion and damaged files
without needing game assets.

```sh
tests/network/cinematics-test.sh
SANITIZE=1 tests/network/cinematics-test.sh
tests/network/cinematic-levels-test.sh
SANITIZE=1 tests/network/cinematic-levels-test.sh
tests/network/cinematic-assets-test.sh
SANITIZE=1 tests/network/cinematic-assets-test.sh
tests/network/cinematic-ogv-test.sh
SANITIZE=1 tests/network/cinematic-ogv-test.sh
```

Co-op servers synchronize episode intros, map cinematics and episode endings.
Each player watches their locally installed Smacker (`.SMK`) or Ogg Theora
(`.ogv`) version. Missing or unreadable movies show white waiting text on a
black screen. Holding Space continuously for three seconds skips only the
local movie; releasing Space or losing focus resets the hold. Finished and
skipped players wait until all current participants finish. A join during a
movie participates in that movie; later joins do not replay finished movies.
Disconnecting a watcher removes them from the barrier. The server continues
servicing UDP and joins while world simulation is paused.

Episode movies use the INI `CutSceneA`/`CutSceneB` and `CutWavA`/`CutWavB`
settings; map movies use `CutScene` and `CutWav`. A matching `.ogv` replacement
is preferred to `.smk`. The game searches its resource paths and the `movie`,
`movies`, `video`, `videos` and `cutscenes` directories. Stock episode movie
filenames are recognized when the original Blood INI has its movie keys
commented out. Movie files remain optional for joining the server.

```sh
python3 tests/network/cinematics.py \
  --binary ./nblood \
  --assets /path/to/blood-1.21 \
  --output /tmp/netblood-cinematics-evidence
```

Run under X11 or Xvfb. The asset directory must contain the original
`CRYPTIC.SMK`; no game movies are stored in this repository. The harness makes
Theora/Vorbis test clips using `ffmpeg`, builds small legal Blood maps and a
custom episode INI, and uses ordinary movement to activate their exit sectors.
It checks dedicated and listen servers, natural episode and map transitions,
SMK and replacement OGV decoding, missing-movie waiting screens, personal skip
hold timing and release reset, frozen world ticks, hot joins, disconnects and
packet-demo playback. Packet demos omit cinematic control messages and the
paused interval; their gameplay snapshots remain replayable offline without
waiting for remote players.

Screenshot checks use Pillow, `xwininfo`, `xprop`, ImageMagick `import` and Xlib.
They verify a black waiting screen with white text and visible color movie
frames, and save captures in the evidence directory. `--skip-screenshots`
omits pixel checks; X11 tools and Xlib are still used to focus each game window
before injecting real SDL key events. `--renderer 3` or `--renderer 5` also
exercises the Polymost or Vulkan cinematic presentation path when enabled in
the tested binary. `result.json` contains the tested binary digest, commands,
phase results, movie timing, screenshots, logs and demo playback comparison.

## Multiplayer scoreboard

`protocol-test.sh` includes scoreboard metadata roundtrips, malformed/truncated
packet rejection, signed scores and connection-age clock boundaries. The live
harness uses a dedicated server and real clients to check names, negative
scores after a fatal fall, server-measured latency through a delayed UDP relay,
late joins, departures, slot reuse, map changes and exact packet-demo replay.
It waits for the real first connection-minute boundary; `--quick` omits that
wait when checking another renderer with the same executable.

```sh
python3 tests/network/scoreboard.py \
  --binary ./nblood --assets /path/to/blood-1.21 \
  --output /tmp/netblood-scoreboard-evidence
```

Run under X11/Xvfb. Screenshots use the same Pillow/ImageMagick/Xlib dependencies
as the cinematic harness; `--skip-screenshots` omits captures. The default F11
binding is exercised by holding the key, and a second client verifies that an
existing custom F11 command remains intact. Local clients must show unknown
country placeholders. A separately labeled synthetic packet-demo copy replaces
only country metadata with known ISO codes to check colored flag rendering;
those countries are never claimed to describe the actual loopback connections.
`--renderer 5 --quick` checks Vulkan presentation. Logs, binary digest, phase
results and screenshots are saved beside `result.json`.
