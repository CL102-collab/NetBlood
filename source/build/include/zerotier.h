// Shared, asynchronous ZeroTier transport and game browser for graphical hosts.
#ifndef BUILD_ZEROTIER_H
#define BUILD_ZEROTIER_H
#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif
#include "enet.h"

#include <cstdint>
#include <string>
#include <vector>

#ifndef ZEROTIER_NETWORK_ID
#define ZEROTIER_NETWORK_ID ""
#endif

namespace zerotier
{
constexpr uint16_t GamePort      = 23513;
constexpr uint16_t DiscoveryPort = 23514;
enum class Game : uint8_t
{
    NBlood   = 1,
    Rednukem = 2
};
enum class State
{
    Stopped,
    Starting,
    Waiting,
    Ready,
    Denied,
    Error
};
struct Config
{
    Game        game = Game::NBlood;
    std::string networkId;
    // Empty paths use the per-user game profile and the executable's directory.
    std::string identityPath, helperPath;
};
struct Advertisement
{
    std::string map, mode;
    int         players = 0, maxPlayers = 0;
    bool        password = false;
    uint32_t    protocol = 0;
};
struct Server : Advertisement
{
    std::string address;
    uint16_t    port = GamePort;
    int         ping = -1;
};
// Starting and joining the overlay never wait for Internet access. pump() must
// also run while menus are open. Selecting another transport need not stop it.
bool                       start(const Config &config, std::string &error);
void                       stop();
void                       pump();
bool                       ready();
State                      state();
const std::string         &status();
std::string                localAddress();
void                       advertise(const Advertisement *value);  // nullptr withdraws the game.
void                       browse(bool enabled);
const std::vector<Server> &servers();
// Address.port is the fixed game port for hosts; nullptr binds a client port.
// The returned ENet host owns its virtual socket. All game packets remain ENet.
ENetHost *createHost(ENetAddress *address, size_t peers, size_t channels, enet_uint32 incomingBandwidth, enet_uint32 outgoingBandwidth);
}  // namespace zerotier
#endif
