# Independent ZeroTier UDP companion

`netblood-zerotier` is a separate process containing the ZeroTier Sockets SDK.
It provides generic UDP sockets over an authenticated IPv4 loopback protocol;
the game, ENet, game discovery and content-transfer code stay in the game
process. It creates no system virtual network adapter and needs no installed
ZeroTier service or administrator privileges.

## Build and package

Python 3.9+, CMake 3.12+, and a C/C++ compiler are required. The helper uses
C++17; the games retain their existing compiler requirements. From the source
root:

```sh
python3 tools/zerotier/build.py --output-dir dist/zerotier
```

The explicit build step downloads four pinned source archives and verifies
their SHA256 hashes against `sources.json`. It patches the pinned SDK's old
CMake configuration for current CMake and portable Windows library selection,
static SDK linkage, and the bundled miniupnpc Windows timeout ABI, then builds
a static SDK inside the companion. It does not install anything
system-wide. The game never downloads or builds its companion at runtime.

Copy the generated executable beside the games and include the generated
`zerotier-licenses` directory and `zerotier-build-info.json` in the package.
The Linux/macOS executable is `netblood-zerotier`; Windows uses
`netblood-zerotier.exe`. The two games share the same companion executable.
The helper is MIT-licensed and separately distributed from the GPL games;
the SDK's current Apache 2.0 and third-party licenses accompany that helper.

`--work-dir` selects the source/build cache, `--jobs` limits compilation,
`--cmake` selects CMake, and `--offline` requires pre-cached verified archives.
Use a distinct work directory for each target toolchain. Pass CMake options
after `--`, for example:

```sh
# Windows using an existing MinGW CMake toolchain file:
python3 tools/zerotier/build.py --work-dir .ci-build/zerotier-win64 \
  --output-dir dist/zerotier-win64 -- -DCMAKE_TOOLCHAIN_FILE=/path/to/mingw.cmake

# Native macOS build; build both slices separately when packaging a universal app:
python3 tools/zerotier/build.py --work-dir .ci-build/zerotier-arm64 \
  --output-dir dist/zerotier-arm64 -- \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0
```

Linux x86-64 has been compiled and the actual SDK startup/IPC lifecycle tested.
Windows x86-64 has been cross-compiled with static GCC/C++/pthread runtimes;
its executable imports only Windows system DLLs. Windows runtime and native
macOS build/runtime validation still require their respective platforms.
Legacy Windows XP and no-SSE2
targets must keep ZeroTier unavailable unless the companion is built and
validated for their advertised platform baseline. A missing companion must not
prevent normal Internet multiplayer or offline play.

## Runtime and network setup

The game launches the helper with a parent loopback UDP port, a fresh 128-bit
token in a private child environment variable, a 16-digit ZeroTier network ID,
and a private persistent identity directory:

```text
netblood-zerotier --ipc-port PORT --token-env NETBLOOD_ZT_IPC_TOKEN --network 16HEX --identity DIRECTORY
```

The helper binds only `127.0.0.1` and accepts authenticated control datagrams
from that specific parent address and port. The token never appears in command
arguments, and is removed from the child's environment after parsing. It never
logs the token or reads
the persisted identity keys. A filesystem lock prevents concurrent use of the
same identity. Status includes the public node ID so a private network's owner
can authorize it. Network initialization is asynchronous; the parent remains
responsive and reports waiting, authorization denial, errors or readiness.
The managed network must assign IPv4 addresses and allow IPv4 broadcast for
game discovery. There is no administrator credential embedded in the game.

Keepalives are sent once per second. The helper exits after 15 seconds without
an authenticated parent message, or immediately when it receives `Stop`.
Virtual sockets are nonblocking and bounded; each socket has an independent
generation so delayed packets cannot enter a later game session that reuses
its logical socket ID. See the MIT-licensed generic protocol header at
`source/build/include/zerotier_protocol.h` for wire details.

The helper needs access to ZeroTier's UDP infrastructure and the configured
network controller. UDP-blocked environments cannot verify an actual overlay
connection. Controllerless ad-hoc networks cannot support broadcast discovery;
they are not substitutes for the managed shared network used here.

## Verification

```sh
python3 tools/zerotier/test_companion.py --helper dist/zerotier/netblood-zerotier
python3 tools/zerotier/test_bridge.py --sdk-source .ci-build/zerotier/sdk-<cache-id>
```

This runs the real bundled SDK and verifies authenticated loopback IPC, error
responses, identity locking, shutdown and orphan cleanup. It does not claim an
actual two-player overlay connection. The additional bridge test uses a
clearly labelled test-only socket provider to exercise byte forwarding,
idempotent socket opens, port reporting, generation checks and oversize drops
without a network controller; it must never ship as a runtime SDK.
