# Discord Rich Presence

NetBlood can publish its game mode, current map, elapsed playing time, server
address and multiplayer party size to the locally running Discord desktop app
on Windows and Linux. Discord's **Join** control and game invitations use the
same server connection information. The playing timer continues across map
changes. Dedicated servers and automated demo validation do not publish presence.

## One-time application setup

Create an application named **NetBlood** in the
[Discord Developer Portal](https://discord.com/developers/applications), then
copy its numeric **Application ID**. This is a public identifier; no bot token,
client secret, OAuth login or Discord account password belongs in NetBlood.
The application's name determines the game name Discord displays.

Open NetBlood's console and configure the ID, replacing the placeholder with
the actual number:

```text
discord_appid "YOUR_APPLICATION_ID"
discord_enabled 1
discord_status
```

These settings are saved with the normal game configuration. No application ID
is supplied by default, so Rich Presence remains inactive until one is set.
For a release, use the same project-owned Discord application for all players.
Discord must be running and signed in on the same computer. NetBlood retries
when Discord starts later or restarts, without requiring a game restart.

Disable the integration with `discord_enabled 0`. `discord_status` reports the
current configuration and connection state.

## Joining and inviting

Once a player is connected to a multiplayer server, presence includes its
numeric IP address and port, game mode, map, and current/maximum player count.
Friends can use Discord's Join control or send a game invitation through
Discord's chat UI. Acceptance delivers the server address to NetBlood; it is
validated before connecting. A successful join action switches the receiving
player to that server.

NetBlood registers a per-user `discord-APPLICATION_ID://` launcher when the
configured application is activated. This lets Discord start a closed game
before delivering the actual join information over local IPC. `discord_register`
retries registration manually, for example after moving a portable installation.
Linux registration requires `xdg-utils`. The existing `netblood://` link handler
continues to serve ordinary website and chat links.

Clients connected over a LAN/private address use the server's
[STUN-discovered public endpoint](stun.md) when one is available. Clients
connected to a public address preserve that working address and port, including
IPv6. A graphical listen host also uses discovery automatically.
To specify a reachable address explicitly, overriding discovery for the host:

```text
discord_server_address "netblood://server.example:23513"
```

This setting applies to the listen host. Connected clients use the server's
discovered endpoint or their actual connection endpoint. Clear the override
with `discord_server_address ""`.
The address must be reachable by the invited player; Discord does not provide
game traffic relay or NAT traversal. Existing
map download, asset checks and protocol compatibility checks still apply.

Offline games, demo playback, pending connections and full servers do not offer
joining. Presence updates are coalesced to respect Discord's update limits;
leaving a joinable session removes its join information promptly.

## Implementation and verification

The integration uses Discord's documented local RPC protocol over Unix sockets
on Linux and named pipes on Windows. IPC runs on a background thread; validated
join requests are applied at a safe point on the game thread. The game does not
send messages or invitations on a user's behalf.

No proprietary Discord SDK is linked. JSON handling uses the repository's
existing BSD-2-Clause `sjson` code, which is compatible with GPLv2.

References:

- [Discord RPC documentation](https://github.com/discord/discord-api-docs/blob/main/developers/topics/rpc.mdx)
- [Discord native IPC guide](https://github.com/discord/discord-rpc/blob/master/documentation/hard-mode.md)

Automated checks use isolated mock Discord IPC endpoints and real game clients.
They do not authenticate to Discord. Verifying Discord's visible profile,
Join control and chat invitations additionally requires a real Application ID
and Discord accounts.

For that final manual check, configure the same Application ID on two computers
with Discord running under different accounts:

1. Join a reachable server and check the first player's profile for the map,
   game mode, server address, player count and elapsed time.
2. Use Join from the other account and confirm both players reach the same server.
3. Send a game invitation using Discord's chat UI. Accept it once with NetBlood
   running and once after closing NetBlood, checking that it launches and joins.
4. Change maps and then disconnect; the timer should continue across maps, and
   the old server's Join control should disappear after disconnecting.
