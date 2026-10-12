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
