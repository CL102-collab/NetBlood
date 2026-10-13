// SPDX-License-Identifier: GPL-2.0-or-later
#include "discord_presence.h"
#include "discord_rpc.h"
#include "neturi.h"
#include "neturireg.h"
#include "common_game.h"
#include "blood.h"
#include "demo.h"
#include "levels.h"
#include "menu.h"
#include "netdemo.h"
#include "netserver.h"
#include "view.h"

#include <ctime>
#include <memory>

int gDiscordEnabled = 1;
char gDiscordApplicationId[21] = {};
char gDiscordServerAddress[300] = {};

namespace
{
std::unique_ptr<discordrpc::Client> client;
bool initialized = false, ready = false;
std::string runningApplication, launchApplication, registrationAttempt, registrationError;
std::string pendingJoin;
int64_t startedAt = 0;

std::string endpointUri(neturi::Endpoint const& endpoint)
{
    return "netblood://" + (endpoint.host.find(':') == std::string::npos
        ? endpoint.host : "[" + endpoint.host + "]") + ":" + std::to_string(endpoint.port);
}

bool parseJoin(std::string const& text, neturi::Endpoint& endpoint, std::string& error)
{
    if (!neturi::parse(text, endpoint, error)) return false;
    if (endpointUri(endpoint).size() > 128)
    {
        error = "Discord connection links must fit in 128 bytes; use a shorter hostname or a numeric IP address.";
        return false;
    }
    return true;
}

std::string displayText(const char* text, size_t maximum)
{
    // Blood map titles can contain legacy code-page bytes, which are not UTF-8.
    std::string result;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p && result.size() < maximum; ++p)
        result += *p >= 32 && *p < 127 ? char(*p) : '?';
    return result;
}

const char* gameMode()
{
    switch (gGameOptions.nGameType)
    {
    case kGameTypeCoop: return "Cooperative";
    case kGameTypeBloodBath: return "Bloodbath";
    case kGameTypeTeams: return "Teams";
    default: return "Single player";
    }
}

std::string mapTitle()
{
    const char* title = nullptr;
    if (gGameOptions.nEpisode >= 0 && gGameOptions.nEpisode < gEpisodeCount
        && gGameOptions.nLevel >= 0 && gGameOptions.nLevel < gEpisodeInfo[gGameOptions.nEpisode].nLevels)
        title = levelGetTitle();
    return displayText(title && *title ? title : gGameOptions.zLevelName, 90);
}

void registerApplication()
{
    registrationAttempt = gDiscordApplicationId;
    registrationError.clear();
    if (neturi::registerDiscordHandler(registrationAttempt, registrationError))
        LOG_F(INFO, "DISCORD launch handler registered application=%s", registrationAttempt.c_str());
    else
        LOG_F(WARNING, "DISCORD launch registration failed: %s (retry with discord_register)", registrationError.c_str());
}

discordrpc::Activity activity()
{
    discordrpc::Activity result;
    result.startedAt = startedAt;
    // A packet demo keeps displaying its last snapshot after the recording
    // reaches EOF. It remains a snapshot client without a live network mode.
    if (gDemo.at1 || netDemoIsPlaying() || (netIsClient() && gNetMode == NETWORK_NONE))
    {
        result.details = "Watching a demo";
        if (gGameStarted) result.state = mapTitle();
        return result;
    }
    NetPresenceInfo info;
    bool const connected = netGetPresenceInfo(info);
    if (!gGameStarted)
    {
        result.details = "In the main menu";
        if (gNetMode == NETWORK_CLIENT) result.details = "Connecting to a server";
        else if (gNetMode == NETWORK_SERVER) result.details = "Hosting a game";
        return result;
    }
    result.details = std::string(gameMode()) + " | " + mapTitle();
    if (!connected)
    {
        result.state = gNetMode == NETWORK_NONE ? "Offline" : "Multiplayer connection";
        return result;
    }

    neturi::Endpoint endpoint;
    std::string error;
    bool hasEndpoint = false;
    if (info.server)
    {
        if (*gDiscordServerAddress)
            hasEndpoint = parseJoin(gDiscordServerAddress, endpoint, error);
    }
    else
    {
        endpoint.host = info.address;
        endpoint.port = info.port;
        hasEndpoint = parseJoin(endpointUri(endpoint), endpoint, error);
    }
    if (!hasEndpoint)
    {
        result.state = netCinematicActive() ? "Watching a cinematic" : "Hosting a game";
        return result;
    }
    std::string const uri = endpointUri(endpoint);
    result.state = uri.substr(sizeof("netblood://") - 1);
    result.partySize = info.players;
    result.partyMax = info.maxPlayers;
    // Peers can reach the same server through different DNS names or IP families.
    // Its session identifier is shared by everyone and survives map changes.
    char party[64];
    Bsnprintf(party, sizeof(party), "netblood:%08x", info.session);
    result.partyId = party;
    if (info.players < info.maxPlayers) result.joinSecret = uri;
    return result;
}
}

