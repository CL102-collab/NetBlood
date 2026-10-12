//-------------------------------------------------------------------------
/*
Copyright (C) 2010-2019 EDuke32 developers and contributors
Copyright (C) 2019 Nuke.YKT

This file is part of NBlood.

NBlood is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License version 2
as published by the Free Software Foundation.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/
//-------------------------------------------------------------------------
#include "build.h"
#include "mmulti.h"
#include "pragmas.h"
#ifndef NETCODE_DISABLE
#include "enet.h"
#endif
#include "compat.h"
#include "config.h"
#include "controls.h"
#include "demo.h"
#include "globals.h"
#include "menu.h"
#include "network.h"
#include "netserver.h"
#include "player.h"
#include "seq.h"
#include "sound.h"
#include "view.h"

bool         gStartNewGame = 0;
PACKETMODE   gPacketMode   = PACKETMODE_1;
ClockTicks   gNetFifoClock = 0;
int          gNetFifoTail  = 0;
int          gNetFifoHead[8];
int          gPredictTail       = 0;
int          gNetFifoMasterTail = 0;
GINPUT       gFifoInput[256][8];
int          myMinLag[8];
int          otherMinLag = 0;
int          myMaxLag    = 0;
unsigned int gChecksum[4];
unsigned int gCheckFifo[256][8][4];
int          gCheckHead[8];
int          gSendCheckTail     = 0;
int          gCheckTail         = 0;
int          gInitialNetPlayers = 0;
int          gBufferJitter      = 1;
int          gPlayerReady[8];
bool         bNoResend  = true;
bool         gRobust    = false;
bool         bOutOfSync = false;
bool         ready2send = false;

NETWORKMODE gNetMode = NETWORK_NONE;
char        gNetAddress[32];
// PORT-TODO: Use different port?
int gNetPort = kNetDefaultPort;


PKT_STARTGAME gPacketStartGame;

#include "blood.h"
#include "inifile.h"
#include "levels.h"
#include "misc.h"
#include "netcontent.h"
#include "netcontentreport.h"
#include "netdemo.h"
#include "netevents.h"
#include "netlag.h"
#include "netmap.h"
#include "netpredict.h"
#include "netprotocol.h"
#include "netsnapshot.h"
#include "timer.h"

#include <algorithm>
#include <deque>
#include <memory>
#include <new>
#include <string>
#include <vector>

extern void MenuSetupEpisodeInfo(void);

int gNetPacketCopies = 3;
int gNetSnapshotRate = 20;
static_assert(kWeaponMax == netwire::WeaponCount, "Update the wire weapon range when adding weapons");
static_assert(netmap::MaxBytes == netwire::MaxMapBytes, "Map storage and wire limits must agree");
namespace
{
using Bytes = std::vector<uint8_t>;
struct CachedSnapshot
{
    uint32_t id = 0;
    Bytes    state;
};
struct MapWindow
{
    unsigned pendingPackets = 0;
};
struct ClientSlot
{
#ifndef NETCODE_DISABLE
    ENetPeer *peer = nullptr;
#endif
    uint32_t              generation = 0, received[netwire::TypeCount] = {}, snapshotAck = 0, fullSnapshotTime = 0;
    uint32_t              mapDownloadNext = 0;
    uint32_t              mapWindowEnd = 0;
    bool                  mapDownloadStarted = false;
    bool                  mapRequestPending = false;
    bool                  contentOffered = false;
    uint32_t              contentFingerprint = 0;
    std::shared_ptr<MapWindow> mapWindow;
    bool                  admitted = false, active = false, ready = false, pendingReady = false, pendingDrop = false, pendingLoops = false;
    netwire::CommandQueue commands;
    GINPUT                lastInput     = {};
    uint32_t              lastInputTime = 0;
};
ClientSlot                   slots[8];
uint32_t                     session = 0, mapGeneration = 0, sendSequence = 0, serverTick = 0, snapshotId = 0, commandSequence = 0;
uint32_t                     clientSlotGeneration = 0, clientReceived[netwire::TypeCount] = {}, clientSnapshot = 0, clientMapCRC = 0, lastSnapshotTime = 0;
uint32_t                     nextTickTime = 0, lastSnapshotTick = 0, playbackStart = 0;
bool                         initialized = false, clientLoaded = false, clientSnapshotReady = false, servicing = false, demoPlayback = false, serverInTick = false;
size_t                       pendingClientBytes = 0;
unsigned                     snapshotBudget     = 0;
uint32_t                     contentCRC         = 0;
bool                         clientContentChecked = false;
NetDemoInfo                  replayInfo;
bool                         replayFirstWelcome  = false;
bool                         playbackEndReported = false;
int                          recordSlot          = -1;
bool                         recordNeedsWelcome = false, recordResetClock = false;
uint64_t                     sentMessages = 0, receivedMessages = 0, malformedMessages = 0, duplicateMessages = 0;
std::deque<CachedSnapshot>   snapshotCache;
std::deque<netwire::Command> commandHistory;
std::deque<Bytes>            pendingClientPackets;
Bytes                        lastWelcome, lastRecordedSnapshot;
Bytes                        serverMapBytes, clientMapBytes;
GAMEOPTIONS                  clientMapOptions = {};
netwire::ServerRules          clientServerRules;
uint32_t                     clientMapSize = 0, clientMapProgressTime = 0;
bool                         clientDownloadingMap = false, mapDownloadDisconnected = false;
std::string                  mapDownloadError;
struct ClientLevelOverride
{
    int       episode, level;
    LEVELINFO original;
};
std::vector<ClientLevelOverride> clientLevelOverrides;
bool                             clientRegistrySaved = false;
int                              clientEpisodeCount = 0, clientLevelCounts[kMaxEpisodes] = {};
char                             clientEpisodeTitles[kMaxEpisodes][32] = {};
void                             restoreClientLevels()
{
    if (!clientRegistrySaved)
        return;
    for (auto &entry : clientLevelOverrides) gEpisodeInfo[entry.episode].levelsInfo[entry.level] = entry.original;
    for (int n = 0; n < kMaxEpisodes; n++)
    {
        gEpisodeInfo[n].nLevels = clientLevelCounts[n];
        Bstrncpyz(gEpisodeInfo[n].title, clientEpisodeTitles[n], sizeof(gEpisodeInfo[n].title));
    }
    gEpisodeCount = clientEpisodeCount;
    clientLevelOverrides.clear();
    clientRegistrySaved = false;
    MenuSetupEpisodeInfo();
}

#ifndef NETCODE_DISABLE
ENetHost     *host           = nullptr;
ENetPeer     *serverPeer     = nullptr;
constexpr int ControlChannel = 0, CommandChannel = 1, SnapshotChannel = 2;
#endif
uint8_t playerMask()
{
    uint8_t mask = 0;
    for (int p = 0; p < 8; p++)
        if (slots[p].active)
            mask |= uint8_t(1 << p);
    return mask;
}
void rebuildRoster()
{
    connecthead  = -1;
    numplayers   = 0;
    int previous = -1;
    for (int p = 0; p < 8; p++)
    {
        connectpoint2[p] = -1;
        if (!slots[p].active)
            continue;
        if (previous < 0)
            connecthead = p;
        else
            connectpoint2[previous] = p;
        previous = p;
        numplayers++;
    }
    gNetPlayers = numplayers;
}
const Bytes *findSnapshot(uint32_t id)
{
    for (auto &c : snapshotCache)
        if (c.id == id)
            return &c.state;
    return nullptr;
}
void cacheSnapshot(uint32_t id, const Bytes &data)
{
    snapshotCache.push_back({ id, data });
    size_t bytes = 0;
    for (auto &c : snapshotCache) bytes += c.state.size();
    while (snapshotCache.size() > 64 || bytes > 64u * 1024u * 1024u)
    {
        bytes -= snapshotCache.front().state.size();
        snapshotCache.pop_front();
    }
}
GINPUT toInput(const netwire::Command &c)
{
    GINPUT i           = {};
    i.syncFlags.byte   = c.sync;
    i.forward          = c.forward;
    i.q16turn          = netwire::normalizeTurn(c.turn);
    i.strafe           = c.strafe;
    i.buttonFlags.byte = c.buttons;
    i.keyFlags.word    = c.keys;
    i.useFlags.byte    = c.use;
    i.newWeapon        = c.weapon;
    i.q16mlook         = c.mlook;
    return i;
}
netwire::Command fromInput(const GINPUT &i, uint32_t sequence)
{
    netwire::Command c;
    c.sequence = sequence;
    c.sync     = i.syncFlags.byte & 63;
    c.forward  = ClipRange(i.forward, -2048, 2048);
    c.turn     = netwire::normalizeTurn(i.q16turn);
    c.strafe   = ClipRange(i.strafe, -2048, 2048);
    c.buttons  = i.buttonFlags.byte & 63;
    c.keys     = i.keyFlags.word & ~0x7000;
    c.use      = i.useFlags.byte & 15;
    c.weapon   = unsigned(i.newWeapon) < netwire::WeaponCount ? i.newWeapon : 0;
    c.mlook    = ClipRange(i.q16mlook, -(127 << 14), 127 << 14);
    return c;
}
Bytes envelope(uint32_t type, const Bytes &body)
{
    netwire::Header h;
    h.type       = type;
    h.session    = session;
    h.generation = mapGeneration;
    h.sequence   = ++sendSequence;
    return netwire::encode(h, body);
}
#ifndef NETCODE_DISABLE
void ENET_CALLBACK releaseMapPacket(void *opaque)
{
    auto *packet = static_cast<ENetPacket *>(opaque);
    auto *window = static_cast<std::shared_ptr<MapWindow> *>(packet->userData);
    --(*window)->pendingPackets;
    delete window;
}
bool sendTo(ENetPeer *peer, uint32_t type, const Bytes &body, bool reliable = true, std::shared_ptr<MapWindow> window = {})
{
    if (!peer)
        return false;
    Bytes wire = envelope(type, body);
    if (wire.empty())
        return false;
    const int   channel = type == netwire::Commands ? CommandChannel : type == netwire::Snapshot && !reliable ? SnapshotChannel : ControlChannel;
    enet_uint32 flags   = reliable ? ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT;
    ENetPacket *packet  = enet_packet_create(wire.data(), wire.size(), flags);
    if (!packet)
        return false;
    if (window)
    {
        auto *reference = new (std::nothrow) std::shared_ptr<MapWindow>(window);
        if (!reference)
        {
            enet_packet_destroy(packet);
            return false;
        }
        ++window->pendingPackets;
        packet->userData = reference;
        packet->freeCallback = releaseMapPacket;
    }
    if (enet_peer_send(peer, channel, packet) < 0)
    {
        enet_packet_destroy(packet);
        return false;
    }
    sentMessages++;
    return true;
}
void sendReject(ENetPeer *peer, const char *why)
{
    netwire::Writer w;
    w.text(why, 160);
    sendTo(peer, netwire::Reject, w.data);
    enet_peer_disconnect_later(peer, 1);
}
#endif
int serverAutoAim()
{
    return gDedicated ? netServerSettings().autoAim : ClipRange(gAutoAim, 0, 3);
}
int serverPlayerSkill()
{
    return gGameOptions.nGameType == kGameTypeCoop ? int(gGameOptions.nDifficulty) : 2;
}
void enforceServerProfiles()
{
    for (auto& profile : gProfile)
    {
        profile.nAutoAim = serverAutoAim();
        profile.skill = serverPlayerSkill();
    }
}
netwire::ServerRules serverRules()
{
    netwire::ServerRules rules;
    rules.maxPlayers = gInitialNetPlayers;
    rules.autoAim = serverAutoAim();
    if (gDedicated)
    {
        auto const& settings = netServerSettings();
        rules.timeLimitMinutes = settings.timeLimitMinutes;
        rules.scoreLimit = settings.scoreLimit;
        rules.rotationIndex = uint32_t(settings.rotationIndex);
        rules.rotationCount = uint32_t(settings.rotationCount);
    }
    return rules;
}
void configureStart(const GAMEOPTIONS &options)
{
    gPacketStartGame                 = {};
    gPacketStartGame.version         = netwire::Version;
    gPacketStartGame.gameType        = options.nGameType;
    gPacketStartGame.difficulty      = options.nDifficulty;
    gPacketStartGame.monsterSettings = options.nMonsterSettings;
    gPacketStartGame.weaponSettings  = options.nWeaponSettings;
    gPacketStartGame.itemSettings    = options.nItemSettings;
    gPacketStartGame.respawnSettings = options.nRespawnSettings;
    gPacketStartGame.episodeId       = options.nEpisode;
    gPacketStartGame.levelId         = options.nLevel;
    gPacketStartGame.weaponsV10x     = options.weaponsV10x;
    gPacketStartGame.bFriendlyFire   = options.bFriendlyFire;
    gPacketStartGame.bPlayerKeys     = options.bPlayerKeys;
    gPacketStartGame.userMap         = options.szUserMap[0] != 0;
    Bstrncpyz(gPacketStartGame.userMapName, options.szUserMap, sizeof(gPacketStartGame.userMapName));
}
// A server's custom-map registry can differ from the client's registry: the
// old levelAddUserMap helper appends a local slot and does not set szUserMap.
// Bootstrap therefore identifies the map by its authoritative filename, then
// binds its local metadata to the server's explicit episode/level indices.
// StartLevel can use that slot without appending/reindexing it a second time.
bool prepareClientLevel(const GAMEOPTIONS &options)
{
    if (!options.zLevelName[0] || options.nEpisode < 0 || options.nEpisode >= kMaxEpisodes || options.nLevel < 0 || options.nLevel >= kMaxLevels)
        return false;
    if (!clientRegistrySaved)
    {
        clientRegistrySaved = true;
        clientEpisodeCount  = gEpisodeCount;
        for (int n = 0; n < kMaxEpisodes; n++)
        {
            clientLevelCounts[n] = gEpisodeInfo[n].nLevels;
            Bstrncpyz(clientEpisodeTitles[n], gEpisodeInfo[n].title, sizeof(clientEpisodeTitles[n]));
        }
    }
    EPISODEINFO &episode = gEpisodeInfo[options.nEpisode];
    LEVELINFO   &level   = episode.levelsInfo[options.nLevel];
    const bool cachedMap = netMapResolve(options.zLevelName) != options.zLevelName;
    if (Bstrcasecmp(level.Filename, options.zLevelName) || cachedMap)
    {
        bool saved = false;
        for (auto &entry : clientLevelOverrides)
            if (entry.episode == options.nEpisode && entry.level == options.nLevel)
            {
                saved = true;
                break;
            }
        if (!saved)
            clientLevelOverrides.push_back({ options.nEpisode, options.nLevel, level });
        // A maximal map name may need a new four-byte .DEF extension.
        char definition[BMAX_PATH + 8];
        Bstrncpyz(definition, options.zLevelName, sizeof(definition));
        ChangeExtension(definition, ".DEF");
        // A cached map's canonical name belongs to the server. Do not use it
        // to probe an unrelated local .DEF (including absolute server paths).
        IniFile   metadata(cachedMap ? nullptr : definition);
        LEVELINFO local = {};
        Bstrncpyz(local.Filename, options.zLevelName, sizeof(local.Filename));
        levelLoadMapInfo(&metadata, &local, nullptr);
        level = local;
    }
    if (!episode.nLevels && !episode.title[0])
        Bsnprintf(episode.title, sizeof(episode.title), "Episode %d", options.nEpisode);
    episode.nLevels = std::max(episode.nLevels, options.nLevel + 1);
    gEpisodeCount   = std::max(gEpisodeCount, options.nEpisode + 1);
    MenuSetupEpisodeInfo();
    return true;
}
void failMapDownload(const std::string &reason)
{
    LOG_F(ERROR, "NET map download failed: %s", reason.c_str());
    netDisconnect();
    gStartNewGame = false;
    mapDownloadError = reason;
    viewSetErrorMessage("Map download failed; see console for details");
}
bool startClientMap()
{
    if (!prepareClientLevel(clientMapOptions))
    {
        failMapDownload("Invalid map filename or level indices");
        return false;
    }
    gGameOptions = clientMapOptions;
    configureStart(clientMapOptions);
    gPacketStartGame.userMap = 0;
    gStartNewGame = true;
    clientDownloadingMap = false;
    Bytes().swap(clientMapBytes);
    return true;
}
#ifndef NETCODE_DISABLE
void requestMapWindow()
{
    netwire::Writer w;
    netwire::MapRequestPayload request = {clientSlotGeneration, clientMapCRC, uint32_t(clientMapBytes.size())};
    if (netwire::writeMapRequest(w, request))
        sendTo(serverPeer, netwire::RequestMap, w.data);
}
#endif
void removePlayer(int p)
{
    if (p < 0 || p >= 8)
        return;
    if (slots[p].active && gGameStarted && gPlayer[p].pSprite)
    {
        seqKill(3, gPlayer[p].pSprite->extra);
        actPostSprite(gPlayer[p].nSprite, kStatFree);
    }
    uint32_t generation = slots[p].generation;
    slots[p]            = ClientSlot();
    slots[p].generation = generation;
    rebuildRoster();
    LOG_F(INFO, "NET disconnected slot=%d players=%d server_tick=%u", p, numplayers, serverTick);
}
#ifndef NETCODE_DISABLE
void sendWelcome(int p)
{
    enforceServerProfiles();
    Bytes       state, packed;
    std::string error;
    bool        delta = false;
    if (!gGameStarted || !ready2send || gStartNewGame || !mapGeneration || !netSnapshotCapture(state, playerMask(), error)
        || !netSnapshotPack(state, nullptr, packed, delta, error))
    {
        if (!error.empty())
            LOG_F(ERROR, "NET bootstrap: %s", error.c_str());
        return;
    }
    netwire::Writer w;
    w.u32(p);
    w.u32(slots[p].generation);
    w.u32(gGameOptions.uMapCRC);
    w.u32(contentCRC);
    w.u32(uint32_t(packed.size()));
    w.bytes(packed.data(), packed.size());
    netwire::writeServerRules(w, serverRules());
    sendTo(slots[p].peer, netwire::Welcome, w.data);
    slots[p].ready       = false;
    slots[p].pendingReady = false;
    slots[p].snapshotAck = 0;
    slots[p].mapDownloadNext = 0;
    slots[p].mapDownloadStarted = false;
    slots[p].mapRequestPending = false;
    slots[p].mapWindow.reset();
}
void pumpMapDownloads()
{
    if (!netIsServer() || !gGameStarted || gStartNewGame)
        return;
    for (auto &c : slots)
    {
        // A request acknowledges receipt at the application layer, but is not
        // proof of ENet acknowledgements. Keep at most one transport window
        // queued even if a peer guesses and pipelines later request offsets.
        if (!c.peer || !c.mapRequestPending || c.ready || c.pendingReady || (c.mapWindow && c.mapWindow->pendingPackets))
            continue;
        if (!c.mapWindow)
            c.mapWindow = std::make_shared<MapWindow>();
        while (c.mapDownloadNext < c.mapWindowEnd)
        {
            uint32_t length = std::min(netwire::MapChunkBytes, uint32_t(serverMapBytes.size()) - c.mapDownloadNext);
            netwire::MapDataPayload chunk = {gGameOptions.uMapCRC, uint32_t(serverMapBytes.size()), c.mapDownloadNext,
                                             length, serverMapBytes.data() + c.mapDownloadNext};
            netwire::Writer w;
            if (!netwire::writeMapData(w, chunk) || !sendTo(c.peer, netwire::MapData, w.data, true, c.mapWindow))
                break;
            c.mapDownloadNext += length;
        }
        c.mapRequestPending = c.mapDownloadNext < c.mapWindowEnd;
    }
}
#endif
Bytes snapshotBody(const Bytes &canonical, uint32_t id, uint32_t baseline, const Bytes *base)
{
    Bytes       packed;
    std::string error;
    bool        delta = false;
    if (!netSnapshotPack(canonical, base, packed, delta, error))
    {
        LOG_F(ERROR, "NET snapshot encode: %s", error.c_str());
        return {};
    }
    netwire::Writer w;
    w.u32(serverTick);
    w.u32(id);
    w.u32(delta ? baseline : 0);
    for (int p = 0; p < 8; p++) w.u32(slots[p].commands.acknowledged);
    w.u8(playerMask());
    w.u32(uint32_t(packed.size()));
    w.bytes(packed.data(), packed.size());
    return w.data;
}
bool recordLoopBaseline()
{
    std::vector<Bytes> events;
    netEventsBaseline(events);
    for (auto &event : events)
    {
        netwire::Writer w;
        w.u32(serverTick);
        w.bytes(event.data(), event.size());
        netwire::Header h;
        h.type       = netwire::Event;
        h.session    = session;
        h.generation = mapGeneration;
        // Sequence zero is a replay-only loop baseline, not a received event.
        if (!netDemoRecordPacket(serverTick, netwire::encode(h, w.data)))
            return false;
    }
    return true;
}
bool recordServerSnapshot(const Bytes &state, uint32_t id)
{
    if (!netDemoIsRecording())
        return false;
    if (recordSlot < 0 || !slots[recordSlot].active)
    {
        recordSlot         = connecthead;
        recordNeedsWelcome = true;
    }
    if (recordSlot < 0)
        return true;  // An empty dedicated server keeps recording open.
    if (recordNeedsWelcome)
    {
        Bytes       packed;
        std::string error;
        bool        delta = false;
        if (!netSnapshotPack(state, nullptr, packed, delta, error))
            return false;
        netwire::Writer w;
        w.u32(recordSlot);
        w.u32(slots[recordSlot].generation);
        w.u32(gGameOptions.uMapCRC);
        w.u32(contentCRC);
        w.u32(packed.size());
        w.bytes(packed.data(), packed.size());
        netwire::writeServerRules(w, serverRules());
        if (!netDemoRecordPacket(serverTick, envelope(netwire::Welcome, w.data), recordResetClock))
            return false;
        recordResetClock = false;
    }
    Bytes body = snapshotBody(state, id, 0, nullptr);
    if (body.empty() || !netDemoRecordPacket(serverTick, envelope(netwire::Snapshot, body)))
        return false;
    if (recordNeedsWelcome)
    {
        recordNeedsWelcome = false;
        return recordLoopBaseline();
    }
    return true;
}
void failContentCheck(std::vector<netassets::Issue> issues, const std::string &reason)
{
    netDisconnect();
    gStartNewGame = false;
    netContentReportShow(std::move(issues), reason);
    viewSetErrorMessage("Game content mismatch. Check the listed files and restart with the server's mod.");
}
bool acceptClientPacket(const uint8_t *data, size_t size, bool replay)
{
    netwire::Header h;
    netwire::Reader r(nullptr, 0);
    if (!netwire::decode(data, size, h, r))
    {
        malformedMessages++;
        return false;
    }
    if (h.type == netwire::ContentManifest)
    {
        // Content is checked before a map, snapshot, or download can start.
        // This control exchange is deliberately excluded from packet demos.
        if (replay || clientContentChecked || (session && session != h.session))
            return false;
        uint32_t fingerprint = r.u32();
        std::vector<netassets::Asset> required;
        std::string error;
        if (!h.session || !netassets::decodeManifest(r, required, error))
        {
            malformedMessages++;
            failContentCheck({}, "Cannot check the server's mod: invalid asset manifest. Use compatible client and server versions.");
            return false;
        }
        auto issues = netContentDifferences(required);
        LOG_F(INFO, "NET content check local=%08x remote=%08x assets=%u issues=%u", contentCRC, fingerprint,
              unsigned(required.size()), unsigned(issues.size()));
        if (!issues.empty())
        {
            failContentCheck(std::move(issues), "Required mod assets are missing, different, or not loaded. Install and select the server's mod, then restart.");
            return false;
        }
        if (fingerprint != contentCRC)
        {
            LOG_F(ERROR, "NET game content mismatch local=%08x remote=%08x", contentCRC, fingerprint);
            failContentCheck({}, "The listed server files match, but effective game definitions differ. Check extra overrides, mod selection and load order, then restart.");
            return false;
        }
#ifndef NETCODE_DISABLE
        session = h.session;
        netwire::Writer w;
        w.u32(fingerprint);
        if (!sendTo(serverPeer, netwire::ContentReady, w.data))
        {
            failContentCheck({}, "The server disconnected during the mod check. Reconnect to try again.");
            return false;
        }
        clientContentChecked = true;
        LOG_F(INFO, "NET content verified assets=%u", unsigned(required.size()));
        return true;
#else
        return false;
#endif
    }
    if (h.type == netwire::Welcome)
    {
        if (!replay && session && h.session != session)
            return false;
        if (!replay && !clientContentChecked)
        {
            failContentCheck({}, "The server did not provide an asset manifest. Update the server to a compatible version.");
            return false;
        }
        if (mapGeneration == h.generation && clientReceived[h.type] && !netwire::newer(h.sequence, clientReceived[h.type]))
        {
            duplicateMessages++;
            return true;
        }
        int      slot           = int(r.u32());
        uint32_t slotGeneration = r.u32(), crc = r.u32(), fingerprint = r.u32(), length = r.u32();
        if (fingerprint != contentCRC)
        {
            LOG_F(ERROR, "NET game content mismatch local=%08x remote=%08x", contentCRC, fingerprint);
            return false;
        }
        if (!r.ok || slot < 0 || slot >= 8 || length > r.size - r.pos || r.size - r.pos - length != netwire::ServerRulesBytes)
        {
            malformedMessages++;
            return false;
        }
        Bytes       packed(length), canonical;
        std::string error;
        r.bytes(packed.data(), length);
        netwire::ServerRules rules;
        GAMEOPTIONS options = {};
        uint8_t     mask    = 0;
        if (!netwire::readServerRules(r, rules) || !r.end() || !netSnapshotUnpack(packed, nullptr, canonical, error)
            || !netSnapshotGetOptions(canonical, options, mask, error))
        {
            LOG_F(ERROR, "NET invalid bootstrap: %s", error.c_str());
            return false;
        }
        if (options.uMapCRC != crc || !options.zLevelName[0])
        {
            malformedMessages++;
            LOG_F(ERROR, "NET bootstrap has an invalid map descriptor");
            return false;
        }
        if (replay && replayFirstWelcome)
        {
            if (replayInfo.mapCrc != crc || replayInfo.localSlot != slot)
            {
                LOG_F(ERROR, "NET demo metadata does not match bootstrap");
                return false;
            }
            replayFirstWelcome = false;
        }
        session              = h.session;
        mapGeneration        = h.generation;
        clientSlotGeneration = slotGeneration;
        clientMapCRC         = crc;
        clientServerRules    = rules;
        std::fill(std::begin(clientReceived), std::end(clientReceived), 0);
        clientReceived[h.type] = h.sequence;
        myconnectindex         = slot;
        gMe = gView = &gPlayer[slot];
        gViewIndex  = slot;
        connecthead = slot;
        numplayers = gNetPlayers = 1;
        for (int p = 0; p < 8; p++) connectpoint2[p] = -1;
        clientLoaded        = false;
        clientSnapshotReady = false;
        clientSnapshot      = 0;
        commandSequence     = 0;
        commandHistory.clear();
        snapshotCache.clear();
        netPredictReset();
        netEventsReset();
        netMapClear();
        clientMapOptions = options;
        clientMapBytes.clear();
        clientMapSize = 0;
        clientDownloadingMap = mapDownloadDisconnected = false;
        mapDownloadError.clear();
        gStartNewGame = false;
        lastWelcome.assign(data, data + size);
        if (netDemoIsRecording() && !replay)
            netDemoRecordPacket(0, lastWelcome, true);
        serverTick = 0;
        LOG_F(INFO, "NET welcome slot=%d generation=%u map_generation=%u map_crc=%08x", slot, slotGeneration, mapGeneration, crc);
        if (netMapPrepare(options.zLevelName, crc, error))
            return startClientMap();
        if (replay)
        {
            failMapDownload("Map is unavailable for offline playback; join a server with this map first");
            return false;
        }
#ifndef NETCODE_DISABLE
        if (!serverPeer)
        {
            failMapDownload("Server disconnected before map transfer");
            return false;
        }
        clientDownloadingMap = true;
        clientMapProgressTime = timerGetTicks();
        LOG_F(INFO, "NET map download started crc=%08x generation=%u", crc, mapGeneration);
        requestMapWindow();
        return true;
#else
        failMapDownload("Networking is disabled");
        return false;
#endif
    }
    if (h.type == netwire::Reject)
    {
        std::string reason = r.text(160);
        if (r.end())
        {
            LOG_F(ERROR, "NET rejected: %s", reason.c_str());
            if (!replay && !clientContentChecked && reason == "Invalid client profile")
                failContentCheck({}, "The server rejected the join format. Update the server and client to compatible versions with mod asset checking.");
            else if (clientDownloadingMap)
                failMapDownload(reason);
            else
                viewSetErrorMessage(reason.c_str());
        }
        return false;
    }
    if (h.session != session || h.generation != mapGeneration)
    {
        duplicateMessages++;
        return false;
    }
    if (h.type == netwire::MapData)
    {
        if (replay || !clientDownloadingMap)
            return false;
        netwire::MapDataPayload chunk;
        if (!netwire::readMapData(r, chunk) || chunk.mapCRC != clientMapCRC || chunk.offset != clientMapBytes.size()
            || (clientMapSize && chunk.totalBytes != clientMapSize))
        {
            malformedMessages++;
            failMapDownload("Invalid map transfer chunk");
            return false;
        }
        clientMapSize = chunk.totalBytes;
        clientMapBytes.insert(clientMapBytes.end(), chunk.data, chunk.data + chunk.length);
        clientMapProgressTime = timerGetTicks();
        if (clientMapBytes.size() == clientMapSize)
        {
            std::string error;
            if (!netMapInstall(clientMapOptions.zLevelName, clientMapCRC, clientMapBytes, error))
            {
                failMapDownload(error);
                return false;
            }
            LOG_F(INFO, "NET map download complete crc=%08x bytes=%u", clientMapCRC, clientMapSize);
            return startClientMap();
        }
#ifndef NETCODE_DISABLE
        if (clientMapBytes.size() % (netwire::MapChunkBytes * netwire::MapWindowChunks) == 0)
            requestMapWindow();
#endif
        return true;
    }
    if (h.type == netwire::Snapshot)
    {
        if (!clientLoaded)
            return false;
        uint32_t tick = r.u32(), id = r.u32(), baseline = r.u32(), ack[8];
        for (auto &a : ack) a = r.u32();
        uint8_t  mask   = r.u8();
        uint32_t length = r.u32();
        if (!r.ok || !(mask & (1 << myconnectindex)) || !id || length != r.size - r.pos)
        {
            malformedMessages++;
            return false;
        }
        if (clientSnapshot && !netwire::newer(id, clientSnapshot))
        {
            duplicateMessages++;
            return true;
        }
        const Bytes *base = baseline ? findSnapshot(baseline) : nullptr;
        if (baseline && !base)
        {
#ifndef NETCODE_DISABLE
            if (!replay)
            {
                netwire::Writer w;
                w.u32(0);
                sendTo(serverPeer, netwire::SnapshotAck, w.data, false);
            }
#endif
            return false;
        }
        Bytes       packed(length), canonical;
        std::string error;
        r.bytes(packed.data(), length);
        if (!r.end() || !netSnapshotUnpack(packed, base, canonical, error))
        {
            malformedMessages++;
            LOG_F(WARNING, "NET rejected snapshot: %s", error.c_str());
            return false;
        }
        if (!netSnapshotValidate(canonical, error))
        {
            malformedMessages++;
            return false;
        }
        netPredictBeforeSnapshot();
        if (!netSnapshotApply(canonical, myconnectindex, error))
        {
            malformedMessages++;
            LOG_F(ERROR, "NET apply snapshot: %s", error.c_str());
            return false;
        }
        if (Bgetenv("NBLOOD_NET_VERIFY_SNAPSHOTS"))
        {
            Bytes roundtrip;
            bool  equal = netSnapshotCapture(roundtrip, mask, error) && roundtrip == canonical;
            LOG_F(INFO, "NET snapshot roundtrip id=%u equal=%d incoming=%08x installed=%08x", id, int(equal), netSnapshotHash(canonical),
                  netSnapshotHash(roundtrip));
        }
        netEventsOnSnapshot();
        bool     initial    = !clientSnapshotReady;
        uint32_t previous   = clientSnapshot;
        clientSnapshot      = id;
        serverTick          = tick;
        clientSnapshotReady = true;
        lastSnapshotTime    = timerGetTicks();
        cacheSnapshot(id, canonical);
        while (!commandHistory.empty() && !netwire::newer(commandHistory.front().sequence, ack[myconnectindex])) commandHistory.pop_front();
        netPredictSnapshot(ack[myconnectindex], tick, initial);
        netLagometerCommandAck(commandHistory.size());
#ifndef NETCODE_DISABLE
        int ping = serverPeer ? int(serverPeer->roundTripTime) : 0;
        if (!replay)
        {
            netwire::Writer w;
            w.u32(id);
            sendTo(serverPeer, netwire::SnapshotAck, w.data, false);
        }
#else
        int ping = 0;
#endif
        netLagometerSnapshot(ping, previous ? int(id - previous - 1) : 0);
        netLagometerDisconnect(false);
        // Packet demos store independent full snapshots, including ones received as deltas.
        Bytes fullPacked;
        bool  delta = false;
        if (netSnapshotPack(canonical, nullptr, fullPacked, delta, error))
        {
            netwire::Writer w;
            w.u32(tick);
            w.u32(id);
            w.u32(0);
            for (auto a : ack) w.u32(a);
            w.u8(mask);
            w.u32(fullPacked.size());
            w.bytes(fullPacked.data(), fullPacked.size());
            lastRecordedSnapshot = netwire::encode(h, w.data);
            if (netDemoIsRecording() && !replay)
                netDemoRecordPacket(tick, lastRecordedSnapshot);
        }
        if (initial || id % 30 == 0)
            LOG_F(INFO, "NET client slot=%d snapshot=%u tick=%u ack=%u baseline=%u hash=%08x", myconnectindex, id, tick, ack[myconnectindex], baseline,
                  netSnapshotHash(canonical));
        return true;
    }
    bool syntheticBaseline = replay && h.type == netwire::Event && h.sequence == 0;
    if (!syntheticBaseline && clientReceived[h.type] && !netwire::newer(h.sequence, clientReceived[h.type]))
    {
        duplicateMessages++;
        return true;
    }
    if (!syntheticBaseline)
        clientReceived[h.type] = h.sequence;
    if (h.type == netwire::Event)
    {
        uint32_t eventTick = r.u32();
        UNREFERENCED_PARAMETER(eventTick);
        if (!r.ok || r.size - r.pos > 1024)
            return false;
        bool ok = netPlayEvent(r.data + r.pos, r.size - r.pos);
        if (ok && netDemoIsRecording() && !replay)
            netDemoRecordPacket(serverTick, Bytes(data, data + size));
        return ok;
    }
    if (h.type == netwire::Chat)
    {
        int         p       = r.u8();
        std::string message = r.text(240);
        if (r.end() && p < 8)
        {
            char text[320];
            Bsnprintf(text, sizeof(text), "%s: %s", gProfile[p].name, message.c_str());
            viewSetMessage(text, 10);
            if (netDemoIsRecording() && !replay)
                netDemoRecordPacket(serverTick, Bytes(data, data + size));
            return true;
        }
    }
    if (h.type == netwire::Taunt)
    {
        int taunt = r.u8();
        if (r.end() && taunt < 10)
        {
            sndStartSample(4400 + taunt, 128, 1, 0);
            if (netDemoIsRecording() && !replay)
                netDemoRecordPacket(serverTick, Bytes(data, data + size));
            return true;
        }
    }
    return false;
}
#ifndef NETCODE_DISABLE
void serverReceive(ENetPeer *peer, const uint8_t *data, size_t size)
{
    int slot = -1;
    for (int p = 0; p < 8; p++)
        if (slots[p].peer == peer)
        {
            slot = p;
            break;
        }
    if (slot < 0)
        return;
    ClientSlot     &c = slots[slot];
    netwire::Header h;
    netwire::Reader r(nullptr, 0);
    if (!netwire::decode(data, size, h, r))
    {
        malformedMessages++;
        return;
    }
    if (h.type == netwire::Hello)
    {
        if (c.admitted || c.contentOffered)
            return;
        uint32_t fingerprint = r.u32();
        std::string name = r.text(MAXPLAYERNAME - 1);
        int         aim = r.u8(), weapon = r.u8(), skill = r.u8();
        uint32_t capability = r.pos < r.size ? r.u32() : 0;
        if (!r.end() || aim > 3 || weapon > 3 || skill > 4)
        {
            sendReject(peer, "Invalid client profile");
            return;
        }
        Bstrncpyz(gProfile[slot].name, name.c_str(), MAXPLAYERNAME);
        // A profile is not an authority to override server gameplay rules.
        gProfile[slot].nAutoAim      = serverAutoAim();
        gProfile[slot].nWeaponSwitch = weapon;
        gProfile[slot].skill         = serverPlayerSkill();
        if (capability)
        {
            if (capability != netassets::ManifestVersion)
            {
                sendReject(peer, "Unsupported asset manifest version; update client and server");
                return;
            }
            Bytes manifest;
            std::string error;
            if (!netassets::encodeManifest(netContentManifest(), manifest, error))
            {
                LOG_F(ERROR, "NET cannot provide content manifest: %s", error.c_str());
                sendReject(peer, "Server cannot describe its mod assets; check the server log");
                return;
            }
            netwire::Writer w;
            w.u32(contentCRC);
            w.bytes(manifest.data(), manifest.size());
            c.contentOffered = true;
            c.contentFingerprint = fingerprint;
            if (!sendTo(peer, netwire::ContentManifest, w.data))
                sendReject(peer, "Server could not send the mod asset manifest");
            return;
        }
        // Older clients can still join with the original fingerprint check.
        // Their protocol cannot display per-file diagnostics.
        if (fingerprint != contentCRC)
        {
            LOG_F(ERROR, "NET game content mismatch local=%08x remote=%08x", contentCRC, fingerprint);
            sendReject(peer, "Game content mismatch; install the server's mod or update the client for file details");
            return;
        }
        c.admitted                   = true;
        sendWelcome(slot);
        return;
    }
    if (h.type == netwire::ContentReady)
    {
        // The server can change maps while the client checks files. Admission
        // is session-scoped; sendWelcome describes the current map generation.
        if (!c.contentOffered || c.admitted || h.session != session)
            return;
        uint32_t fingerprint = r.u32();
        if (!r.end() || fingerprint != contentCRC || c.contentFingerprint != contentCRC)
        {
            sendReject(peer, "Game content mismatch; select the server's mod and restart");
            return;
        }
        c.admitted = true;
        sendWelcome(slot);
        return;
    }
    if (!c.admitted || h.session != session || h.generation != mapGeneration)
        return;
    if (h.type != netwire::Commands && c.received[h.type] && !netwire::newer(h.sequence, c.received[h.type]))
    {
        duplicateMessages++;
        return;
    }
    if (h.type == netwire::RequestMap)
    {
        netwire::MapRequestPayload request;
        if (!netwire::readMapRequest(r, request) || request.slotGeneration != c.generation || request.mapCRC != gGameOptions.uMapCRC)
        {
            malformedMessages++;
            return;
        }
        // Serve only the map currently running on this server. A peer cannot
        // choose a filename, restart a transfer, or queue unbounded windows.
        if (!gGameStarted || gStartNewGame || c.ready || c.pendingReady || c.mapRequestPending || request.offset != c.mapDownloadNext
            || (c.mapDownloadStarted && !request.offset))
            return;
        if (serverMapBytes.empty())
        {
            std::string error;
            if (!netMapRead(gGameOptions.zLevelName, serverMapBytes, error)
                || !netMapValidate(serverMapBytes, gGameOptions.uMapCRC, error))
            {
                serverMapBytes.clear();
                LOG_F(ERROR, "NET map transfer unavailable: %s", error.c_str());
                sendReject(peer, "Server cannot provide the current map");
                return;
            }
        }
        if (request.offset >= serverMapBytes.size())
            return;
        if (!c.mapDownloadStarted)
            LOG_F(INFO, "NET map upload started slot=%d crc=%08x bytes=%u", slot, request.mapCRC, unsigned(serverMapBytes.size()));
        c.mapDownloadStarted = true;
        c.mapRequestPending = true;
        c.mapWindowEnd = std::min(uint32_t(serverMapBytes.size()), request.offset + netwire::MapChunkBytes * netwire::MapWindowChunks);
    }
    else if (h.type == netwire::Ready)
    {
        uint32_t generation = r.u32(), crc = r.u32();
        if (!r.end() || generation != c.generation || crc != gGameOptions.uMapCRC)
        {
            sendReject(peer, "Map content mismatch");
            return;
        }
        c.pendingReady = true;
    }
    else if (h.type == netwire::SnapshotAck)
    {
        uint32_t id = r.u32();
        if (r.end() && (!id || findSnapshot(id)))
            c.snapshotAck = id;
    }
    else if (h.type == netwire::Commands && c.ready)
    {
        uint32_t generation = r.u32();
        unsigned count      = r.u8();
        if (generation != c.generation || !count || count > 8)
        {
            malformedMessages++;
            return;
        }
        netwire::Command commands[8];
        for (unsigned i = 0; i < count; i++)
            if (!netwire::readCommand(r, commands[i]))
            {
                malformedMessages++;
                return;
            }
        if (!r.end())
        {
            malformedMessages++;
            return;
        }
        for (unsigned i = 0; i < count; i++)
            if (!c.commands.push(commands[i]))
            {
                if (netwire::newer(commands[i].sequence, c.commands.acknowledged) && commands[i].sequence - c.commands.acknowledged > 255
                    && timerGetTicks() - c.lastInputTime > 1000)
                {
                    c.commands.resync(commands[i]);
                    LOG_F(INFO, "NET command window resynchronized slot=%d next=%u ack=%u", slot, commands[i].sequence, c.commands.acknowledged);
                }
                else
                    duplicateMessages++;
            }
    }
    else if (h.type == netwire::Chat)
    {
        std::string message = r.text(240);
        if (r.end())
        {
            netwire::Writer w;
            w.u8(slot);
            w.text(message, 240);
            for (auto &dest : slots)
                if (dest.ready)
                    sendTo(dest.peer, netwire::Chat, w.data);
            if (!gDedicated)
                viewSetMessage(message.c_str(), 10);
        }
    }
    else if (h.type == netwire::Taunt)
    {
        uint8_t taunt = r.u8();
        if (r.end() && taunt < 10)
        {
            netwire::Writer w;
            w.u8(taunt);
            for (auto &dest : slots)
                if (dest.ready)
                    sendTo(dest.peer, netwire::Taunt, w.data);
            if (!gDedicated)
                sndStartSample(4400 + taunt, 128, 1, 0);
        }
    }
    c.received[h.type] = h.sequence;
}
#endif
}  // namespace
void netResetState(void)
{
    gNetFifoClock = gFrameClock = totalclock = 0;
    gNetFifoMasterTail                       = 0;
    gPredictTail                             = 0;
    gNetFifoTail                             = 0;
    memset(gNetFifoHead, 0, sizeof(gNetFifoHead));
    memset(gCheckFifo, 0, sizeof(gCheckFifo));
    memset(myMinLag, 0, sizeof(myMinLag));
    otherMinLag = 0;
    myMaxLag    = 0;
    memset(gCheckHead, 0, sizeof(gCheckHead));
    gSendCheckTail = 0;
    gCheckTail     = 0;
    bOutOfSync     = 0;
    gBufferJitter  = 1;
}

void CalcGameChecksum(void)
{
    memset(gChecksum, 0, sizeof(gChecksum));
    gChecksum[0] = wrand();
    for (int p = connecthead; p >= 0; p = connectpoint2[p])
    {
        gChecksum[1] ^= gPlayer[p].CalcNonSpriteChecksum();

        int *pBuffer = (int *)gPlayer[p].pSprite;
        int  sum     = 0;
        int  length  = sizeof(spritetype) / 4;
        while (length--) sum += *pBuffer++;
        gChecksum[2] ^= sum;

        gChecksum[3] ^= gPlayer[p].pXSprite->CalcChecksum();
    }
}


bool netIsLive()
{ return gNetMode != NETWORK_NONE || demoPlayback; }
uint32_t netAppliedCommandSequence(int player)
{ return netIsServer() && player >= 0 && player < 8 ? slots[player].commands.acknowledged : 0; }
bool netInServerTick()
{ return serverInTick; }
bool netIsServer()
{ return gNetMode == NETWORK_SERVER; }
bool netIsClient()
{ return gNetMode == NETWORK_CLIENT || demoPlayback; }
bool netClientHasSnapshot()
{ return clientSnapshotReady; }

void netResetToSinglePlayer(void)
{
    myconnectindex = connecthead = 0;
    gInitialNetPlayers = gNetPlayers = numplayers = 1;
    for (int p = 0; p < 8; p++) connectpoint2[p] = -1;
    gGameOptions.nGameType = kGameTypeSinglePlayer;
    gNetMode               = NETWORK_NONE;
    gMe = gView = &gPlayer[0];
    gViewIndex  = 0;
    UpdateNetworkMenus();
    gGameMenuMgr.Deactivate();
}
void netInitialize(bool bConsole)
{
    UNREFERENCED_PARAMETER(bConsole);
    NETWORKMODE mode = gNetMode;
    if (mode != NETWORK_NONE)
    {
        if (gDemo.at1)
            gDemo.StopPlayback();
        if (gDemo.at0)
            gDemo.Close();
    }
    int maxPlayers = ClipRange(gNetPlayers, 1, 8);
    netDeinitialize();
    gNetMode           = mode;
    gInitialNetPlayers = maxPlayers;
    contentCRC         = netContentCRC();
    netResetState();
    netLagReset();
    std::fill(std::begin(slots), std::end(slots), ClientSlot());
    snapshotCache.clear();
    commandHistory.clear();
    pendingClientPackets.clear();
    std::fill(std::begin(clientReceived), std::end(clientReceived), 0);
    clientLoaded = clientSnapshotReady = false;
    clientSnapshot                     = 0;
    commandSequence = sendSequence = serverTick = snapshotId = mapGeneration = 0;
    sentMessages = receivedMessages = malformedMessages = duplicateMessages = 0;
    netPredictReset();
    if (mode == NETWORK_NONE)
    {
        netResetToSinglePlayer();
        return;
    }
#ifndef NETCODE_DISABLE
    if (enet_initialize() != 0)
    {
        LOG_F(ERROR, "NET ENet initialization failed");
        netResetToSinglePlayer();
        return;
    }
    initialized         = true;
    ENetAddress address = {};
    if (mode == NETWORK_SERVER)
    {
        address.host = ENET_HOST_ANY;
        address.port = gNetPort;
        host         = enet_host_create(&address, 8, 3, 0, 0);
        session      = timerGetTicks() ^ uint32_t(timerGetFractionalTicks() * 1000.0) ^ 0x4e424c44u;
        if (!session)
            session = 1;
        myconnectindex = gDedicated ? -1 : 0;
        if (!gDedicated)
            slots[0].active = slots[0].ready = true;
        rebuildRoster();
        LOG_F(INFO, "NET server listening UDP port=%d dedicated=%d maxplayers=%d copies=%d", gNetPort, int(gDedicated), gInitialNetPlayers, gNetPacketCopies);
    }
    else
    {
        host    = enet_host_create(nullptr, 1, 3, 0, 0);
        session = 0;
        if (host && enet_address_set_host(&address, gNetAddress) == 0)
        {
            address.port = gNetPort;
            serverPeer   = enet_host_connect(host, &address, 3, netwire::Version);
        }
        myconnectindex = 0;
        connecthead    = 0;
        numplayers = gNetPlayers = 1;
        for (auto &p : connectpoint2) p = -1;
        LOG_F(INFO, "NET connecting UDP %s:%d", gNetAddress, gNetPort);
    }
    if (!host || (mode == NETWORK_CLIENT && !serverPeer))
    {
        LOG_F(ERROR, "NET socket/resolve failed");
        netDeinitialize();
        netResetToSinglePlayer();
        return;
    }
    host->checksum           = enet_crc32;
    host->maximumPacketSize  = netwire::MaxPayload + netwire::HeaderSize;
    host->maximumWaitingData = 32u * 1024u * 1024u;
    host->packetCopies       = ClipRange(gNetPacketCopies, 1, 8);
    enet_socket_set_option(host->socket, ENET_SOCKOPT_RCVBUF, 4 * 1024 * 1024);
    enet_socket_set_option(host->socket, ENET_SOCKOPT_SNDBUF, 4 * 1024 * 1024);
#else
    LOG_F(ERROR, "Networking is disabled in this build");
    netResetToSinglePlayer();
#endif
}
void netDeinitialize(void)
{
    netStopPacketRecording();
    netDemoStopPlayback();
    demoPlayback = false;
    serverInTick = false;
    restoreClientLevels();
    netMapClear();
    Bytes().swap(serverMapBytes);
    Bytes().swap(clientMapBytes);
    clientDownloadingMap = mapDownloadDisconnected = false;
    clientMapSize = 0;
    mapDownloadError.clear();
    clientContentChecked = false;
    clientServerRules = {};
    netContentReportClear();
#ifndef NETCODE_DISABLE
    if (host)
    {
        for (size_t n = 0; n < host->peerCount; n++)
            if (host->peers[n].state != ENET_PEER_STATE_DISCONNECTED)
                enet_peer_disconnect_now(&host->peers[n], 0);
        enet_host_flush(host);
        enet_host_destroy(host);
        host = nullptr;
    }
    serverPeer = nullptr;
    if (initialized)
        enet_deinitialize();
#endif
    initialized  = false;
    clientLoaded = clientSnapshotReady = false;
    pendingClientPackets.clear();
    pendingClientBytes = 0;
    snapshotCache.clear();
    commandHistory.clear();
    netPredictReset();
    netEventsReset();
    netLagReset();
}
void netDisconnect()
{
    netStopPacketRecording();
    netDemoStopPlayback();
    demoPlayback = false;
    netDeinitialize();
    netResetToSinglePlayer();
    gGameStarted = false;
    ready2send   = false;
}
void netUpdate(void)
{
#ifndef NETCODE_DISABLE
    if (!host || servicing)
        return;
    servicing          = true;
    host->packetCopies = ClipRange(gNetPacketCopies, 1, 8);
    ENetEvent event;
    int       budget = 256;
    while (budget-- && enet_host_service(host, &event, 0) > 0)
    {
        if (event.type == ENET_EVENT_TYPE_CONNECT)
        {
            enet_peer_timeout(event.peer, 16, 5000, 15000);
            if (netIsServer())
            {
                int slot     = -1;
                int occupied = 0;
                for (auto &c : slots)
                    if (c.peer || c.active)
                        occupied++;
                if (occupied < ClipRange(gInitialNetPlayers, 1, 8))
                    for (int p = 0; p < 8; p++)
                        if (!slots[p].peer && !slots[p].active)
                        {
                            slot = p;
                            break;
                        }
                if (slot < 0 || event.data != netwire::Version)
                {
                    sendReject(event.peer, slot < 0 ? "Server full" : "Protocol version mismatch");
                    continue;
                }
                uint32_t generation = slots[slot].generation + 1;
                if (!generation)
                    generation = 1;
                slots[slot]            = ClientSlot();
                slots[slot].generation = generation;
                slots[slot].peer       = event.peer;
                LOG_F(INFO, "NET connected slot=%d generation=%u", slot, generation);
            }
            else
            {
                netwire::Writer w;
                w.u32(contentCRC);
                w.text(szPlayerName, MAXPLAYERNAME - 1);
                w.u8(ClipRange(gAutoAim, 0, 3));
                w.u8(ClipRange(gWeaponSwitch, 0, 3));
                w.u8(ClipRange(gSkill, 0, 4));
                w.u32(netassets::ManifestVersion);
                sendTo(serverPeer, netwire::Hello, w.data);
            }
        }
        else if (event.type == ENET_EVENT_TYPE_RECEIVE)
        {
            receivedMessages++;
            if (netIsServer())
                serverReceive(event.peer, event.packet->data, event.packet->dataLength);
            else if (pendingClientPackets.size() < 128 && event.packet->dataLength <= 32u * 1024u * 1024u - pendingClientBytes)
            {
                pendingClientBytes += event.packet->dataLength;
                pendingClientPackets.emplace_back(event.packet->data, event.packet->data + event.packet->dataLength);
            }
            enet_packet_destroy(event.packet);
        }
        else if (event.type == ENET_EVENT_TYPE_DISCONNECT)
        {
            if (netIsServer())
            {
                for (auto &c : slots)
                    if (c.peer == event.peer)
                    {
                        c.peer        = nullptr;
                        c.pendingDrop = true;
                    }
            }
            else
            {
                serverPeer          = nullptr;
                clientSnapshotReady = false;
                mapDownloadDisconnected = clientDownloadingMap;
                netLagometerDisconnect(true);
                LOG_F(WARNING, "NET disconnected from server");
            }
        }
    }
    pumpMapDownloads();
    enet_host_flush(host);
    servicing = false;
#endif
}
void faketimerhandler(void)
{ netUpdate(); }
void netGetPackets(void)
{ netUpdate(); }
void netClientPump()
{
    bool wasRecording = netDemoIsRecording();
    netUpdate();
    if (!netIsClient())
        return;
    if (clientDownloadingMap && (mapDownloadDisconnected || timerGetTicks() - clientMapProgressTime > 30000))
    {
        failMapDownload(mapDownloadDisconnected ? "Server disconnected during map transfer" : "Map transfer timed out");
        return;
    }
    if (demoPlayback)
    {
        Bytes    packet;
        uint32_t tick;
        int      budget = 64;
        while (budget-- && netDemoNextPacket(timerGetTicks() - playbackStart, packet, tick))
        {
            if (!netDemoReplayPacket(packet.data(), packet.size()))
            {
                if (mapDownloadError.empty())
                    LOG_F(ERROR, "NET demo contains an invalid protocol packet");
                netDemoStopPlayback();
                playbackEndReported = true;
                break;
            }
            if (gStartNewGame)
                break;
        }
        if (!netDemoIsPlaying() && !playbackEndReported)
        {
            playbackEndReported = true;
            if (!netDemoLastError().empty())
                LOG_F(ERROR, "NET demo playback failed: %s", netDemoLastError().c_str());
            else
                LOG_F(INFO, "NET demo playback completed");
        }
    }
    else
    {
        while (!pendingClientPackets.empty())
        {
            Bytes data = std::move(pendingClientPackets.front());
            pendingClientPackets.pop_front();
            pendingClientBytes -= data.size();
            acceptClientPacket(data.data(), data.size(), false);
            if (gStartNewGame)
                break;
        }
    }
    if (wasRecording && !netDemoIsRecording() && !netDemoLastError().empty())
        LOG_F(ERROR, "NET demo recording failed: %s", netDemoLastError().c_str());
}
void netDrawConnectionStatus()
{
    if (gDedicated)
        return;
    if (netContentReportDraw())
        return;
    if (clientDownloadingMap)
    {
        char progress[80];
        if (clientMapSize)
            Bsnprintf(progress, sizeof(progress), "%u%% (%u / %u KB)", unsigned(clientMapBytes.size() * 100 / clientMapSize),
                      unsigned((clientMapBytes.size() + 1023) / 1024), unsigned((clientMapSize + 1023) / 1024));
        else
            Bstrncpyz(progress, "Waiting for server", sizeof(progress));
        viewDrawText(3, "Downloading map", 160, 85, -128, 0, 1, 1);
        viewDrawText(3, progress, 160, 100, -128, 0, 1, 1);
    }
    else if (!mapDownloadError.empty() && !gGameStarted)
    {
        viewDrawText(3, "Map download failed", 160, 85, -128, 0, 1, 1);
        viewDrawText(3, "See console for details", 160, 100, -128, 0, 1, 1);
    }
}
void netOnLevelStarted()
{
    if (!netIsLive())
        return;
    netPredictReset();
    netEventsReset();
    if (netIsServer())
    {
        enforceServerProfiles();
        Bytes().swap(serverMapBytes);
        commandSequence = 0;
        if (netDemoIsRecording())
        {
            recordNeedsWelcome = true;
            recordResetClock   = true;
        }
        mapGeneration++;
        if (!mapGeneration)
            mapGeneration = 1;
        serverTick = snapshotId = lastSnapshotTick = 0;
        snapshotBudget                             = 30;
        snapshotCache.clear();
        nextTickTime = timerGetTicks();
        for (int p = 0; p < 8; p++)
        {
            slots[p].commands         = netwire::CommandQueue();
            slots[p].snapshotAck      = 0;
            slots[p].fullSnapshotTime = 0;
            slots[p].lastInput        = {};
            slots[p].ready            = slots[p].active && p == myconnectindex;
#ifndef NETCODE_DISABLE
            if (slots[p].peer && slots[p].admitted)
                sendWelcome(p);
#endif
        }
        LOG_F(INFO, "NET map generation=%u crc=%08x players=%d", mapGeneration, gGameOptions.uMapCRC, numplayers);
    }
    else
    {
        if (clientMapCRC != gGameOptions.uMapCRC)
        {
            LOG_F(ERROR, "NET local map CRC mismatch expected=%08x actual=%08x", clientMapCRC, gGameOptions.uMapCRC);
            netDisconnect();
            return;
        }
        clientLoaded = true;
        gMe = gView = &gPlayer[myconnectindex];
        gViewIndex  = myconnectindex;
#ifndef NETCODE_DISABLE
        if (!demoPlayback)
        {
            netwire::Writer w;
            w.u32(clientSlotGeneration);
            w.u32(gGameOptions.uMapCRC);
            sendTo(serverPeer, netwire::Ready, w.data);
        }
#endif
        LOG_F(INFO, "NET client map loaded slot=%d generation=%u", myconnectindex, mapGeneration);
    }
}
bool netServerBeginTick()
{
    if (!netIsServer() || !gGameStarted)
        return false;
    uint32_t now = timerGetTicks();
    if (int32_t(now - nextTickTime) < 0)
        return false;
    // Fixed 30Hz deadlines, bounded catch-up after a suspended process.
    if (now - nextTickTime > 1000)
        nextTickTime = now;
    nextTickTime += 33 + (serverTick % 3 == 2);
    enforceServerProfiles();
    for (int p = 0; p < 8; p++)
    {
        auto &c = slots[p];
        if (c.pendingDrop)
        {
            removePlayer(p);
            continue;
        }
        if (c.pendingReady)
        {
            c.pendingReady     = false;
            c.fullSnapshotTime = 0;
            if (!c.active)
            {
                c.active = true;
                rebuildRoster();
                // A hot join resets only the joining player's personal scores.
                // Existing team scores belong to the match and its score limit.
                playerInit(p, 0, false);
                playerStart(p, 1);
            }
            c.ready         = true;
            c.lastInput     = {};
            c.lastInputTime = now;
            LOG_F(INFO, "NET joined slot=%d generation=%u players=%d tick=%u", p, c.generation, numplayers, serverTick);
            lastSnapshotTick = 0;
        }
        if (!c.active)
            continue;
        netwire::Command command;
        GINPUT           input = {};
        if (c.ready && c.commands.pop(command))
        {
            input           = toInput(command);
            c.lastInput     = input;
            c.lastInputTime = now;
        }
        else if (c.ready && now - c.lastInputTime < 250)
        {
            input               = c.lastInput;
            input.keyFlags.word = 0;
            input.useFlags.byte = 0;
            input.newWeapon     = 0;
            input.q16turn       = 0;
            input.q16mlook      = 0;
        }
        gFifoInput[gNetFifoTail & 255][p] = input;
        gNetFifoHead[p]                   = gNetFifoTail + 1;
    }
    serverInTick = true;
    return true;
}
void netServerEndTick()
{
    serverInTick = false;
    if (!netIsServer())
        return;
    serverTick++;
    if (gStartNewGame)
        return;
    if (serverTick % 30 == 0)
        LOG_F(INFO, "NET server_tick=%u players=%d snapshots=%u", serverTick, numplayers, snapshotId);
    snapshotBudget += ClipRange(gNetSnapshotRate, 1, 30);
    if (lastSnapshotTick && snapshotBudget < 30)
        return;
    snapshotBudget %= 30;
    lastSnapshotTick = serverTick;
    Bytes       state;
    std::string error;
    if (!netSnapshotCapture(state, playerMask(), error))
    {
        LOG_F(ERROR, "NET capture failed: %s", error.c_str());
        return;
    }
    uint32_t id = ++snapshotId;
    if (!id)
        id = ++snapshotId;
#ifndef NETCODE_DISABLE
    for (auto &c : slots)
        if (c.peer && c.ready)
        {
            const Bytes *base = c.snapshotAck ? findSnapshot(c.snapshotAck) : nullptr;
            if (!base && c.fullSnapshotTime && timerGetTicks() - c.fullSnapshotTime < 1000)
                continue;
            if (!base)
                c.fullSnapshotTime = timerGetTicks();
            Bytes body = snapshotBody(state, id, c.snapshotAck, base);
            if (!body.empty())
            {
                sendTo(c.peer, netwire::Snapshot, body, !base);
                if (c.pendingLoops)
                {
                    std::vector<Bytes> events;
                    netEventsBaseline(events);
                    for (auto &event : events)
                    {
                        netwire::Writer w;
                        w.u32(serverTick);
                        w.bytes(event.data(), event.size());
                        sendTo(c.peer, netwire::Event, w.data);
                    }
                    c.pendingLoops = false;
                }
            }
        }
#endif
    if (id == 1 || id % 30 == 0)
        LOG_F(INFO, "NET server snapshot=%u tick=%u hash=%08x bytes=%u", id, serverTick, netSnapshotHash(state), unsigned(state.size()));
    cacheSnapshot(id, state);
    if (netDemoIsRecording() && !recordServerSnapshot(state, id))
        LOG_F(ERROR, "NET server packet recording failed: %s", netDemoLastError().c_str());
    netUpdate();
}
void netGetInput(void)
{
    if (netIsClient())
    {
        if (!clientSnapshotReady || demoPlayback)
            return;
        uint32_t id = ++commandSequence;
        if (!id)
            id = ++commandSequence;
        netwire::Command command = fromInput(gNetInput, id);
        commandHistory.push_back(command);
        while (commandHistory.size() > 128) commandHistory.pop_front();
        GINPUT input = toInput(command);
        netPredictCommand(id, input);
#ifndef NETCODE_DISABLE
        netwire::Writer w;
        w.u32(clientSlotGeneration);
        unsigned count = std::min<size_t>(commandHistory.size(), 8);
        w.u8(count);
        for (size_t n = commandHistory.size() - count; n < commandHistory.size(); n++) netwire::writeCommand(w, commandHistory[n]);
        sendTo(serverPeer, netwire::Commands, w.data, false);
        enet_host_flush(host);
#endif
        return;
    }
    if (netIsServer())
    {
        if (myconnectindex >= 0)
        {
            uint32_t id = ++commandSequence;
            if (!id)
                id = ++commandSequence;
            slots[myconnectindex].commands.push(fromInput(gNetInput, id));
        }
        return;
    }
    // Preserve the original offline and legacy-demo FIFO path exactly.
    GINPUT &input = gFifoInput[gNetFifoHead[myconnectindex] & 255][myconnectindex];
    input         = gNetInput;
    gNetFifoHead[myconnectindex]++;
    for (int p = connecthead; p >= 0; p = connectpoint2[p])
        if (p != myconnectindex)
        {
            gFifoInput[gNetFifoHead[p] & 255][p] = gFifoInput[(gNetFifoHead[p] - 1) & 255][p];
            gNetFifoHead[p]++;
        }
}
void netMasterUpdate(void)
{
}
void netCheckSync(void)
{
}
void netWaitForEveryone(char unused)
{
    UNREFERENCED_PARAMETER(unused);
    netUpdate();
}
void netPrepareCurrentMapSettings()
{
    netServerApplySettings();
    configureStart(gGameOptions);
}
void netBroadcastNewGame(void)
{
    if (!netIsServer())
        return;
    const PKT_STARTGAME &settings = gPacketStartGame;
    if (settings.gameType <= kGameTypeSinglePlayer || settings.gameType > kGameTypeTeams || settings.difficulty > 4 || settings.episodeId >= kMaxEpisodes
        || settings.levelId >= kMaxLevels || (!settings.userMap && settings.levelId >= gEpisodeInfo[int(settings.episodeId)].nLevels))
    {
        LOG_F(ERROR, "NET invalid server map/rule settings");
        return;
    }
    gGameOptions.nGameType   = settings.gameType;
    gGameOptions.nEpisode    = settings.episodeId;
    gGameOptions.nLevel      = settings.levelId;
    gGameOptions.nDifficulty = gGameOptions.nDifficultyQuantity = gGameOptions.nDifficultyHealth = settings.difficulty;
    gGameOptions.nMonsterSettings                                                                = settings.monsterSettings;
    gGameOptions.nWeaponSettings                                                                 = settings.weaponSettings;
    gGameOptions.nItemSettings                                                                   = settings.itemSettings;
    gGameOptions.nRespawnSettings                                                                = settings.respawnSettings;
    gGameOptions.bFriendlyFire                                                                   = settings.bFriendlyFire;
    gGameOptions.bPlayerKeys                                                                     = settings.bPlayerKeys;
    gGameOptions.weaponsV10x                                                                     = settings.weaponsV10x;
    gGameOptions.uGameFlags &= ~kGameFlagContinuing;
    if (settings.userMap)
        Bstrncpyz(gGameOptions.szUserMap, settings.userMapName, sizeof(gGameOptions.szUserMap));
    else
        gGameOptions.szUserMap[0] = 0;
    // StartLevel performs map/resource loading on the main thread after the
    // current authoritative tick has ended. No peer can initiate this path.
    gStartNewGame = true;
}
void netBroadcastPlayerInfo(int p)
{
    if (p < 0 || p >= 8)
        return;
    Bstrncpyz(gProfile[p].name, szPlayerName, sizeof(gProfile[p].name));
    if (!netIsClient())
        gProfile[p].nAutoAim = netIsServer() ? serverAutoAim() : gAutoAim;
    gProfile[p].nWeaponSwitch = gWeaponSwitch;
    if (!netIsClient())
        gProfile[p].skill = netIsServer() ? serverPlayerSkill() : gSkill;
}
void netBroadcastMessage(int p, const char *text)
{
#ifndef NETCODE_DISABLE
    if (!text)
        return;
    netwire::Writer w;
    if (netIsServer())
        w.u8(ClipRange(p, 0, 7));
    w.text(text, 240);
    if (netIsServer())
    {
        for (auto &c : slots)
            if (c.ready)
                sendTo(c.peer, netwire::Chat, w.data);
    }
    else if (netIsClient())
        sendTo(serverPeer, netwire::Chat, w.data);
#else
    UNREFERENCED_PARAMETER(p);
    UNREFERENCED_PARAMETER(text);
#endif
}
void netBroadcastTaunt(int p, int taunt)
{
    UNREFERENCED_PARAMETER(p);
    if (taunt < 0 || taunt >= 10)
        return;
#ifndef NETCODE_DISABLE
    netwire::Writer w;
    w.u8(taunt);
    if (netIsServer())
    {
        for (auto &c : slots)
            if (c.ready)
                sendTo(c.peer, netwire::Taunt, w.data);
    }
    else if (netIsClient())
        sendTo(serverPeer, netwire::Taunt, w.data);
#endif
    if (!gDedicated)
        sndStartSample(4400 + taunt, 128, 1, 0);
}
void netBroadcastFrag(const char *text)
{ netBroadcastMessage(myconnectindex, text); }
void netPlayerQuit(int p)
{
    if (netIsServer() && p >= 0 && p < 8)
        slots[p].pendingDrop = true;
}
void netBroadcastPlayerLogoff(int p)
{ netPlayerQuit(p); }
void netBroadcastMyLogoff(bool restart)
{
    netDisconnect();
    gQuitGame = true;
    if (restart)
        gRestartGame = true;
}
void netPrintStatus()
{
    NetPredictionStats prediction = netPredictGetStats();
    LOG_F(INFO,
          "NET prediction commands=%llu replayed=%llu reconciliations=%llu pending=%u correction=%d max_correction=%d history_overflows=%llu predicted_sounds=%llu "
          "suppressed_sounds=%llu",
          (unsigned long long)prediction.commands, (unsigned long long)prediction.replayedCommands, (unsigned long long)prediction.reconciliations,
          prediction.pendingCommands, prediction.lastCorrection, prediction.maxCorrection, (unsigned long long)prediction.historyOverflows,
          (unsigned long long)prediction.predictedSounds, (unsigned long long)prediction.suppressedSounds);
    LOG_F(INFO,
          "NET status role=%s protocol=%u session=%u map_generation=%u tick=%u players=%d copies=%d snapshot=%u snapshot_hash=%08x sent=%llu received=%llu "
          "malformed=%llu duplicates=%llu",
          netIsServer()   ? "server"
          : netIsClient() ? "client"
                          : "offline",
          netwire::Version, session, mapGeneration, serverTick, numplayers, gNetPacketCopies, netIsServer() ? snapshotId : clientSnapshot,
          findSnapshot(netIsServer() ? snapshotId : clientSnapshot) ? netSnapshotHash(*findSnapshot(netIsServer() ? snapshotId : clientSnapshot)) : 0,
          (unsigned long long)sentMessages, (unsigned long long)receivedMessages, (unsigned long long)malformedMessages, (unsigned long long)duplicateMessages);
    for (int p = connecthead; p >= 0; p = connectpoint2[p])
        if (gPlayer[p].pSprite && gPlayer[p].pXSprite)
        {
            auto *s = gPlayer[p].pSprite;
            LOG_F(INFO, "NET player slot=%d x=%d y=%d z=%d health=%d ack=%u autoaim=%d skill=%d", p, int(s->x), int(s->y), int(s->z), int(gPlayer[p].pXSprite->health),
                  netAppliedCommandSequence(p), gProfile[p].nAutoAim, gProfile[p].skill);
        }
    if (netIsLive())
    {
        auto const rules = netIsServer() ? serverRules() : clientServerRules;
        LOG_F(INFO, "NET rules gametype=%d timelimit=%u scorelimit=%u maxplayers=%u autoaim=%u weaponsstay=%d difficulty=%d losekeys=%d episode=%d level=%d rotation_index=%u rotation_count=%u",
              int(gGameOptions.nGameType), rules.timeLimitMinutes, rules.scoreLimit, rules.maxPlayers, rules.autoAim,
              int(gGameOptions.nWeaponSettings == 1), int(gGameOptions.nDifficulty), int(gGameOptions.bPlayerKeys == LOSTONDEATH),
              gGameOptions.nEpisode + 1, gGameOptions.nLevel + 1, rules.rotationIndex, rules.rotationCount);
    }
#ifndef NETCODE_DISABLE
    if (host)
        LOG_F(INFO, "NET UDP sent_datagrams=%u sent_bytes=%u received_datagrams=%u received_bytes=%u", host->totalSentPackets, host->totalSentData,
              host->totalReceivedPackets, host->totalReceivedData);
#endif
}
bool netStartPacketRecording(const char *path)
{
    if (demoPlayback || (!netIsServer() && (!netIsClient() || !clientSnapshotReady)))
    {
        LOG_F(ERROR, "NET packet recording requires an active network game");
        return false;
    }
    if (netIsServer() && connecthead < 0)
    {
        LOG_F(ERROR, "NET packet recording needs an active player viewpoint; join the dedicated server first");
        return false;
    }
    recordSlot = netIsServer() ? (myconnectindex >= 0 ? myconnectindex : connecthead) : myconnectindex;
    NetDemoInfo info;
    info.protocolVersion = netwire::Version;
    info.mapCrc          = gGameOptions.uMapCRC;
    info.contentCrc      = contentCRC;
    info.tickRate        = 30;
    info.localSlot       = recordSlot;
    info.mapName         = gGameOptions.zLevelName;
    std::string error;
    if (!netDemoStartRecording(path, info, error))
    {
        LOG_F(ERROR, "NET demo: %s", error.c_str());
        return false;
    }
    if (netIsServer())
    {
        Bytes state;
        if (!netSnapshotCapture(state, playerMask(), error))
        {
            netDemoStopRecording();
            return false;
        }
        if (!snapshotId)
            snapshotId = 1;
        recordNeedsWelcome = true;
        recordResetClock   = false;
        return recordServerSnapshot(state, snapshotId);
    }
    if (!netDemoRecordPacket(serverTick, lastWelcome) || !netDemoRecordPacket(serverTick, lastRecordedSnapshot))
        return false;
    return recordLoopBaseline();
}

void netStopPacketRecording()
{
    bool wasRecording = netDemoIsRecording();
    netDemoStopRecording();
    if (wasRecording && !netDemoLastError().empty())
        LOG_F(ERROR, "NET demo recording failed: %s", netDemoLastError().c_str());
}
bool netStartPacketPlayback(const char *path)
{
    if (gDemo.at1)
        gDemo.StopPlayback();
    if (gDemo.at0)
        gDemo.Close();
    netDisconnect();
    NetDemoInfo info;
    std::string error;
    if (!netDemoStartPlayback(path, info, error))
    {
        LOG_F(ERROR, "NET demo: %s", error.c_str());
        return false;
    }
    contentCRC = netContentCRC();
    if (info.protocolVersion != netwire::Version || info.tickRate != 30 || info.contentCrc != contentCRC)
    {
        LOG_F(ERROR, "NET unsupported packet demo protocol/tick rate");
        netDemoStopPlayback();
        return false;
    }
    replayInfo          = info;
    replayFirstWelcome  = true;
    playbackEndReported = false;
    demoPlayback        = true;
    session = mapGeneration = 0;
    std::fill(std::begin(clientReceived), std::end(clientReceived), 0);
    playbackStart = timerGetTicks();
    netPredictReset();
    // The next main-loop pump installs the bootstrap. A legacy playback loop
    // interrupted by this console command must unwind before changing its roster.
    return true;
}
bool netDemoReplayPacket(const uint8_t *data, size_t size)
{
    netwire::Header h;
    netwire::Reader body(nullptr, 0);
    if (!netwire::decode(data, size, h, body)
        || (h.type != netwire::Welcome && h.type != netwire::Snapshot && h.type != netwire::Event && h.type != netwire::Chat && h.type != netwire::Taunt))
        return false;
    return acceptClientPacket(data, size, true);
}
void netBroadcastEvent(const uint8_t *data, size_t size)
{
#ifndef NETCODE_DISABLE
    if (!netIsServer() || !data || !size || size > 1024)
        return;
    netwire::Writer w;
    w.u32(serverTick);
    w.bytes(data, size);
    if (netDemoIsRecording() && recordSlot >= 0 && slots[recordSlot].active && !recordNeedsWelcome)
        netDemoRecordPacket(serverTick, envelope(netwire::Event, w.data));
    for (auto &c : slots)
        if (c.ready)
            sendTo(c.peer, netwire::Event, w.data);
#else
    UNREFERENCED_PARAMETER(data);
    UNREFERENCED_PARAMETER(size);
#endif
}
