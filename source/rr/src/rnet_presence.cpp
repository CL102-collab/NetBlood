// SPDX-License-Identifier: GPL-2.0-or-later
#include "rnet_presence.h"
#include "rnet.h"
#include "rnet_uri.h"
#include "../../blood/src/discord_rpc.h"
#include "../../blood/src/netstun_client.h"
#include "duke3d.h"
#include "cmdline.h"
#include "timer.h"

#include <cerrno>
#include <ctime>
#include <memory>
#include <mutex>
#include <new>
#ifdef _WIN32
#include <process.h>
#include <wincrypt.h>
#else
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
#endif

namespace rnet { namespace presence {
namespace {
constexpr uint8_t Advertise = 128;
int32_t discordEnabled = 1;
char discordApplication[21] = {};
char discordAddress[300] = {};
char stunServer[300] = "auto";
bool initialized = false, discordReady = false, launchedExternally = false;
std::string launchApplication, pendingJoin, initialJoin, runningApplication;
std::string registeredApplication, registrationError, discoveredUri, remoteUri;
std::string configuredStun, lastStunStatus, lastStunError;
std::string stunOption, applicationOption, addressOption;
bool hasStunOption = false, hasApplicationOption = false, hasAddressOption = false;
std::unique_ptr<discordrpc::Client> discord;
int64_t startedAt = 0;
uint64_t session = 0, remoteSession = 0;
unsigned remotePlayers = 0, remoteCapacity = 0;
std::vector<uint8_t> lastAdvertisement;
ENetHost* attached = nullptr;
ENetInterceptCallback previousIntercept = nullptr;

uint64_t newSession()
{
    uint64_t id = 0;
#ifdef _WIN32
    HCRYPTPROV provider = 0;
    if (CryptAcquireContext(&provider, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT))
    {
        bool ok = CryptGenRandom(provider, sizeof(id), reinterpret_cast<BYTE*>(&id)) != 0;
        CryptReleaseContext(provider, 0);
        if (ok && id) return id;
    }
#else
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0)
    {
        ssize_t count;
        do { count = read(fd, &id, sizeof(id)); } while (count < 0 && errno == EINTR);
        close(fd);
        if (count == sizeof(id) && id) return id;
    }
#endif
    // Party IDs are labels, never authentication credentials. Discovery and
    // direct hosting remain available if the OS random source is unavailable.
    static uint64_t serial = 0;
    id = (uint64_t(std::time(nullptr)) << 32) ^ uint64_t(timerGetTicks()) ^ (++serial << 16);
    return id ? id : 1;
}

bool equal(const char* a, const char* b) { return Bstrcasecmp(a, b) == 0; }
bool isSwitch(const char* value, const char* name)
{
    if (*value != '-' && *value != '/') return false;
    size_t length = Bstrlen(name);
    return !Bstrncasecmp(value + 1, name, length)
        && (value[length + 1] == 0 || value[length + 1] == '=');
}
bool plainSwitch(const char* value, const char* name)
{
    return isSwitch(value, name) && !Bstrchr(value, '=');
}
bool parseJoin(const std::string& value, uri::Endpoint& result, std::string& error)
{
    if (!uri::parse(value, result, error)) return false;
    if (uri::format(result).size() <= 128) return true;
    error = "Discord links must fit in 128 bytes; use a shorter hostname or numeric address.";
    return false;
}
bool parseStun(const std::string& value, uri::Endpoint& result, std::string& error)
{
    if (value.empty() || value.size() >= sizeof(stunServer))
    {
        error = "Use auto, off, or a STUN host[:port].";
        return false;
    }
    std::string link = !Bstrncasecmp(value.c_str(), "rednukem://", 11) ? value : "rednukem://" + value;
    if (!uri::parse(link, result, error)) return false;
    std::string authority = link.substr(11);
    if (!authority.empty() && authority.back() == '/') authority.pop_back();
    bool explicitPort = authority.front() == '[' ? authority.find("]:") != std::string::npos
        : authority.find(':') != std::string::npos;
    // STUN's default port is 3478, distinct from the game port.
    if (!explicitPort) result.port = 3478;
    return true;
}
bool validateSetting(const char* name, const char* value, std::string& error)
{
    if (equal(name, "discord_appid"))
    {
        if (!*value || discordrpc::validApplicationId(value)) return true;
        error = "discord_appid requires a nonzero numeric Discord application ID (at most 20 digits).";
        return false;
    }
    if (equal(name, "discord_server_address") && *value)
    {
        uri::Endpoint endpoint;
        return parseJoin(value, endpoint, error);
    }
    if (equal(name, "net_stunserver") && !equal(value, "auto") && !equal(value, "off"))
    {
        uri::Endpoint endpoint;
        return parseStun(value, endpoint, error);
    }
    return true;
}
int settingCommand(osdcmdptr_t args)
{
    std::string error;
    if (args->numparms && !validateSetting(args->name, args->parms[0], error))
    {
        OSD_Printf("%s\n", error.c_str());
        return OSDCMD_OK;
    }
    return osdcmd_cvar_set(args);
}

#ifndef NETCODE_DISABLE
netstun::Client stun;
struct Lookup
{
    std::mutex mutex;
    bool done = false;
    std::vector<uri::Endpoint> names;
    std::vector<ENetAddress> addresses;
    std::string error;
};
std::shared_ptr<Lookup> lookup;
std::vector<uri::Endpoint> lookupNames;
bool retryLookup = false;
uint32_t retryAt = 0;
std::string setupError;
int ENET_CALLBACK intercept(ENetHost* host, void* event)
{
    int result = stun.intercept(host);
    return result ? result : previousIntercept ? previousIntercept(host, event) : 0;
}
void resolve(const std::shared_ptr<Lookup>& item)
{
    std::vector<ENetAddress> addresses;
    std::string error;
#ifdef _WIN32
    WSADATA data;
    bool ready = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    if (!ready) error = "Cannot initialize STUN DNS networking.";
#else
    bool ready = true;
#endif
    if (ready)
        for (auto& name : item->names)
        {
            ENetAddress address{};
            if (!enet_address_set_host(&address, &name.host[0]))
            {
                address.port = name.port;
                addresses.push_back(address);
            }
        }
#ifdef _WIN32
    if (ready) WSACleanup();
#endif
    std::lock_guard<std::mutex> lock(item->mutex);
    item->addresses = std::move(addresses);
    item->error = std::move(error);
    item->done = true;
}
#ifdef _WIN32
unsigned __stdcall resolveThread(void* arg)
#else
void* resolveThread(void* arg)
#endif
{
    std::unique_ptr<std::shared_ptr<Lookup>> item(static_cast<std::shared_ptr<Lookup>*>(arg));
    resolve(*item);
#ifdef _WIN32
    return 0;
#else
    return nullptr;
#endif
}
void beginLookup(uint32_t now)
{
    if (lookup || lookupNames.empty() || !attached) return;
    auto item = std::make_shared<Lookup>();
    item->names = lookupNames;
    auto* argument = new (std::nothrow) std::shared_ptr<Lookup>(item);
    bool started = false;
    if (argument)
    {
        // Thread creation must remain recoverable in the -fno-exceptions build.
#ifdef _WIN32
        uintptr_t thread = _beginthreadex(nullptr, 0, resolveThread, argument, 0, nullptr);
        started = thread != 0;
        if (started) CloseHandle(reinterpret_cast<HANDLE>(thread));
#else
        pthread_attr_t attributes;
        if (!pthread_attr_init(&attributes))
        {
            if (!pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED))
            {
                pthread_t thread;
                started = !pthread_create(&thread, &attributes, resolveThread, argument);
            }
            pthread_attr_destroy(&attributes);
        }
#endif
    }
    retryLookup = !started;
    if (started) lookup = std::move(item);
    else
    {
        delete argument;
        setupError = "Cannot start STUN DNS worker; retrying in 30 seconds.";
        retryAt = now;
    }
}
std::string stunStatus()
{
    if (lookup) return "resolving STUN servers";
    if (retryLookup) return "DNS unavailable; retry pending";
    if (!setupError.empty()) return "unavailable";
    return stun.status();
}
std::string stunError() { return setupError.empty() ? stun.error() : setupError; }
void startDiscovery()
{
    if (!attached || !rnet::server() || rnet::zeroTier() || equal(stunServer, "off")) return;
    lookupNames.clear();
    if (equal(stunServer, "auto"))
        for (const char* name : { "stun.l.google.com", "stun1.l.google.com", "stun2.l.google.com" })
        {
            uri::Endpoint endpoint;
            endpoint.host = name;
            endpoint.port = 19302;
            lookupNames.push_back(endpoint);
        }
    else
    {
        uri::Endpoint endpoint;
        if (!parseStun(stunServer, endpoint, setupError)) return;
        lookupNames.push_back(endpoint);
    }
    const uint32_t now = timerGetTicks();
    if (!stun.start(attached, now))
    {
        setupError = stun.error();
        return;
    }
    previousIntercept = attached->intercept;
    enet_host_set_intercept(attached, intercept);
    beginLookup(now);
}
void updateDiscovery()
{
    const uint32_t now = timerGetTicks();
    if (retryLookup && now - retryAt >= 30000) beginLookup(now);
    if (lookup)
    {
        auto item = lookup;
        std::lock_guard<std::mutex> lock(item->mutex);
        if (item->done)
        {
            std::string error = item->error;
            if (item->addresses.empty())
            {
                if (error.empty()) error = "Could not resolve STUN servers.";
                error += " Retrying in 30 seconds.";
            }
            else stun.configure(item->addresses, now, error);
            setupError = error;
            retryLookup = !error.empty();
            if (retryLookup) retryAt = now;
            lookup.reset();
        }
    }
    stun.pump(now);
    ENetAddress observed;
    std::string discovered;
    if (stun.discoveredAddress(observed))
    {
        char numeric[64];
        if (!enet_address_get_host_ip(&observed, numeric, sizeof(numeric)))
        {
            const uint8_t* bytes = observed.host.s6_addr;
            bool mapped = bytes[10] == 255 && bytes[11] == 255;
            for (int i = 0; i < 10; ++i) mapped &= bytes[i] == 0;
            if (mapped) Bsnprintf(numeric, sizeof(numeric), "%u.%u.%u.%u", bytes[12], bytes[13], bytes[14], bytes[15]);
            uri::Endpoint endpoint;
            endpoint.host = numeric;
            endpoint.port = observed.port;
            discovered = uri::format(endpoint);
        }
    }
    if (discoveredUri != discovered)
    {
        discoveredUri = discovered;
        if (!discovered.empty()) OSD_Printf("NET STUN discovered %s (inbound reachability is not verified)\n", discovered.c_str());
    }
    std::string status = stunStatus(), error = stunError();
    if (status != lastStunStatus)
    {
        OSD_Printf("NET STUN %s\n", status.c_str());
        lastStunStatus = status;
    }
    if (error != lastStunError)
    {
        if (!error.empty()) OSD_Printf("NET STUN: %s\n", error.c_str());
        lastStunError = error;
    }
}
#else
std::string stunStatus() { return "networking disabled"; }
std::string stunError() { return {}; }
void startDiscovery() {}
void updateDiscovery() {}
#endif

unsigned playerCount()
{
    unsigned count = 0;
    for (int player = 0; player < MAXPLAYERS; ++player)
        if (rnet::connected(player)) ++count;
    return count;
}
std::vector<uint8_t> advertisement()
{
    std::vector<uint8_t> result{1};
    for (unsigned n = 0; n < 8; ++n) result.push_back(uint8_t(session >> (n * 8)));
    result.push_back(uint8_t(playerCount()));
    result.push_back(uint8_t(rnet::maxPlayers()));
    std::string endpoint = joinUri();
    result.insert(result.end(), endpoint.begin(), endpoint.end());
    return result;
}
void publish()
{
    if (!rnet::server() || rnet::playback()) return;
    auto bytes = advertisement();
    if (bytes == lastAdvertisement) return;
    lastAdvertisement = bytes;
    rnet::broadcast(Advertise, bytes);
}
std::string displayText(const std::string& value, size_t limit)
{
    std::string result;
    for (unsigned char ch : value)
    {
        if (result.size() >= limit) break;
        result += ch >= 32 && ch < 127 ? char(ch) : '?';
    }
    return result;
}
void registerDiscord()
{
    registeredApplication = discordApplication;
    registrationError.clear();
    if (uri::registerDiscordHandler(registeredApplication, registrationError))
        OSD_Printf("DISCORD registered launch application=%s\n", registeredApplication.c_str());
    else OSD_Printf("DISCORD launch registration failed: %s\n", registrationError.c_str());
}
discordrpc::Activity activity()
{
    discordrpc::Activity result;
    result.startedAt = startedAt;
    if (rnet::playback() || ud.recstat == 2)
    {
        result.details = "Watching a demo";
        return result;
    }
    bool game = rnet::inGame();
    if (!rnet::active())
    {
        bool single = g_player[myconnectindex].ps && (g_player[myconnectindex].ps->gm & MODE_GAME);
        result.details = single ? "Single player" : "In the main menu";
        result.state = "Offline";
        return result;
    }
    if (!game)
    {
        result.details = rnet::server() ? "Hosting a game" : "Connecting to a server";
        return result;
    }
    int flags = (ud.coop >= 0 && ud.coop < MAXGAMETYPES) ? g_gametypeFlags[ud.coop] : 0;
    std::string mode = flags & GAMETYPE_COOP ? "Cooperative" : flags & GAMETYPE_TDM ? "Team Dukematch" : "Dukematch";
    result.details = mode + " | " + displayText(rnet::mapName(), 90);
    std::string link = joinUri(), error;
    uri::Endpoint endpoint;
    if (link.empty() || !parseJoin(link, endpoint, error))
    {
        result.state = rnet::server() ? "Hosting a game" : "Multiplayer";
        return result;
    }
    result.state = uri::address(endpoint);
    unsigned count = rnet::server() ? playerCount() : remotePlayers;
    unsigned capacity = rnet::server() ? unsigned(rnet::maxPlayers()) : remoteCapacity;
    uint64_t id = rnet::server() ? session : remoteSession;
    // Until the server advertises authenticated capacity/session data, show
    // presence without a join button. Never invite into a full or stale game.
    if (!id || !count || capacity < count) return result;
    result.partyId = "rednukem:" + std::to_string(id);
    result.partySize = int(count);
    result.partyMax = int(capacity);
    if (count < capacity) result.joinSecret = uri::format(endpoint);
    return result;
}
void updateDiscord()
{
    std::string wanted = !rnet::dedicated() && discordEnabled && discordrpc::validApplicationId(discordApplication)
        ? discordApplication : "";
    if (wanted != runningApplication)
    {
        if (discord) discord->stop();
        discord.reset();
        discordReady = false;
        pendingJoin.clear();
        runningApplication = wanted;
        if (!wanted.empty())
        {
            if (registeredApplication != wanted) registerDiscord();
            discord.reset(new discordrpc::Client);
            discord->start(wanted);
        }
    }
    if (!discord) return;
    discordrpc::Event event;
    while (discord->poll(event))
        switch (event.kind)
        {
        case discordrpc::Event::Ready:
            discordReady = true;
            OSD_Printf("DISCORD connected application=%s\n", runningApplication.c_str());
            break;
        case discordrpc::Event::Disconnected:
            discordReady = false;
            break;
        case discordrpc::Event::Error:
            OSD_Printf("DISCORD %s\n", event.text.c_str());
            break;
        case discordrpc::Event::Join:
        {
            uri::Endpoint endpoint;
            std::string error;
            if (parseJoin(event.text, endpoint, error)) pendingJoin = uri::address(endpoint);
            else OSD_Printf("DISCORD rejected invitation: %s\n", error.c_str());
            break;
        }
        }
    discord->setActivity(activity());
}
int stunCommand(osdcmdptr_t args)
{
    if (args->numparms) return OSDCMD_SHOWHELP;
    OSD_Printf("NET STUN status=%s server=%s\n", stunStatus().c_str(), stunServer);
    if (!stunError().empty()) OSD_Printf("NET STUN %s\n", stunError().c_str());
    if (!joinUri().empty()) OSD_Printf("NET join link: %s\n", joinUri().c_str());
    return OSDCMD_OK;
}
int discordCommand(osdcmdptr_t args)
{
    if (args->numparms) return OSDCMD_SHOWHELP;
    OSD_Printf("DISCORD status=%s application=%s\n", rnet::dedicated() ? "unavailable on dedicated server"
        : !discordEnabled ? "disabled" : !*discordApplication ? "unconfigured" : discordReady ? "connected" : "waiting for Discord desktop",
        *discordApplication ? discordApplication : "unset");
    if (!registrationError.empty()) OSD_Printf("DISCORD launch registration: %s\n", registrationError.c_str());
    if (rnet::server() && joinUri().empty())
        OSD_Printf("Set discord_server_address rednukem://public-host:port to advertise your server.\n");
    return OSDCMD_OK;
}
int registerDiscordCommand(osdcmdptr_t args)
{
    if (args->numparms) return OSDCMD_SHOWHELP;
    if (!discordrpc::validApplicationId(discordApplication))
        OSD_Printf("Set discord_appid to your Discord application's numeric ID first.\n");
    else if (rnet::dedicated()) OSD_Printf("Discord integration is unavailable on dedicated servers.\n");
    else registerDiscord();
    return OSDCMD_OK;
}
bool commandError(const std::string& error, int& exitCode)
{
    std::fprintf(stderr, "Rednukem connection link: %s\n", error.c_str());
#ifdef _WIN32
    MessageBoxA(nullptr, error.c_str(), "Rednukem connection link", MB_OK | MB_ICONERROR);
#endif
    exitCode = 1;
    return true;
}
} // namespace

