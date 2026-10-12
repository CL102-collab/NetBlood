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
