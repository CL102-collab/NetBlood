// SPDX-License-Identifier: GPL-2.0-or-later
// Keep offline games and legacy demos usable in builds configured NETCODE=0.
#include "duke3d.h"
#include "rnet.h"

#ifdef NETCODE_DISABLE
namespace rnet
{
bool zeroTierSelected = false;
char zeroTierNetwork[17] = "";
namespace
{
void unavailable() { OSD_Printf("Multiplayer is unavailable in this NETCODE=0 build.\n"); }
int statusCommand(osdcmdptr_t) { unavailable(); return OSDCMD_OK; }
}
bool active() { return false; }
bool server() { return false; }
bool client() { return false; }
bool dedicated() { return false; }
bool playback() { return false; }
bool inGame() { return false; }
bool zeroTier() { return false; }
bool waitingForZeroTier() { return false; }
std::string zeroTierNetworkId() { return {}; }
bool startZeroTier() { unavailable(); return false; }
bool zeroTierReady() { return false; }
std::string zeroTierStatus() { return "Multiplayer unavailable in this build."; }
bool selectZeroTier(bool) { unavailable(); return false; }
bool hostZeroTier(int, const char*) { unavailable(); return false; }
uint32_t tick() { return 0; }
uint32_t mapSerial() { return 0; }
int localPlayer() { return myconnectindex; }
int maxPlayers() { return 1; }
bool connected(int player) { return player == myconnectindex; }
bool playing(int player) { return player == myconnectindex; }
uint32_t connectedSince(int) { return 0; }
uint32_t ping(int) { return 0; }
std::string address(int) { return {}; }
std::string endpoint() { return {}; }
_ENetHost* host() { return nullptr; }
_ENetPeer* peer(int) { return nullptr; }
void send(int, uint8_t, const std::vector<uint8_t>&, bool) {}
void broadcast(uint8_t, const std::vector<uint8_t>&, bool) {}
void disconnect(const std::string&) {}
void reject(int, const std::string&) {}
void connect(const std::string&, bool, const std::string&) { unavailable(); }
bool passwordRetryPending() { return false; }
void retryPassword() { unavailable(); }
void cancelPasswordPrompt() {}
void setDedicated(bool) {}
void setMaxPlayers(int) {}
void queueMap(const std::string&, bool) { unavailable(); }
std::string mapName() { return {}; }
void mapLoaded(const char*) {}
void contentReady(int) {}
void fail(const std::string& reason) { OSD_Printf("NET error: %s\n",reason.c_str()); }
void status() { unavailable(); }
void pump() {}
void startup() {}
void shutdown() {}
void gameTick() {}
void draw() {}
void registerCommands()
{
    for (const char* name : {"net_status","net_disconnect","net_record","net_stoprecord","net_playback"})
        OSD_RegisterFunction(name,"Multiplayer unavailable in a NETCODE=0 build",statusCommand);
}
bool earlyArguments(int&, const char* const*&, int&) { return false; }
bool argument(int, const char* const* argv, int& index)
{
    const char* arg = argv[index];
    if (!Bstrcasecmp(arg,"-dedicated") || !Bstrcasecmp(arg,"-server") ||
        !Bstrcasecmp(arg,"-client") || !Bstrcasecmp(arg,"-connect") ||
        !Bstrcasecmp(arg,"-port") || !Bstrcasecmp(arg,"-password") || !Bstrcasecmp(arg,"-zerotier"))
    {
        initprintf("Multiplayer requires a build compiled with NETCODE=1.\n");
        G_GameExit("Multiplayer requires a build compiled with NETCODE=1.");
        ++index;
        return true;
    }
    return false;
}
bool record(const char*) { unavailable(); return false; }
void stopRecord() {}
bool play(const char*) { unavailable(); return false; }
} // namespace rnet
#endif