bool earlyArguments(int& argc, const char* const* argv, int& exitCode)
{
    bool registration = false, connection = false, discordLaunch = false;
    for (int i = 1; i < argc; ++i)
    {
        // Password/config values are not independent switches or external links.
        if ((plainSwitch(argv[i], "password") || plainSwitch(argv[i], "stunserver") ||
             plainSwitch(argv[i], "discord-server-address") || plainSwitch(argv[i], "discord-appid") ||
             plainSwitch(argv[i], "client") || plainSwitch(argv[i], "connect")) && i + 1 < argc)
        {
            ++i;
            continue;
        }
        if (plainSwitch(argv[i], "zerotier"))
        {
            if (i + 1 < argc && argv[i + 1][0] != '-') ++i;
            continue;
        }
        registration |= isSwitch(argv[i], "register-uri");
        connection |= isSwitch(argv[i], "connect-uri") || !Bstrncasecmp(argv[i], "rednukem:", 9);
        discordLaunch |= isSwitch(argv[i], "discord-launch");
    }
    if (!registration && !connection && !discordLaunch) return false;
    std::string error;
    if (registration)
    {
        if (argc != 2 || !plainSwitch(argv[1], "register-uri"))
            return commandError("Use -register-uri by itself to register connection links.", exitCode);
        if (!uri::registerHandler(error)) return commandError(error, exitCode);
        std::fprintf(stdout, "Registered rednukem:// connection links for this user.\n");
#ifdef _WIN32
        MessageBoxA(nullptr, "Registered rednukem:// connection links for this user.", "Rednukem connection links", MB_OK);
#endif
        exitCode = 0;
        return true;
    }
    if (discordLaunch)
    {
        if ((argc != 3 && argc != 4) || !plainSwitch(argv[1], "discord-launch") || !discordrpc::validApplicationId(argv[2]))
            return commandError("Discord launches require -discord-launch and a valid application ID only.", exitCode);
        if (argc == 4)
        {
            std::string scheme = std::string("discord-") + argv[2] + ":";
            if (Bstrlen(argv[3]) > 1024 || Bstrncasecmp(argv[3], scheme.c_str(), scheme.size()))
                return commandError("Discord launch URI does not match the application.", exitCode);
        }
        launchApplication = argv[2];
    }
    else
    {
        const char* value = argc == 2 && !Bstrncasecmp(argv[1], "rednukem:", 9) ? argv[1]
            : argc == 3 && plainSwitch(argv[1], "connect-uri") ? argv[2] : nullptr;
        if (!value) return commandError("Open exactly one rednukem://host[:port] link without additional arguments.", exitCode);
        uri::Endpoint endpoint;
        if (!uri::parse(value, endpoint, error)) return commandError(error, exitCode);
        initialJoin = uri::address(endpoint);
    }
    if (!uri::prepareLaunch(error)) return commandError(error, exitCode);
    launchedExternally = true;
    g_noSetup = 1;
    g_commandSetup = 0;
    argc = 1;
    return false;
}
bool parseArgument(int argc, const char* const* argv, int& index)
{
    const char* name = nullptr;
    char* destination = nullptr;
    size_t capacity = 0;
    if (plainSwitch(argv[index], "stunserver")) { name = "net_stunserver"; destination = stunServer; capacity = sizeof(stunServer); }
    else if (plainSwitch(argv[index], "discord-appid")) { name = "discord_appid"; destination = discordApplication; capacity = sizeof(discordApplication); }
    else if (plainSwitch(argv[index], "discord-server-address")) { name = "discord_server_address"; destination = discordAddress; capacity = sizeof(discordAddress); }
    else return false;
    std::string error;
    if (index + 1 >= argc) { rnet::fail(std::string(argv[index++]) + " requires a value."); return true; }
    const char* value = argv[index + 1];
    index += 2;
    if (Bstrlen(value) >= capacity || !validateSetting(name, value, error))
    {
        rnet::fail(error.empty() ? "Presence option is too long." : error);
        return true;
    }
    Bstrncpyz(destination, value, capacity);
    if (destination == stunServer) { stunOption = value; hasStunOption = true; }
    else if (destination == discordApplication) { applicationOption = value; hasApplicationOption = true; }
    else { addressOption = value; hasAddressOption = true; }
    return true;
}
bool externalLaunch() { return launchedExternally; }
void registerCommands()
{
    static osdcvardata_t settings[] = {
        { "net_stunserver", "STUN discovery: auto, off, or host[:port]", stunServer, CVAR_STRING | CVAR_FUNCPTR, 0, sizeof(stunServer) },
        { "discord_enabled", "enable Discord Rich Presence and invitations", &discordEnabled, CVAR_BOOL, 0, 1 },
        { "discord_appid", "Discord application ID (empty disables integration)", discordApplication, CVAR_STRING | CVAR_FUNCPTR, 0, sizeof(discordApplication) },
        { "discord_server_address", "public rednukem://host:port override for invitations", discordAddress, CVAR_STRING | CVAR_FUNCPTR, 0, sizeof(discordAddress) },
    };
    for (auto& setting : settings) OSD_RegisterCvar(&setting, settingCommand);
    OSD_RegisterFunction("net_stunstatus", "net_stunstatus: show public endpoint discovery", stunCommand);
    OSD_RegisterFunction("net_stun_status", "net_stun_status: show public endpoint discovery", stunCommand);
    OSD_RegisterFunction("discord_status", "discord_status: show Discord connection status", discordCommand);
    OSD_RegisterFunction("discord_register", "discord_register: register this executable with Discord", registerDiscordCommand);
}
void startup()
{
    if (initialized) return;
    initialized = true;
    startedAt = int64_t(std::time(nullptr));
    // Options are parsed before the saved cvars/autoexec. Explicit command-line
    // choices keep precedence over those files, including a discovery opt-out.
    if (hasStunOption) Bstrncpyz(stunServer, stunOption.c_str(), sizeof(stunServer));
    if (hasApplicationOption) Bstrncpyz(discordApplication, applicationOption.c_str(), sizeof(discordApplication));
    if (hasAddressOption) Bstrncpyz(discordAddress, addressOption.c_str(), sizeof(discordAddress));
    if (!launchApplication.empty())
    {
        Bstrncpyz(discordApplication, launchApplication.c_str(), sizeof(discordApplication));
        discordEnabled = 1;
    }
    if (!initialJoin.empty())
    {
        rnet::connect(initialJoin);
        initialJoin.clear();
    }
    pump();
}
void detachHost()
{
#ifndef NETCODE_DISABLE
    if (attached && attached->intercept == intercept) enet_host_set_intercept(attached, previousIntercept);
    stun.stop();
    lookup.reset();
    lookupNames.clear();
    retryLookup = false;
    setupError.clear();
#endif
    attached = nullptr;
    previousIntercept = nullptr;
    discoveredUri.clear();
    remoteUri.clear();
    remoteSession = 0;
    remotePlayers = remoteCapacity = 0;
    lastAdvertisement.clear();
    lastStunError.clear();
    lastStunStatus.clear();
    configuredStun.clear();
}
void pump()
{
    if (!initialized) return;
    if (rnet::host() != attached || (rnet::server() && configuredStun != stunServer))
    {
        bool changedHost = rnet::host() != attached;
        // Core calls detachHost before destruction; a configuration change can
        // safely detach and reinstall the intercept on the still-live socket.
        detachHost();
        attached = rnet::host();
        configuredStun = stunServer;
        if (changedHost && attached && rnet::server())
            session = newSession();
        startDiscovery();
    }
    if (attached && rnet::server() && !rnet::zeroTier()) updateDiscovery();
    publish();
    updateDiscord();
}
void applyPendingJoin()
{
    if (!initialized || !discordEnabled || rnet::dedicated() || pendingJoin.empty()) return;
    const std::string address = pendingJoin;
    pendingJoin.clear();
    rnet::connect(address);
}
void shutdown()
{
    detachHost();
    initialized = discordReady = false;
    pendingJoin.clear();
    if (discord) discord->stop();
    discord.reset();
    runningApplication.clear();
}
void mapChanged() { lastAdvertisement.clear(); }
void connected(int player)
{
    if (rnet::server() && player >= 0) rnet::send(player, Advertise, advertisement());
    lastAdvertisement.clear();
}
bool handle(int player, uint8_t type, const uint8_t* data, size_t size)
{
    if (type != Advertise) return false;
    if (!rnet::client() || player != -1 || rnet::playback() || size < 11 || size > 310 || data[0] != 1) return true;
    if (data[9] > data[10] || !data[10] || data[10] > MAXPLAYERS) return true;
    uint64_t id = 0;
    for (unsigned n = 0; n < 8; ++n) id |= uint64_t(data[n + 1]) << (n * 8);
    if (!id) return true;
    std::string endpoint(reinterpret_cast<const char*>(data + 11), size - 11);
    if (!endpoint.empty())
    {
        uri::Endpoint parsed;
        std::string error;
        if (!uri::parse(endpoint, parsed, error)) return true;
        endpoint = uri::format(parsed);
    }
    remoteUri = endpoint;
    remoteSession = id;
    remotePlayers = data[9];
    remoteCapacity = data[10];
    return true;
}
std::string joinUri()
{
    if (!rnet::active() || rnet::playback() || rnet::zeroTier()) return {};
    if (rnet::server())
    {
        if (*discordAddress)
        {
            uri::Endpoint endpoint;
            std::string error;
            return parseJoin(discordAddress, endpoint, error) ? uri::format(endpoint) : "";
        }
        return discoveredUri;
    }
    if (!remoteUri.empty()) return remoteUri;
    uri::Endpoint endpoint;
    std::string error;
    return uri::parse("rednukem://" + rnet::endpoint(), endpoint, error) ? uri::format(endpoint) : "";
}
} } // namespace rnet::presence