bool discordPresenceValidateSetting(const char* name, const char* value, std::string& error)
{
    if (!Bstrcasecmp(name, "discord_appid"))
    {
        if (!*value || discordrpc::validApplicationId(value)) return true;
        error = "discord_appid must be a nonzero Discord application ID (up to 20 decimal digits), or empty to disable integration.";
        return false;
    }
    if (!Bstrcasecmp(name, "discord_server_address") && *value)
    {
        neturi::Endpoint endpoint;
        return parseJoin(value, endpoint, error);
    }
    return true;
}

bool discordPresenceSetLaunchApplication(const char* applicationId, std::string& error)
{
    if (!discordrpc::validApplicationId(applicationId))
    {
        error = "Discord launch requires a valid nonzero application ID.";
        return false;
    }
    launchApplication = applicationId;
    return true;
}

void discordPresenceInitialize()
{
    if (gDedicated || gDemoRunValidation || initialized) return;
    if (!launchApplication.empty())
    {
        Bstrncpyz(gDiscordApplicationId, launchApplication.c_str(), sizeof(gDiscordApplicationId));
        gDiscordEnabled = 1;
    }
    initialized = true;
    startedAt = static_cast<int64_t>(std::time(nullptr));
    discordPresencePump();
}

void discordPresenceShutdown()
{
    initialized = ready = false;
    pendingJoin.clear();
    if (client) client->stop();
    client.reset();
    runningApplication.clear();
}

void discordPresencePump()
{
    if (!initialized || gDedicated || gDemoRunValidation) return;
    std::string const desired = gDiscordEnabled && discordrpc::validApplicationId(gDiscordApplicationId)
        ? gDiscordApplicationId : "";
    if (runningApplication != desired)
    {
        if (client) client->stop();
        client.reset();
        ready = false;
        pendingJoin.clear();
        runningApplication = desired;
        if (!desired.empty())
        {
            if (registrationAttempt != desired) registerApplication();
            client.reset(new discordrpc::Client);
            client->start(desired);
            LOG_F(INFO, "DISCORD connecting application=%s", desired.c_str());
        }
    }
    if (!client) return;
    discordrpc::Event event;
    while (client->poll(event))
    {
        switch (event.kind)
        {
        case discordrpc::Event::Ready:
            ready = true;
            LOG_F(INFO, "DISCORD connected application=%s", runningApplication.c_str());
            break;
        case discordrpc::Event::Disconnected:
            ready = false;
            LOG_F(INFO, "DISCORD disconnected");
            break;
        case discordrpc::Event::Error:
            LOG_F(WARNING, "DISCORD %s", event.text.c_str());
            break;
        case discordrpc::Event::Join:
        {
            neturi::Endpoint endpoint;
            std::string error;
            if (parseJoin(event.text, endpoint, error))
            {
                pendingJoin = endpointUri(endpoint);
                LOG_F(INFO, "DISCORD join queued %s", pendingJoin.c_str());
            }
            else
            {
                LOG_F(WARNING, "DISCORD rejected join: %s", error.c_str());
                viewSetMessage("Invalid Discord game invitation");
            }
            break;
        }
        }
    }
    client->setActivity(activity());
}

bool discordPresenceHasPendingJoin()
{
    return !pendingJoin.empty();
}

void discordPresenceApplyPendingJoin()
{
    if (!initialized || !gDiscordEnabled || gDedicated || gDemoRunValidation || pendingJoin.empty()) return;
    std::string const uri = pendingJoin;
    pendingJoin.clear();
    neturi::Endpoint endpoint;
    std::string error;
    if (!parseJoin(uri, endpoint, error)) return;
    LOG_F(INFO, "DISCORD joining %s", uri.c_str());
    NetworkJoinEndpoint(endpoint.host.c_str(), endpoint.port);
    discordPresencePump();
}

void discordPresenceRegister()
{
    if (gDedicated || gDemoRunValidation)
    {
        OSD_Printf("Discord is unavailable in dedicated servers and validation runs.\n");
        return;
    }
    if (!discordrpc::validApplicationId(gDiscordApplicationId))
    {
        OSD_Printf("Set discord_appid to your Discord application's numeric ID first.\n");
        return;
    }
    registerApplication();
}

void discordPresencePrintStatus()
{
    const char* state = gDedicated || gDemoRunValidation ? "unavailable"
        : !gDiscordEnabled ? "disabled" : !*gDiscordApplicationId ? "unconfigured"
        : ready ? "connected" : "waiting for Discord desktop";
    LOG_F(INFO, "DISCORD status=%s application=%s", state, *gDiscordApplicationId ? gDiscordApplicationId : "unset");
    if (!registrationError.empty()) LOG_F(WARNING, "DISCORD launch registration: %s", registrationError.c_str());
    if (gNetMode == NETWORK_SERVER && !*gDiscordServerAddress)
        OSD_Printf("Set discord_server_address to netblood://public-host:port to enable invitations from this listen server.\n");
}
