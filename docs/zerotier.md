# ZeroTier multiplayer

NBlood and Rednukem can host and join games over a bundled ZeroTier connection.
Players do not need to install ZeroTier, create a system network adapter, run as
administrator, or configure a forwarded game port. ZeroTier attempts a direct
encrypted connection and can use its relay infrastructure when a direct path is
unavailable. Relay latency can be higher than a direct connection.

## Playing

Open **Multiplayer** and choose a connection:

- **Internet** opens the existing **Host** and **Join** menus. Addresses, ports,
  STUN discovery, and ordinary game links keep their existing behavior.
- **ZeroTier** connects to the distribution's shared game network and offers
  **Browse Games** and **Host Game**.

In **Host Game**, select the maximum player count and an optional password, then
start the game using the existing game settings. The host plays normally; a
dedicated server is not required. The maximum includes the host. The ZeroTier
game port is fixed at **23513** and is not editable in these menus.

**Browse Games** shows each active game's current/maximum players, map, game
mode, password status, and measured round-trip ping. Full games stay visible.
Select a game to join; protected games ask for their password. Refreshing the
list does not create a player connection or reserve a slot. NBlood and Rednukem
list their own games on the shared network.

Players can join after a level has started. The existing multiplayer connection
checks passwords and content, transfers missing user maps and supported mods,
and sends the current world snapshot. An automatic restart to activate a
downloaded mod keeps the ZeroTier network and join password. The host and other
players continue while a joining player downloads or loads content.

The base game's licensed data is still required. Passwords use the existing
game protocol and are never included in browser advertisements. ZeroTier
encrypts overlay traffic; game passwords remain separate from network admission.
Ordinary Internet URI/Discord invitations do not describe a ZeroTier network;
use the ZeroTier browser to join these games.

## Provisioning the shared network

There is deliberately no invented or third-party network ID in the source.
Before publishing a usable ZeroTier build, the distributor must create and
operate a managed ZeroTier network and configure its default ID. Without an ID,
the ZeroTier menus show that the network has not been set up; Internet play
continues to work.

The controller must:

1. Assign IPv4 addresses to members and allow members to reach each other.
2. Permit broadcast/multicast discovery and UDP game traffic on ports 23513
   and 23514 within the virtual network. The game uses 23514 for discovery.
3. Admit new players automatically if joining is to require no administrator
   action. A private network that requires individual authorization will leave
   first-time players waiting until its owner authorizes their node IDs.
4. Have sufficient membership capacity for the expected player population.

Use a controller/network admission policy suitable for a public game community.
Keep controller credentials outside game packages. The game needs only the
16-digit hexadecimal network ID. A game password controls entry to that game;
it does not authorize membership in a private ZeroTier network.

Controllerless ZeroTier ad-hoc networks do not support broadcast/multicast and
cannot provide this game browser. Use a managed network. No external game
directory or master-server account is needed for discovery within that network.

Set the distribution default when compiling both games:

```sh
make -j4 ZEROTIER_NETWORK_ID=0123456789abcdef
```

Replace the example ID with the real network ID. For development, each game's
console can set and save `net_zerotier_network 0123456789abcdef`. Clear an override
with `net_zerotier_network ""`. Disconnect before changing the network of an
active ZeroTier game.

Release workflows also read the repository variable `ZEROTIER_NETWORK_ID` and
pass it into the platform builds. The standard Windows, Linux and macOS packages
bundle the companion. Existing XP, no-SSE2 and BSD package targets retain
Internet multiplayer and explicitly identify the companion as unavailable until
their SDK/CPU combination is validated; their installation notes describe this.

For command-line hosting or joining, use the same ID on both ends:

```sh
./nblood -zerotier 0123456789abcdef -server 8 -gametype bloodbath
./nblood -zerotier 0123456789abcdef -client 10.10.0.2

./rednukem -zerotier 0123456789abcdef -server 8
./rednukem -zerotier 0123456789abcdef -client 10.10.0.3
```

The client addresses above are examples of the hosts' assigned ZeroTier IPv4
addresses, not their public Internet addresses. Normal interactive play uses
the browser instead. `-zerotier` without an ID uses the configured default.

## Building and distributing the companion

The games use a separately executable **netblood-zerotier** companion, which is
bundled alongside the game executable (`netblood-zerotier.exe` on Windows). It
embeds libzt and runs without a system ZeroTier service. The game executables do
not link the ZeroTier SDK. The companion exposes a bounded, authenticated local
datagram interface and does not interpret game packets or game passwords.

Build the companion explicitly with Python, CMake, and a C/C++ toolchain:

```sh
python3 tools/zerotier/build.py --work-dir .ci-build/zerotier \
  --output-dir dist/zerotier --jobs 4
```

Copy the companion, `zerotier-licenses`, and `zerotier-build-info.json` from the
output directory into the game distribution. Keep the notices with the
companion. Build-time dependencies are fetched at fixed revisions and checked
against SHA-256 digests; `--offline` requires already verified cached archives.
The game never downloads or executes a networking SDK at runtime.

The source manifest, portability patch, independent companion license, original
dependency notices, and license transition information are under
[`tools/zerotier`](../tools/zerotier). Building the game alone still supports
Internet play; selecting ZeroTier without its companion reports the missing
component.

Each game stores a persistent ZeroTier node identity in its user profile. Treat
the private identity as private data and do not include it in releases or copy
it between simultaneously running players. Browsing and disconnecting do not
recreate the node. The companion exits when the game shuts down.

## Validation

Standalone tests cover the custom ENet backend, bounded discovery messages,
browser refresh/expiry, and the local companion protocol without needing game
data or a controller. The normal native UDP, password, map-download, and
addon-restart checks continue to apply. The graphical menu checks need the
appropriate game data and an X11 display.

Before release, use two machines on the provisioned network to check discovery,
full-game visibility, a protected listen host, late joining, a missing user map,
and a downloaded mod that restarts and reconnects. Exercise both direct and
relayed ZeroTier paths. Local transport fixtures do not establish Internet NAT
traversal or the availability of the production controller.
