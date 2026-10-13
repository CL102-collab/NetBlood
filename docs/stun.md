# Public STUN discovery

NetBlood automatically discovers a hosted game's public UDP address using
Google's public STUN servers. This applies to dedicated and graphical listen
servers on Windows and Linux. No relay server, account, or additional hosted
service is required.

The defaults are `stun.l.google.com:19302`, `stun1.l.google.com:19302`, and
`stun2.l.google.com:19302`. DNS resolution runs in the background, and discovery
uses the actual ENet game socket, including its listening port. A failed server
is retried through the next endpoint; game startup and existing connections do
not wait for discovery. DNS failures retain a diagnostic and retry after
30 seconds without blocking the game. Google operates these public services
independently of NetBlood; availability is not guaranteed.

## Configuration

The default configuration is equivalent to:

```sh
./nblood -dedicated -port 23513 -gametype bloodbath -map E1M1 -stunserver auto
```

Choose another standard STUN server, or disable discovery:

```sh
./nblood -dedicated -stunserver stun.example:3478
./nblood -dedicated -stunserver off
```

The saved console variable is `net_stun_server`. Accepted values are `auto`,
`off`, an empty string (also disabled), or a server hostname/IP with optional
port. Bracket IPv6 addresses, for example `[2001:db8::20]:3478`. A
`netblood://` prefix is accepted for service addresses; these are interpreted
as STUN endpoints only when supplied to `-stunserver` or `net_stun_server`.
Custom endpoints default to port 3478. Use `off` to disable discovery on the
command line; `-stunserver=` also supplies an empty value.

Changes take effect the next time the game starts hosting. `-stunserver`
takes precedence over saved and autoexec settings when hosting starts. Clients
connecting to an ordinary dedicated server do not contact public STUN themselves.

Run `net_stun_status` to see the discovery state and the mapped endpoint.
The server also prints a `netblood://IP:port` link when discovery succeeds.
The mapped port may differ from the server's listening port.

## Invitations and reachability

A listen host with Discord Rich Presence enabled can use the discovered
endpoint automatically. An explicit `discord_server_address` continues to
override discovery for that host. Connected clients receive the discovered
endpoint from the game server so their Discord invitations can use its public
address when they joined over a LAN/private address. Clients already connected
to a public address keep sharing that working address and port.

Discovery metadata refreshes when the mapping changes and expires when STUN
responses stop. It is separate from gameplay state and is excluded from packet
demos. Offline games, demo playback and full servers remain non-joinable in
Discord.

**STUN discovery does not prove that incoming connections can reach the server.**
It reports the mapping seen by the STUN server. Some NATs use a different mapping
for each destination or reject incoming traffic from peers the host has not
contacted. Google STUN does not introduce game peers, coordinate UDP hole
punching, or relay game traffic.

A server behind restrictive NAT or CGNAT can therefore remain unreachable.
Use port forwarding where the router/ISP permits it, or host a dedicated server
on a publicly reachable address. This implementation does not claim coordinated
hole punching or universal CGNAT traversal. A client that receives no initial
server response reports a connection error with these options.

## Validation

Tests use local STUN fixtures and real ENet/game sockets, without contacting
public services. They cover RFC 5769 IPv4/IPv6 responses, transaction/source
validation, malformed data, primary-server failure and fallback, stale mapping
expiry, disablement, unchanged reliable/fragmented game traffic and packet
triplication, live host/client presence, DNS failure recovery, connection-timeout
diagnostics, map changes, and packet-demo isolation.
Public Google STUN reachability and a particular ISP's NAT behavior require
verification from that network.
