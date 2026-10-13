// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef BLOOD_NETSTUN_CLIENT_H
#define BLOOD_NETSTUN_CLIENT_H

#include "enet.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// STUN discovery on the game's existing ENet UDP socket. All methods run on
// the ENet service thread. DNS belongs to the caller; no threads or peer/socket
// ownership are introduced here. Discovery does not prove inbound reachability.
namespace netstun
{
class Client
{
public:
    Client();
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // Millisecond monotonic clock, supporting uint32 wrap. Does not install an
    // intercept callback; the caller forwards its callback to intercept().
    bool start(ENetHost* host, uint32_t now);
    // Up to eight resolved endpoints; empty disables discovery. Outstanding
    // transactions and old mappings are discarded when configuration changes.
    // Each endpoint gets 10 seconds with one-second retransmissions. Refresh
    // success, or retry an exhausted cycle, after 30 seconds. Mappings expire
    // after 60 seconds without success; every endpoint attempt has a fresh ID.
    bool configure(const std::vector<ENetAddress>& endpoints, uint32_t now, std::string& error);
    void pump(uint32_t now);
    // Consumes cookie-bearing STUN packets before ENet decoding, even malformed
    // or unsolicited ones. Returns zero for ordinary game datagrams.
    int intercept(ENetHost* host);
    // Call before destroying the ENet host. Does not close its socket.
    void stop();

    bool active() const;
    bool discoveredAddress(ENetAddress& address) const;
    std::string status() const;
    std::string error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};
}
#endif
