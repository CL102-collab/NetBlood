// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef BLOOD_DISCORD_RPC_H
#define BLOOD_DISCORD_RPC_H

#include <cstdint>
#include <memory>
#include <string>

// Discord's documented local RPC protocol. No Discord SDK, credentials or
// connection to an Internet service is needed by this client.
namespace discordrpc
{
struct Activity
{
    std::string details;
    std::string state;
    std::string joinSecret;
    std::string partyId;
    int64_t startedAt = 0;
    int partySize = 0;
    int partyMax = 0;
};

struct Event
{
    enum Kind { Ready, Disconnected, Error, Join };
    Kind kind = Disconnected;
    std::string text;
};

bool validApplicationId(std::string const& applicationId);

// All IPC and JSON processing happen on a background thread. Public methods
// are intended for the main thread; no callback invokes game code.
class Client
{
public:
    Client();
    ~Client();
    void start(std::string const& applicationId);
    void stop();
    void setActivity(Activity const& activity);
    bool poll(Event& event);

    Client(Client const&) = delete;
    Client& operator=(Client const&) = delete;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}

#endif
