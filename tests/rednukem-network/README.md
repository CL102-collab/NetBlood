# Rednukem multiplayer integration checks

`run.py` launches the real Rednukem executable in isolated directories. It runs
a dedicated server without a display and with invalid SDL video/audio drivers;
the shared SDL audit library terminates the process if it creates a window or
audio device. Two graphical clients join after simulation has started. Ordinary
SDL key events move a player, and console commands change the map and record a
packet demo. The runner then disconnects everyone, checks continued empty-server
simulation, joins again, and replays the recording through the map transition.
It verifies record CRCs, timeline consistency and independent snapshots, then
compares the final replay tick, snapshot sequence and state hash to the recording.

The server is password protected. A real ENet probe checks rejection of malformed
authentication, a different protocol version, a missing password and an incorrect
password before ordinary clients join with the correct password. Six malformed
external URI invocations must fail before game assets or SDL startup.

The generated Build v7 maps and minimal CON are original test data. Supply Duke
Nukem 3D art and palette assets; the official 1.3D shareware GRP is sufficient.
This does not test the shareware game's original CON scripts. No game assets are
downloaded or checked into the repository by this runner.

```sh
SANITIZE=1 tests/rednukem-network/packet-demo-test.sh
python3 tests/rednukem-network/run.py --binary ./rednukem --assets /path/to/duke3d
```

Linux requires an existing X11 `DISPLAY` (Xvfb is suitable), Python 3, a C
compiler, and SDL2 development headers discoverable by `pkg-config`. Use
`--renderer 3` for OpenGL (default), `--renderer 5` for Vulkan, or `--renderer 0`
for software. In the configured cloud workspace:

```sh
source /workspace/netblood-env/activate-vulkan.sh
make -j8 rednukem RELEASE=0 LTO=0 USE_VULKAN=1
/workspace/netblood-env/with-vulkan-display.sh \
    python3 tests/rednukem-network/run.py \
    --assets /workspace/rednukem-game-data/duke13-shareware
```

`--output` selects a new evidence directory; `--timeout` controls startup/status
waits. The runner keeps the tested executable, input driver, generated fixture,
isolated configurations, logs, packet recording, screenshots, asset/executable
hashes and `result.json`, including failures and process exit codes. Public STUN
discovery is disabled for these local tests.

Additional integration runners share the same generated resources and process
helpers:

```sh
python3 tests/rednukem-network/presence.py --assets /path/to/duke3d
python3 tests/rednukem-network/content.py --assets /path/to/duke3d
python3 tests/rednukem-network/social.py --assets /path/to/duke3d
python3 tests/rednukem-network/sound.py --assets /path/to/duke3d
python3 tests/rednukem-network/rules.py --assets /path/to/duke3d
python3 tests/rednukem-network/scores.py --assets /path/to/duke3d
python3 tests/rednukem-network/revive-score.py --assets /path/to/duke3d
python3 tests/rednukem-network/listen.py --assets /path/to/duke3d
python3 tests/rednukem-network/input-gap.py --assets /path/to/duke3d
python3 tests/rednukem-network/offline.py --assets /path/to/duke3d
python3 tests/rednukem-network/legacy.py --assets /path/to/duke3d \
    --baseline-binary /path/to/pre-port/rednukem
```

The presence suite uses local STUN and Discord test services to exercise the
game's actual discovery socket and invitation callbacks, including cold launches,
full servers and disconnect/playback state. It also exercises password retry and
cancellation through the real masked input form. The content suite exercises custom
resource transfer and game restart. The social suite uses four clients for blank
names, team/private and public chat, scoreboard controls, fixed vote eligibility,
real timeout/cooldown periods, map choices and team balancing. The rules suite
checks revival, eliminated players, runtime life-limit changes, synchronized
cinematics, wipe/intermission timing and map rotation. The scores suite drives
over 300 real player deaths to verify byte-counter wraparound, received scoreboard
scores, map/occupant resets and episode aliases in the map rotation.
The revive-score suite checks that revival clears the previous attacker's kill
credit before a later self-kill.
These suites use real time and can take several minutes.

The sound suite checks dedicated CON sound timing, physical voices through SDL's
dummy audio driver, hot-join loop baselines, RTS sender exclusion, recordings
started during a loop, and listen-host sound deduplication. The standalone
`tests/network/rednukem-sound-test.sh` checks bounded sound-resource parsing and
supports `SANITIZE=1`.

Network map validation supports binary Build versions 7–9 up to 16 MiB;
text-format maps are unsupported.

The listen suite checks host movement and camera motion after a remote player
joins, three reconnects in the same client process across map changes, and a
graphical player using slot 15 while fourteen additional ENet peers hold the
other remote slots. It also checks both disconnect commands and repeats a listen
host's packet recording, requiring the same final state on each playback. The holding
peers authenticate but do not simulate gameplay, so this is not a sixteen-player
load test. Camera comparison requires Pillow.

The input-gap suite drops only client UDP datagrams containing gameplay input
for over 1100 input sequences (about 37 seconds), leaving other traffic intact.
It checks that the connection survives, the server resumes accepting commands,
and authoritative movement recovers without reconnecting.

The offline command smoke checks ordinary `restartmap` and `changelevel` across
both fixture maps, including builds with `NETCODE=0`.

`legacy.py` requires an executable built before the multiplayer changes. It
records real SDL movement in the old input-demo format with state diffs disabled,
then plays the same recording with both binaries. It requires equal simulation
tick counts and the candidate's profiling diagnostics to report zero RNG seed
mismatches against the recording. This checks the generated fixture; it does not
replace a campaign-wide original DOS demo corpus or native rendering parity.

The standalone `tests/network/rednukem-uri-test.sh` and
`tests/network/rednukem-uri-registration.py` check branded URI parsing and desktop
registration. `tests/network/rednukem-social-test.sh` checks the sixteen-player
social models. The local `content-storage-test.sh`, `content-restart-test.sh` and
`content-map-test.sh` check bounded storage, private restart handoffs, map topology
and malformed map data; each supports `SANITIZE=1`. These can run independently
of the graphical integration suite.

`render-bounds-test.sh` runs the production animation-bound helpers against
integer extremes, missing-art fallback underflow, and the complete serialized
weapon-frame range; it supports `SANITIZE=1`. The separate
`tests/network/osd-password-redaction-test.sh` checks password redaction in
console output and history.
