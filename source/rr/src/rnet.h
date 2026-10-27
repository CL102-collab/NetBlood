// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct _ENetHost;
struct _ENetPeer;

// Rednukem's authoritative protocol is independent of Blood and of legacy demos.
namespace rnet
{
constexpr uint32_t Protocol = 1;
constexpr unsigned TickRate = 30;
constexpr size_t MaxMessage = 16u * 1024u * 1024u;
enum Message : uint8_t
{
    Hello = 1, Authenticate, Reject, Welcome, Ready, Input, Snapshot, SnapshotAck,
    ContentFirst = 32, SocialFirst = 64, RulesFirst = 96, PresenceFirst = 128,
};
bool active();
bool server();
bool client();
bool dedicated();
bool playback();
bool inGame();
// Transport choice applies to the next connection. A live session keeps its
// transport while the player visits the other multiplayer menus.
extern bool zeroTierSelected;
extern char zeroTierNetwork[17];
bool zeroTier();
bool waitingForZeroTier();
std::string zeroTierNetworkId();
bool startZeroTier();
bool zeroTierReady();
std::string zeroTierStatus();
bool selectZeroTier(bool enabled);
bool hostZeroTier(int players, const char* password);
uint32_t tick();
uint32_t mapSerial();
int localPlayer();
int maxPlayers();
bool connected(int player);
bool playing(int player);
uint32_t connectedSince(int player);
uint32_t ping(int player);
std::string address(int player);
std::string endpoint();
_ENetHost* host();
_ENetPeer* peer(int player);
// On clients, player=-1 addresses the server. On servers it addresses all peers.
void send(int player, uint8_t type, const std::vector<uint8_t>& data, bool reliable = true);
void broadcast(uint8_t type, const std::vector<uint8_t>& data, bool reliable = true);
void disconnect(const std::string& reason = std::string());
void reject(int player, const std::string& reason);
void connect(const std::string& address, bool useZeroTier = false,
    const std::string& network = std::string());
bool passwordRetryPending();
void retryPassword();
void cancelPasswordPrompt();
void setDedicated(bool value);
void setMaxPlayers(int value);
void queueMap(const std::string& map, bool restart = false);
std::string mapName();
void mapLoaded(const char* path);
void contentReady(int player);
void fail(const std::string& reason);
void status();
void pump();
void startup();
void shutdown();
void gameTick();
void draw();
void registerCommands();
bool earlyArguments(int& argc, const char* const*& argv, int& exitCode);
bool argument(int argc, const char* const* argv, int& index);
bool record(const char* path);
void stopRecord();
bool play(const char* path);
}
