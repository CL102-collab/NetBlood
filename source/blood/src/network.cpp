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
#include "credits.h"
#include "asound.h"
#include "levels.h"
#include "demo.h"
#include "globals.h"
#include "menu.h"
#include "network.h"
#include "netserver.h"
#include "netcoop.h"
#include "player.h"
#include "osd.h"
#include "seq.h"
#include "sfx.h"
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
char        gNetAddress[254]; // Full DNS hostname (253 bytes) plus terminator.
// PORT-TODO: Use different port?
int gNetPort = kNetDefaultPort;


PKT_STARTGAME gPacketStartGame;

#include "blood.h"
#include "inifile.h"
#include "levels.h"
#include "misc.h"
#include "netcontent.h"
#include "netcompat.h"
#include "netcontentreport.h"
#include "netdemo.h"
#include "netevents.h"
#include "netlag.h"
#include "netmap.h"
#include "netpredict.h"
#include "netprotocol.h"
#include "netcinematic.h"
#include "netcountry.h"
#include "netteams.h"
#include "netvote.h"
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
static_assert(MAXPLAYERNAME - 1 == netscoreboard::MaxName, "Scoreboard names must fit the player profile");
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
    uint64_t              joinOrder = 0;
    uint64_t              joinedAtMillis = 0;
    uint16_t              country = 0;
    int                   team = -1;
    netvote::Cooldown     voteCooldown;
    std::string           voteIdentity;
    netcoop::LifeState     coopLives;
    bool                  coopDowned = false;
    uint8_t               coopGiveUpTicks = 0;
    uint32_t              voteRequestWindow = 0, voteRequests = 0;
};
ClientSlot                   slots[8];
struct CoopReconnect { std::string identity; netcoop::LifeState lives; bool downed; };
std::deque<CoopReconnect>     coopReconnects;
uint64_t                     nextJoinOrder = 0;
netscoreboard::State          scoreboard;
uint32_t                     scoreboardHeartbeat = 0;
bool                         scoreboardDirty = true;
netcinematic::Barrier         cinematicBarrier;
netcinematic::State           serverCinematic, clientCinematic;
std::deque<netcinematic::Descriptor> cinematicQueue, pendingOutros;
uint32_t                     nextCinematicId = 0, localCinematicId = 0, localCinematicStart = 0;
bool                         localCinematicFinished = false, localCinematicOpened = false;
netcinematic::HoldToSkip      cinematicSkip;
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
netvote::Vote                 mapVote;
NetServerVoteAction           voteAction = NetServerVoteAction::Restart;
size_t                       voteMapIndex = 0;
uint32_t                     voteId = 0, voteCaller = 0, voteHeartbeat = 0;
netwire::VoteState            voteState = netwire::VoteState::Idle;
std::string                  voteTarget;
netwire::VoteStatusPayload    clientVote;
netwire::VoteOptionsPayload   clientVoteOptions;
uint32_t                     clientVoteReceived = 0;
struct VoteCooldownEntry { std::string identity; netvote::Cooldown cooldown; };
std::deque<VoteCooldownEntry> voteCooldowns;
uint32_t                     clientMapSize = 0, clientMapProgressTime = 0;
bool                         clientDownloadingMap = false, mapDownloadDisconnected = false;
std::string                  mapDownloadError;
bool                         clientVersionReceived = false;
std::vector<std::string>      clientVersionError;
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
bool sendConnectionPacket(ENetPeer *peer, Bytes wire)
{
    if (!peer || wire.empty()) return false;
    ENetPacket *packet = enet_packet_create(wire.data(), wire.size(), ENET_PACKET_FLAG_RELIABLE);
    if (!packet) return false;
    if (enet_peer_send(peer, ControlChannel, packet) < 0)
    {
        enet_packet_destroy(packet);
        return false;
    }
    ++sentMessages;
    return true;
}
void sendServerVersion(ENetPeer *peer)
{
    netcompat::ServerInfo info;
    info.build = s_buildRev;
    info.build.resize(std::min(info.build.size(), netcompat::MaxBuild));
    for (char& ch : info.build)
        if (static_cast<unsigned char>(ch) < 32 || static_cast<unsigned char>(ch) > 126) ch = '?';
    sendConnectionPacket(peer, netcompat::encodeServerInfo(info));
}
void rejectClientVersion(ENetPeer *peer, uint32_t clientVersion)
{
    char reason[161];
    Bsnprintf(reason, sizeof(reason), "Your client uses network version %u; this server requires %u. %s",
        clientVersion, netwire::Version, clientVersion < netwire::Version
            ? "Please update your game and reconnect."
            : "Ask the server administrator to update, or use a matching game version.");
    LOG_F(WARNING, "NET rejected client version client=%u required=%u: %s", clientVersion, netwire::Version, reason);
    // Diagnostic-only framing in the client's advertised protocol lets older
    // decoders read the rejection without accepting any older gameplay packets.
    sendConnectionPacket(peer, netcompat::encodeReject(clientVersion ? clientVersion : netwire::Version, reason));
    enet_peer_disconnect_later(peer, 1);
}
#endif
netscoreboard::State collectScoreboard()
{
    netscoreboard::State state;
    uint64_t const now = static_cast<uint64_t>(timerGetFractionalTicks());
    for (int p = 0; p < 8; ++p)
    {
        auto const& client = slots[p];
        if ((!client.active && !client.admitted) || client.pendingDrop) continue;
        netscoreboard::Row row;
        row.slot = p;
        row.generation = client.generation;
        row.name = gProfile[p].name;
        row.name.resize(std::min(row.name.size(), netscoreboard::MaxName));
        for (char& c : row.name) if (static_cast<unsigned char>(c) < 32 || c == 127) c = ' ';
        row.score = client.active ? gPlayer[p].fragCount : 0;
        row.livesRemaining = netCoopLifeLimit() ? client.coopLives.remaining() : -1;
        row.downed = netCoopLifeLimit() && client.coopDowned;
        row.giveUpTicks = row.downed ? client.coopGiveUpTicks : 0;
        row.team = client.active && gGameOptions.nGameType == kGameTypeTeams ? gPlayer[p].teamId & 1 : netscoreboard::NoTeam;
        row.elapsedMinutes = netscoreboard::joinedMinutes(now, client.joinedAtMillis);
        if (client.country)
        {
            row.country += static_cast<char>(client.country >> 8);
            row.country += static_cast<char>(client.country & 255);
        }
#ifndef NETCODE_DISABLE
        if (client.peer) row.pingMs = std::min(client.peer->roundTripTime, netscoreboard::MaxPingMs);
#endif
        state.rows.push_back(row);
    }
    return state;
}
void sendScoreboard(int p)
{
    scoreboard = collectScoreboard();
#ifndef NETCODE_DISABLE
    if (p >= 0 && p < 8 && slots[p].peer)
    {
        netwire::Writer writer;
        if (netscoreboard::writeState(writer, scoreboard)) sendTo(slots[p].peer, netwire::Scoreboard, writer.data);
    }
#else
    UNREFERENCED_PARAMETER(p);
#endif
}
void updateScoreboard()
{
    if (!netIsServer() || !gGameStarted || !ready2send || gStartNewGame || !mapGeneration) return;
    uint32_t const now = timerGetTicks();
    if (!scoreboardDirty && uint32_t(now - scoreboardHeartbeat) < 1000) return;
    scoreboard = collectScoreboard();
    netwire::Writer writer;
    if (!netscoreboard::writeState(writer, scoreboard)) return;
#ifndef NETCODE_DISABLE
    for (auto const& client : slots)
        if (client.admitted && !client.pendingDrop) sendTo(client.peer, netwire::Scoreboard, writer.data);
#endif
    if (netDemoIsRecording() && !recordNeedsWelcome && recordSlot >= 0 && slots[recordSlot].active)
        netDemoRecordPacket(serverTick, envelope(netwire::Scoreboard, writer.data));
    scoreboardHeartbeat = now;
    scoreboardDirty = false;
}
bool recordScoreboardBaseline()
{
    if (netIsServer()) scoreboard = collectScoreboard();
    netwire::Writer writer;
    if (!netscoreboard::writeState(writer, scoreboard)) return false;
    netwire::Header header;
    header.type = netwire::Scoreboard;
    header.session = session;
    header.generation = mapGeneration;
    // Sequence zero provides the current board when recording starts mid-map.
    return netDemoRecordPacket(serverTick, netwire::encode(header, writer.data));
}
const char* cinematicKind(netcinematic::Kind kind)
{
    return kind == netcinematic::Kind::Outro ? "outro" : kind == netcinematic::Kind::Map ? "map" : "intro";
}
int cinematicRemaining(const netcinematic::State& state)
{
    unsigned mask = state.participants & ~state.done;
    int count = 0;
    for (; mask; mask >>= 1) count += mask & 1;
    return count;
}
void stopLocalCinematic(bool resumeMusic = false)
{
    if (gDedicated) return;
    bool const wasActive = localCinematicId != 0;
    // A newly admitted client can draw the waiting screen before it is in
    // the participant mask. Restore that screen's palette on disconnect too.
    credCinematicEnd();
    localCinematicId = localCinematicStart = 0;
    localCinematicFinished = localCinematicOpened = false;
    cinematicSkip.reset();
    if (wasActive)
    {
        gInput = gNetInput = {};
        ctrlClearAllInput();
        if (resumeMusic && gGameStarted && !gStartNewGame)
        {
            ambInit();
            levelTryPlayMusicOrNothing(gGameOptions.nEpisode, gGameOptions.nLevel);
        }
    }
}
void showCinematicState(const netcinematic::State& state)
{
    if (clientCinematic.id != state.id || !state.active)
        stopLocalCinematic(!state.active);
    clientCinematic = state;
}
uint8_t cinematicParticipants()
{
    uint8_t mask = 0;
    for (int p = 0; p < 8; ++p)
        if (!slots[p].pendingDrop && (slots[p].active || slots[p].admitted)) mask |= 1u << p;
    return mask;
}
void sendCinematicState(int p)
{
    if (!serverCinematic.id) return;
    if (p == myconnectindex && !gDedicated)
    {
        showCinematicState(serverCinematic);
        return;
    }
#ifndef NETCODE_DISABLE
    netwire::Writer w;
    if (netcinematic::writeState(w, serverCinematic))
        sendTo(slots[p].peer, netwire::CinematicState, w.data);
#endif
}
void publishCinematicState()
{
    LOG_F(INFO, "NET cinematic id=%u state=%s kind=%s participants=%u done=%u remaining=%d",
          serverCinematic.id, serverCinematic.active ? "active" : "end", cinematicKind(serverCinematic.descriptor.kind),
          unsigned(serverCinematic.participants), unsigned(serverCinematic.done), cinematicRemaining(serverCinematic));
    for (int p = 0; p < 8; ++p)
        if (slots[p].active || slots[p].admitted) sendCinematicState(p);
}
void startQueuedCinematic(uint8_t participants)
{
    serverCinematic = {};
    if (!++nextCinematicId) ++nextCinematicId;
    serverCinematic.id = nextCinematicId;
    serverCinematic.active = true;
    serverCinematic.participants = participants;
    serverCinematic.descriptor = std::move(cinematicQueue.front());
    cinematicQueue.pop_front();
    cinematicBarrier.start(serverCinematic.id, participants);
}
void updateServerCinematic()
{
    if (!netIsServer() || !serverCinematic.active || gStartNewGame) return;
    uint8_t const participants = cinematicParticipants();
    cinematicBarrier.updateParticipants(participants);
    bool changed = serverCinematic.participants != participants || serverCinematic.done != cinematicBarrier.done();
    serverCinematic.participants = participants;
    serverCinematic.done = cinematicBarrier.done();
    if (cinematicBarrier.complete())
    {
        if (!cinematicQueue.empty() && participants)
            startQueuedCinematic(participants);
        else
        {
            cinematicQueue.clear();
            cinematicBarrier.reset();
            serverCinematic.active = false;
            serverCinematic.participants = serverCinematic.done = 0;
            nextTickTime = timerGetTicks();
            lastSnapshotTick = 0;
            for (auto& c : slots) { c.commands = {}; c.lastInput = {}; c.lastInputTime = 0; }
        }
        changed = true;
    }
    if (changed) publishCinematicState();
}
std::string cinematicRelativePath(std::string path)
{
    std::replace(path.begin(), path.end(), '\\', '/');
    while (path.compare(0, 2, "./") == 0) path.erase(0, 2);
    if ((!path.empty() && path[0] == '/') || path.find(':') != std::string::npos)
        path.erase(0, path.find_last_of('/') + 1);
    return path;
}
void appendCinematic(std::deque<netcinematic::Descriptor>& queue, const LevelCinematic& scene, netcinematic::Kind kind)
{
    netcinematic::Descriptor descriptor;
    descriptor.video = cinematicRelativePath(scene.video);
    descriptor.audio = cinematicRelativePath(scene.audio);
    descriptor.audioResource = scene.audioResource;
    descriptor.kind = kind;
    if (netcinematic::validDescriptor(descriptor)) queue.push_back(std::move(descriptor));
    else LOG_F(WARNING, "NET ignoring invalid cinematic path: %s", scene.video.c_str());
}
void finishLocalCinematic(const char* reason)
{
    localCinematicFinished = true;
    localCinematicOpened = false;
    credCinematicEnd();
    cinematicSkip.reset();
    LOG_F(INFO, "NET cinematic local id=%u state=waiting reason=%s", localCinematicId, reason);
    if (netIsServer())
        cinematicBarrier.markDone(localCinematicId, myconnectindex);
#ifndef NETCODE_DISABLE
    else if (!demoPlayback)
    {
        netwire::Writer w;
        netcinematic::Done done;
        done.slotGeneration = clientSlotGeneration;
        done.id = localCinematicId;
        if (netcinematic::writeDone(w, done)) sendTo(serverPeer, netwire::CinematicDone, w.data);
    }
#endif
}
int serverAutoAim()
{
    return gDedicated ? netServerSettings().autoAim : ClipRange(gAutoAim, 0, 3);
}
int serverPlayerSkill()
{
    return gGameOptions.nGameType == kGameTypeCoop ? int(gGameOptions.nDifficulty) : 2;
}
void ensureServerPlayerName(int player)
{
    if (gProfile[player].name[0]) return;
    static_assert(kMaxPlayers <= 100, "Automatic player names must cover every slot");
    for (int number = 0; number < 100; ++number)
    {
        char candidate[8];
        Bsnprintf(candidate, sizeof(candidate), "Caleb%02d", number);
        bool available = true;
        for (int other = 0; other < kMaxPlayers; ++other)
        {
            auto const& client = slots[other];
            // Reserve names during content checks and map loading as well as
            // gameplay. Unused slots can still contain an old profile.
            if (other == player || client.pendingDrop
                || !(client.active || client.admitted || client.contentOffered)) continue;
            if (!Bstrcasecmp(candidate, gProfile[other].name))
            {
                available = false;
                break;
            }
        }
        if (!available) continue;
        Bstrncpyz(gProfile[player].name, candidate, sizeof(gProfile[player].name));
        scoreboardDirty = true;
        LOG_F(INFO, "NET assigned player name slot=%d name=%s", player, candidate);
        return;
    }
}

void enforceServerProfiles()
{
    for (auto& profile : gProfile)
    {
        profile.nAutoAim = serverAutoAim();
        profile.skill = serverPlayerSkill();
    }
}
void balanceTeams()
{
    if (!netIsServer() || gGameOptions.nGameType != kGameTypeTeams || !ready2send || gStartNewGame)
        return;
    netteams::Roster roster{};
    for (int p = 0; p < 8; ++p)
    {
        if (!slots[p].active || slots[p].pendingDrop) continue;
        roster[p].active = true;
        roster[p].team = unsigned(gPlayer[p].teamId);
        roster[p].joinOrder = slots[p].joinOrder;
        slots[p].team = gPlayer[p].teamId;
    }
    for (int p; (p = netteams::selectBalancePlayer(roster)) >= 0;)
    {
        int const previous = int(roster[p].team), team = 1 - previous;
        // Select by join age before checking death: wait for this newest
        // player, never substitute an older teammate who died first.
        // Rebuilding the roster each tick cancels/reselects a pending move
        // when joins or departures change which team needs balancing.
        if (!playerChangeTeam(&gPlayer[p], team)) break;
        roster[p].team = unsigned(team);
        slots[p].team = team;
        lastSnapshotTick = 0;
        LOG_F(INFO, "NET team balance slot=%d from=%d to=%d join_order=%llu", p, previous, team,
              (unsigned long long)slots[p].joinOrder);
        char message[160];
        Bsnprintf(message, sizeof(message), "%s moved to %s team to balance teams", gProfile[p].name, team ? "Red" : "Blue");
        viewSetMessage(message, 10);
    }
}
netwire::ServerRules serverRules()
{
    netwire::ServerRules rules;
    rules.maxPlayers = gInitialNetPlayers;
    rules.autoAim = serverAutoAim();
    rules.mapVoteEnabled = netServerSettings().mapVoteEnabled;
    if (gDedicated)
    {
        auto const& settings = netServerSettings();
        rules.timeLimitMinutes = settings.timeLimitMinutes;
        rules.scoreLimit = settings.scoreLimit;
        rules.rotationIndex = uint32_t(settings.rotationIndex);
        rules.rotationCount = uint32_t(settings.rotationCount);
        if (gGameOptions.nGameType == kGameTypeCoop)
            rules.coopLives = settings.coopLives;
        if (gGameOptions.nGameType == kGameTypeBloodBath)
        {
            rules.allowHolsterWeapon = settings.holsterWeapon;
            rules.allowChaseView = settings.chaseView;
        }
    }
    return rules;
}
const char* voteActionName(uint32_t action)
{
    return action == 1 ? "map" : action == 2 ? "restart" : action == 3 ? "next" : "none";
}
bool readyVoter(int p)
{
    if (!gGameStarted || !ready2send || p < 0 || p >= 8 || !slots[p].active || !slots[p].ready || slots[p].pendingDrop) return false;
    if (!gDedicated && p == myconnectindex) return true;
#ifndef NETCODE_DISABLE
    return slots[p].peer != nullptr;
#else
    return false;
#endif
}
netwire::VoteStatusPayload voteStatus(int recipient)
{
    uint32_t const now = timerGetTicks();
    netwire::VoteStatusPayload status;
    status.state = voteState;
    if (voteState != netwire::VoteState::Idle)
    {
        status.id = voteId;
        status.action = voteAction == NetServerVoteAction::Map ? 1 : voteAction == NetServerVoteAction::Restart ? 2 : 3;
        status.target = voteTarget;
        status.caller = voteCaller;
        status.remainingMs = mapVote.remaining(now);
        status.eligible = mapVote.eligibleCount();
        status.yes = mapVote.yesCount();
        status.no = mapVote.noCount();
        status.required = mapVote.requiredCount();
        if (recipient >= 0 && recipient < 8)
        {
            status.voted = mapVote.hasVoted(recipient, slots[recipient].generation);
            status.canVote = mapVote.active() && readyVoter(recipient) && !status.voted
                && mapVote.eligible(recipient, slots[recipient].generation) && status.remainingMs;
        }
    }
    if (recipient >= 0 && recipient < 8) status.cooldownMs = slots[recipient].voteCooldown.remaining(now);
    return status;
}
netwire::VoteOptionsPayload voteOptions(bool requested)
{
    netwire::VoteOptionsPayload options;
    options.enabled = netServerSettings().mapVoteEnabled;
    options.listRequested = requested;
    options.maps = netServerVoteMaps();
    if (!options.maps.empty()) options.actions |= 1;
    if (options.enabled && gGameOptions.nGameType == kGameTypeCoop)
    {
        options.actions |= 2;
        size_t index = 0;
        std::string error;
        if (netServerResolveVote(NetServerVoteAction::Next, "", index, error)) options.actions |= 4;
    }
    return options;
}
void logVoteStatus(const netwire::VoteStatusPayload& status)
{
    LOG_F(INFO, "NET vote active=%d id=%u eligible=%u yes=%u no=%u required=%u remaining_ms=%u action=%s target=%s can_vote=%u voted=%u cooldown_ms=%u",
          int(status.state == netwire::VoteState::Active), status.id, status.eligible, status.yes, status.no,
          status.required, status.remainingMs, voteActionName(status.action), status.target.c_str(), status.canVote, status.voted, status.cooldownMs);
}
void showVoteStatus(const netwire::VoteStatusPayload& status)
{
    bool const changed = status.id != clientVote.id || status.state != clientVote.state
        || status.yes != clientVote.yes || status.no != clientVote.no;
    clientVote = status;
    clientVoteReceived = timerGetTicks();
    if (changed || !status.notice.empty()) logVoteStatus(status);
    if (status.rejected) LOG_F(INFO, "NET vote rejected: %s", status.notice.c_str());
    if (!status.notice.empty()) viewSetMessage(status.notice.c_str(), 10);
    if (changed && status.state >= netwire::VoteState::Passed)
    {
        const char* result = status.state == netwire::VoteState::Passed ? "passed" : status.state == netwire::VoteState::Failed ? "failed" : "cancelled";
        LOG_F(INFO, "NET vote result=%s id=%u eligible=%u yes=%u no=%u required=%u", result, status.id,
              status.eligible, status.yes, status.no, status.required);
        if (status.notice.empty())
        {
            char message[128];
            Bsnprintf(message, sizeof(message), "Map vote %s: %u yes, %u no (%u yes required)", result, status.yes, status.no, status.required);
            viewSetMessage(message, 10);
        }
    }
}
void showVoteOptions(const netwire::VoteOptionsPayload& options)
{
    clientVoteOptions = options;
    if (!options.listRequested) return;
    LOG_F(INFO, "NET vote options enabled=%u actions=%u maps=%u", options.enabled, options.actions, unsigned(options.maps.size()));
    if (!options.enabled) OSD_Printf("Map voting is disabled by the server.\n");
    else if (options.actions & 2) OSD_Printf("callvote restart%s\n", options.actions & 4 ? " or callvote next" : " (no next level available)");
    else if (options.maps.empty()) OSD_Printf("Map voting requires a server map rotation.\n");
    for (size_t n = 0; n < options.maps.size(); ++n)
        LOG_F(INFO, "NET vote map index=%u name=%s", unsigned(n), options.maps[n].c_str());
}
void sendVoteStatus(int p, const std::string& notice = {}, bool rejected = false)
{
    netwire::VoteStatusPayload status = voteStatus(p);
    status.notice = notice.substr(0, 160);
    status.rejected = rejected;
    if (!gDedicated && p == myconnectindex) showVoteStatus(status);
#ifndef NETCODE_DISABLE
    else if (p >= 0 && p < 8 && slots[p].peer)
    {
        netwire::Writer w;
        if (netwire::writeVoteStatus(w, status)) sendTo(slots[p].peer, netwire::VoteStatus, w.data);
    }
#endif
}
void sendVoteOptions(int p, bool requested = false)
{
    auto options = voteOptions(requested);
    if (!gDedicated && p == myconnectindex) showVoteOptions(options);
#ifndef NETCODE_DISABLE
    else if (p >= 0 && p < 8 && slots[p].peer)
    {
        netwire::Writer w;
        if (netwire::writeVoteOptions(w, options)) sendTo(slots[p].peer, netwire::VoteOptions, w.data);
    }
#endif
}
void broadcastVote(const std::string& notice = {})
{
    logVoteStatus(voteStatus(-1));
    for (int p = 0; p < 8; ++p) if (readyVoter(p)) sendVoteStatus(p, notice);
    if (netDemoIsRecording() && !recordNeedsWelcome && recordSlot >= 0 && slots[recordSlot].active)
    {
        auto status = voteStatus(recordSlot);
        status.notice = notice.substr(0, 160);
        netwire::Writer w;
        if (netwire::writeVoteStatus(w, status)) netDemoRecordPacket(serverTick, envelope(netwire::VoteStatus, w.data));
    }
    voteHeartbeat = timerGetTicks();
}
void rejectVote(int p, const std::string& reason)
{
    LOG_F(INFO, "NET vote rejected: %s", reason.c_str());
    sendVoteStatus(p, reason, true);
}
void rememberVoteCooldown(int p)
{
    uint32_t const now = timerGetTicks();
    auto const& identity = slots[p].voteIdentity;
    for (auto it = voteCooldowns.begin(); it != voteCooldowns.end();)
        if (it->cooldown.ready(now) || it->identity == identity) it = voteCooldowns.erase(it);
        else ++it;
    if (identity.empty()) return; // Local listen-server player stays in its slot.
    if (voteCooldowns.size() >= 128) voteCooldowns.pop_front();
    voteCooldowns.push_back({identity, slots[p].voteCooldown});
}
void cancelVote(const char* reason)
{
    if (!mapVote.active()) return;
    voteState = netwire::VoteState::Cancelled;
    LOG_F(INFO, "NET vote result=cancelled id=%u eligible=%u yes=%u no=%u required=%u", voteId,
          mapVote.eligibleCount(), mapVote.yesCount(), mapVote.noCount(), mapVote.requiredCount());
    // End its clock before serializing the terminal state; retain the tally.
    mapVote.cancel(true);
    broadcastVote(reason);
}
void updateVote()
{
    if (!netIsServer() || !mapVote.active()) return;
    if (gStartNewGame || (gGameOptions.uGameFlags & kGameFlagContinuing))
    {
        cancelVote("Map vote cancelled because the server is changing levels");
        return;
    }
    uint32_t const now = timerGetTicks();
    auto const result = mapVote.poll(now);
    if (result != netvote::Result::NoResult)
    {
        std::string error;
        bool const passed = result == netvote::Result::Passed;
        bool const applied = passed && netServerApplyVote(voteAction, voteMapIndex, error);
        voteState = applied ? netwire::VoteState::Passed : passed ? netwire::VoteState::Cancelled : netwire::VoteState::Failed;
        const char* outcome = applied ? "passed" : passed ? "cancelled" : "failed";
        LOG_F(INFO, "NET vote result=%s id=%u eligible=%u yes=%u no=%u required=%u", outcome, voteId,
              mapVote.eligibleCount(), mapVote.yesCount(), mapVote.noCount(), mapVote.requiredCount());
        std::string notice = "Map vote "; notice += outcome;
        if (!error.empty()) notice += ": " + error;
        broadcastVote(notice);
    }
    else if (uint32_t(now - voteHeartbeat) >= 1000) broadcastVote();
}
void handleVoteRequest(int p, const netwire::VoteRequestPayload& request)
{
    updateVote(); // Expire before accepting a ballot from this service batch.
    if (!readyVoter(p)) { rejectVote(p, "Join the current map before voting"); return; }
    if (request.command == netwire::VoteCommand::List) { sendVoteOptions(p, true); return; }
    if (request.command == netwire::VoteCommand::QueryStatus) { sendVoteStatus(p, "Map vote status"); return; }
    if (!netServerSettings().mapVoteEnabled) { rejectVote(p, "Map voting is disabled by the server"); return; }
    if (gStartNewGame || (gGameOptions.uGameFlags & kGameFlagContinuing))
    { rejectVote(p, "The server is changing levels"); return; }
    uint32_t const now = timerGetTicks();
    if (request.command == netwire::VoteCommand::Yes || request.command == netwire::VoteCommand::No)
    {
        if (!mapVote.active() || request.id != voteId) { rejectVote(p, "No matching map vote is active"); return; }
        if (!mapVote.eligible(p, slots[p].generation)) { rejectVote(p, "You joined after this vote started and cannot vote in it"); return; }
        if (mapVote.hasVoted(p, slots[p].generation)) { rejectVote(p, "You have already voted"); return; }
        if (!mapVote.cast(now, p, slots[p].generation, request.command == netwire::VoteCommand::Yes))
        { rejectVote(p, "The voting period has ended"); return; }
        broadcastVote();
        return;
    }
    if (mapVote.active()) { rejectVote(p, "Another map vote is already in progress"); return; }
    if (!slots[p].voteCooldown.ready(now))
    {
        char reason[128];
        Bsnprintf(reason, sizeof(reason), "Wait %u seconds before calling another map vote", (slots[p].voteCooldown.remaining(now) + 999) / 1000);
        rejectVote(p, reason); return;
    }
    NetServerVoteAction action = request.command == netwire::VoteCommand::Map ? NetServerVoteAction::Map
        : request.command == netwire::VoteCommand::Restart ? NetServerVoteAction::Restart : NetServerVoteAction::Next;
    size_t index = 0;
    std::string error;
    if (!netServerResolveVote(action, request.map, index, error)) { rejectVote(p, error); return; }
    netvote::Roster roster{};
    for (int n = 0; n < 8; ++n) if (readyVoter(n)) roster[n] = slots[n].generation;
    if (!mapVote.start(now, roster, p, slots[p].generation)) { rejectVote(p, "Could not start map vote"); return; }
    voteAction = action;
    voteMapIndex = index;
    voteCaller = p;
    if (!++voteId) voteId = 1;
    voteState = netwire::VoteState::Active;
    voteTarget = action == NetServerVoteAction::Map ? netServerVoteMaps()[index] : action == NetServerVoteAction::Restart ? "current level" : "next level";
    slots[p].voteCooldown.accept(now);
    rememberVoteCooldown(p);
    broadcastVote("Map vote started (30 seconds; use vote yes or vote no)");
}
bool recordVoteBaseline()
{
    auto status = netIsServer() ? voteStatus(recordSlot) : clientVote;
    auto options = netIsServer() ? voteOptions(false) : clientVoteOptions;
    options.listRequested = 0;
    status.notice.clear(); status.rejected = 0;
    if (!netIsServer())
    {
        uint32_t const elapsed = timerGetTicks() - clientVoteReceived;
        status.remainingMs -= std::min(elapsed, status.remainingMs);
        status.cooldownMs -= std::min(elapsed, status.cooldownMs);
    }
    netwire::Writer a, b;
    if (!netwire::writeVoteOptions(a, options) || !netwire::writeVoteStatus(b, status)) return false;
    netwire::Header h; h.session = session; h.generation = mapGeneration;
    h.type = netwire::VoteOptions;
    if (!netDemoRecordPacket(serverTick, netwire::encode(h, a.data))) return false;
    h.type = netwire::VoteStatus;
    return netDemoRecordPacket(serverTick, netwire::encode(h, b.data));
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
void initializeCoopLives(int p)
{
    auto& client = slots[p];
    client.coopLives.reset(netCoopLifeLimit());
    client.coopDowned = false;
    client.coopGiveUpTicks = 0;
    if (!netCoopLifeLimit() || client.voteIdentity.empty()) return;
    for (auto it = coopReconnects.begin(); it != coopReconnects.end(); ++it)
        if (it->identity == client.voteIdentity)
        {
            client.coopLives = it->lives;
            client.coopDowned = it->downed;
            coopReconnects.erase(it);
            LOG_F(INFO, "NET coop rejoin slot=%d lives=%d", p, client.coopLives.remaining());
            break;
        }
}
void rememberCoopLives(int p)
{
    auto const& client = slots[p];
    if (!netCoopLifeLimit() || !client.admitted || client.voteIdentity.empty()) return;
    // As with map-vote cooldowns, reconnecting with the same address/name must
    // not replenish the budget. Forget this ledger on every new level.
    for (auto it = coopReconnects.begin(); it != coopReconnects.end();)
        if (it->identity == client.voteIdentity) it = coopReconnects.erase(it);
        else ++it;
    if (coopReconnects.size() >= 128) coopReconnects.pop_front();
    coopReconnects.push_back({client.voteIdentity, client.coopLives, client.coopDowned});
}
void removePlayer(int p)
{
    if (p < 0 || p >= 8)
        return;
    rememberCoopLives(p);
    if (slots[p].active && gGameStarted && gPlayer[p].pSprite)
    {
        seqKill(3, gPlayer[p].pSprite->extra);
        actPostSprite(gPlayer[p].nSprite, kStatFree);
    }
    uint32_t generation = slots[p].generation;
    // Clear acknowledgement ownership before this slot can be reused.
    cinematicBarrier.updateParticipants(cinematicBarrier.participants() & ~(1u << p));
    slots[p]            = ClientSlot();
    slots[p].generation = generation;
    scoreboardDirty = true;
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
        else
            LOG_F(INFO, "NET bootstrap deferred slot=%d started=%d ready=%d changing=%d generation=%u",
                  p, int(gGameStarted), int(ready2send), int(gStartNewGame), mapGeneration);
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
    if (!sendTo(slots[p].peer, netwire::Welcome, w.data))
        LOG_F(ERROR, "NET bootstrap send failed slot=%d map_generation=%u", p, mapGeneration);
    else
        LOG_F(INFO, "NET bootstrap sent slot=%d map_generation=%u", p, mapGeneration);
    slots[p].ready       = false;
    slots[p].pendingReady = false;
    sendVoteOptions(p);
    sendVoteStatus(p);
    sendCinematicState(p);
    sendScoreboard(p);
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
        return recordLoopBaseline() && recordVoteBaseline() && recordScoreboardBaseline();
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
void failClientVersion(uint32_t required, const std::string &build)
{
    // Tear down outside the ENet service loop, then retain the error on the
    // pre-game screen until the user starts another connection or leaves it.
    netDisconnect();
    gStartNewGame = false;
    char detail[128];
    Bsnprintf(detail, sizeof(detail), "Your network version: %u. Server requires: %u.", netwire::Version, required);
    if (!required)
        clientVersionError = {"Invalid server version information", "Please update the server and reconnect."};
    else if (netwire::Version < required)
        clientVersionError = {"Your game version is too old", detail, "Please update your game and reconnect."};
    else
        clientVersionError = {"The server uses an older game version", detail,
            "Ask the server administrator to update,", "or use a matching game version."};
    LOG_F(ERROR, "NET version mismatch client=%u required=%u build=%s: %s", netwire::Version, required,
        build.c_str(), clientVersionError.back().c_str());
    viewSetErrorMessage(clientVersionError.front().c_str());
}
bool acceptClientPacket(const uint8_t *data, size_t size, bool replay)
{
    if (!replay && netcompat::isServerInfo(data, size))
    {
        if (clientVersionReceived || clientContentChecked || clientSnapshotReady) return false;
        netcompat::ServerInfo info;
        if (!netcompat::decodeServerInfo(data, size, info))
        {
            ++malformedMessages;
            failClientVersion(0, {});
            return false;
        }
        LOG_F(INFO, "NET server version required=%u client=%u build=%s", info.requiredProtocol, netwire::Version, info.build.c_str());
        if (info.requiredProtocol != netwire::Version)
        {
            failClientVersion(info.requiredProtocol, info.build);
            return false;
        }
        clientVersionReceived = true;
        return true;
    }
    if (!replay && !clientVersionReceived && !clientContentChecked && !clientSnapshotReady)
    {
        uint32_t protocol = 0;
        std::string reason;
        if (netcompat::decodeReject(data, size, protocol, reason) && protocol != netwire::Version)
        {
            LOG_F(ERROR, "NET rejected by server version=%u: %s", protocol, reason.c_str());
            failClientVersion(protocol, {});
            return false;
        }
    }
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
        stopLocalCinematic();
        clientCinematic = {};
        mapGeneration        = h.generation;
        clientSlotGeneration = slotGeneration;
        clientMapCRC         = crc;
        clientServerRules    = rules;
        clientVote = {};
        clientVoteOptions = {};
        scoreboard = {};
        clientVoteReceived = timerGetTicks();
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
    bool syntheticBaseline = replay && (h.type == netwire::Event || h.type == netwire::VoteStatus || h.type == netwire::VoteOptions
        || h.type == netwire::Scoreboard) && h.sequence == 0;
    if (!syntheticBaseline && clientReceived[h.type] && !netwire::newer(h.sequence, clientReceived[h.type]))
    {
        duplicateMessages++;
        return true;
    }
    if (h.type == netwire::Scoreboard)
    {
        if (!netscoreboard::readState(r, scoreboard)) { malformedMessages++; return false; }
        if (!syntheticBaseline) clientReceived[h.type] = h.sequence;
        if (netDemoIsRecording() && !replay) netDemoRecordPacket(serverTick, Bytes(data, data + size));
        return true;
    }
    if (!syntheticBaseline)
        clientReceived[h.type] = h.sequence;
    if (h.type == netwire::VoteStatus)
    {
        netwire::VoteStatusPayload status;
        if (!netwire::readVoteStatus(r, status)) return false;
        showVoteStatus(status);
        if (netDemoIsRecording() && !replay) netDemoRecordPacket(serverTick, Bytes(data, data + size));
        return true;
    }
    if (h.type == netwire::CinematicState)
    {
        netcinematic::State state;
        if (replay || !netcinematic::readState(r, state)) return false;
        showCinematicState(state);
        // Movies and the remote-viewer barrier are not simulation packets.
        // Packet demos preserve gameplay while omitting these frozen intervals.
        return true;
    }
    if (h.type == netwire::VoteOptions)
    {
        netwire::VoteOptionsPayload options;
        if (!netwire::readVoteOptions(r, options)) return false;
        showVoteOptions(options);
        if (netDemoIsRecording() && !replay) netDemoRecordPacket(serverTick, Bytes(data, data + size));
        return true;
    }
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
        ensureServerPlayerName(slot);
        // A profile is not an authority to override server gameplay rules.
        gProfile[slot].nAutoAim      = serverAutoAim();
        gProfile[slot].nWeaponSwitch = weapon;
        gProfile[slot].skill         = serverPlayerSkill();
        // Rejoining with the same endpoint/name retains the recent cooldown.
        // Ports are intentionally excluded: reconnecting chooses a new port.
        c.voteIdentity.assign(reinterpret_cast<const char*>(&peer->address.host), sizeof(peer->address.host));
        std::string voteName = name;
        for (char& ch : voteName) if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
        c.voteIdentity += voteName;
        for (auto const& entry : voteCooldowns)
            if (entry.identity == c.voteIdentity && !entry.cooldown.ready(timerGetTicks())) c.voteCooldown = entry.cooldown;
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
        initializeCoopLives(slot);
        c.joinedAtMillis = static_cast<uint64_t>(timerGetFractionalTicks());
        scoreboardDirty = true;
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
        initializeCoopLives(slot);
        c.joinedAtMillis = static_cast<uint64_t>(timerGetFractionalTicks());
        scoreboardDirty = true;
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
        if (!c.active && !c.joinOrder) c.joinOrder = ++nextJoinOrder;
        c.pendingReady = true;
    }
    else if (h.type == netwire::CinematicDone)
    {
        netcinematic::Done done;
        if (!netcinematic::readDone(r, done) || done.slotGeneration != c.generation)
        {
            malformedMessages++;
            return;
        }
        if ((c.ready || c.pendingReady) && serverCinematic.active)
        {
            cinematicBarrier.updateParticipants(cinematicParticipants());
            cinematicBarrier.markDone(done.id, slot);
        }
    }
    else if (h.type == netwire::SnapshotAck)
    {
        uint32_t id = r.u32();
        if (r.end() && (!id || findSnapshot(id)))
            c.snapshotAck = id;
    }
    else if (h.type == netwire::Commands && c.ready)
    {
        if (serverCinematic.active) return;
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
    else if (h.type == netwire::VoteRequest)
    {
        uint32_t const now = timerGetTicks();
        if (uint32_t(now - c.voteRequestWindow) >= 1000) { c.voteRequestWindow = now; c.voteRequests = 0; }
        if (++c.voteRequests > 8) return;
        netwire::VoteRequestPayload request;
        if (!netwire::readVoteRequest(r, request)) { malformedMessages++; return; }
        handleVoteRequest(slot, request);
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
bool netGetPresenceInfo(NetPresenceInfo& info)
{
    info = {};
#ifndef NETCODE_DISABLE
    if (!initialized || !host || demoPlayback || !gGameStarted || !session)
        return false;
    info.server = netIsServer();
    if (info.server)
    {
        if (gDedicated || !mapGeneration || !ready2send) return false;
        info.maxPlayers = gInitialNetPlayers;
        info.port = static_cast<uint16_t>(gNetPort);
    }
    else
    {
        if (!serverPeer || serverPeer->state != ENET_PEER_STATE_CONNECTED || !clientLoaded || !clientSnapshotReady)
            return false;
        char address[64];
        if (enet_address_get_host_ip(&serverPeer->address, address, sizeof(address)) != 0)
            return false;
        // ENet uses a dual-stack IPv6 socket, including IPv4-mapped addresses.
        const unsigned char* bytes = serverPeer->address.host.s6_addr;
        bool mapped = bytes[10] == 255 && bytes[11] == 255;
        for (int n = 0; n < 10; ++n) mapped &= bytes[n] == 0;
        if (mapped)
            Bsnprintf(address, sizeof(address), "%u.%u.%u.%u", bytes[12], bytes[13], bytes[14], bytes[15]);
        info.address = address;
        info.port = serverPeer->address.port;
        info.maxPlayers = static_cast<int>(clientServerRules.maxPlayers);
    }
    info.session = session;
    info.players = numplayers;
    return info.players > 0 && info.maxPlayers >= info.players;
#else
    return false;
#endif
}
bool netAllowHolsterWeapon()
{
    if (!netIsLive() || gGameOptions.nGameType != kGameTypeBloodBath) return true;
    return (netIsServer() ? serverRules() : clientServerRules).allowHolsterWeapon != 0;
}
bool netAllowChaseView()
{
    if (!netIsLive() || gGameOptions.nGameType != kGameTypeBloodBath) return true;
    return (netIsServer() ? serverRules() : clientServerRules).allowChaseView != 0;
}
int netCoopLifeLimit()
{
    if (!netIsLive() || gGameOptions.nGameType != kGameTypeCoop) return 0;
    return netIsServer() ? (gDedicated ? netServerSettings().coopLives : 0) : int(clientServerRules.coopLives);
}
int netCoopLivesRemaining(int p)
{
    int const limit = netCoopLifeLimit();
    if (!limit || p < 0 || p >= kMaxPlayers) return -1;
    if (netIsServer()) return slots[p].coopLives.remaining();
    for (auto const& row : scoreboard.rows)
        if (row.slot == p) return row.livesRemaining;
    return limit; // The reliable roster metadata follows the map bootstrap.
}
bool netCoopPlayerIsSpectator(int p)
{
    return netCoopLifeLimit() && netCoopLivesRemaining(p) == 0;
}
bool netCoopPlayerIsDowned(int p)
{
    if (!netCoopLifeLimit() || p < 0 || p >= kMaxPlayers) return false;
    if (netIsServer()) return slots[p].coopDowned;
    for (auto const& row : scoreboard.rows)
        if (row.slot == p) return row.downed;
    return false;
}
int netCoopGiveUpProgress(int p)
{
    if (!netCoopPlayerIsDowned(p)) return 0;
    if (netIsServer()) return int(slots[p].coopGiveUpTicks) * 100 / 90;
    for (auto const& row : scoreboard.rows)
        if (row.slot == p) return int(row.giveUpTicks) * 100 / 90;
    return 0;
}
void netOnPlayerDowned(int p)
{
    if (!netIsServer() || !netCoopLifeLimit() || p < 0 || p >= kMaxPlayers || !slots[p].active
        || slots[p].coopLives.exhausted()) return;
    slots[p].coopDowned = true;
    slots[p].coopGiveUpTicks = 0;
    scoreboardDirty = true;
    LOG_F(INFO, "NET coop downed slot=%d lives=%d", p, slots[p].coopLives.remaining());
}
bool netProcessCoopDownedInput(int p, bool useHeld)
{
    if (!netIsServer() || !netCoopPlayerIsDowned(p)) return false;
    auto& client = slots[p];
    if (!useHeld)
    {
        scoreboardDirty |= client.coopGiveUpTicks != 0;
        client.coopGiveUpTicks = 0;
        return false;
    }
    ++client.coopGiveUpTicks;
    // Progress is reliable and recorded, with at most ten updates per second.
    scoreboardDirty |= client.coopGiveUpTicks % 3 == 0;
    if (client.coopGiveUpTicks < 90) return false;
    client.coopDowned = false;
    client.coopGiveUpTicks = 0;
    scoreboardDirty = true;
    return true;
}
void netOnPlayerRevived(int p, int rescuer)
{
    if (!netIsServer() || !netCoopPlayerIsDowned(p)) return;
    slots[p].coopDowned = false;
    slots[p].coopGiveUpTicks = 0;
    scoreboardDirty = true;
    LOG_F(INFO, "NET coop revived slot=%d rescuer=%d lives=%d", p, rescuer, slots[p].coopLives.remaining());
    netMessageForPlayer(p, "Revived by a teammate. No life lost.");
    netMessageForPlayer(rescuer, "Teammate revived.");
}
void netOnPlayerDeath(int p)
{
    if (!netIsServer() || !netCoopLifeLimit() || p < 0 || p >= kMaxPlayers || !slots[p].active
        || !slots[p].coopLives.die()) return;
    slots[p].coopDowned = false;
    slots[p].coopGiveUpTicks = 0;
    scoreboardDirty = true;
    int const remaining = slots[p].coopLives.remaining();
    LOG_F(INFO, "NET coop death slot=%d lives=%d spectator=%d", p, remaining, int(remaining == 0));
    netMessageForPlayer(p, remaining ? "Press Use to respawn."
        : "Spectating until the next level or restart.");
}
void netOnPlayerSpawned(int p)
{
    if (!netIsServer() || !netCoopLifeLimit() || p < 0 || p >= kMaxPlayers || !slots[p].active) return;
    bool const wasDowned = slots[p].coopDowned;
    slots[p].coopDowned = false;
    slots[p].coopGiveUpTicks = 0;
    if (!slots[p].coopLives.respawn() || wasDowned)
    {
        // Reconnects retain incapacitation/exhaustion and still need a valid
        // player sprite in the authoritative snapshot roster.
        playerDamageSprite(gPlayer[p].nSprite, &gPlayer[p], kDamageFall, 65535);
    }
    scoreboardDirty = true;
}
bool netClientHasSnapshot()
{ return clientSnapshotReady; }
const netscoreboard::State& netGetScoreboard()
{ return scoreboard; }

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
        {
            slots[0].active = slots[0].ready = true;
            slots[0].generation = 1;
            slots[0].joinOrder = ++nextJoinOrder;
            slots[0].joinedAtMillis = static_cast<uint64_t>(timerGetFractionalTicks());
            netBroadcastPlayerInfo(0);
        }
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
    stopLocalCinematic();
    serverCinematic = clientCinematic = {};
    cinematicQueue.clear();
    pendingOutros.clear();
    cinematicBarrier.reset();
    nextCinematicId = 0;
    netStopPacketRecording();
    netDemoStopPlayback();
    demoPlayback = false;
    serverInTick = false;
    nextJoinOrder = 0;
    scoreboard = {};
    scoreboardHeartbeat = 0;
    scoreboardDirty = true;
    restoreClientLevels();
    coopReconnects.clear();
    netMapClear();
    Bytes().swap(serverMapBytes);
    Bytes().swap(clientMapBytes);
    clientDownloadingMap = mapDownloadDisconnected = false;
    clientMapSize = 0;
    mapDownloadError.clear();
    clientContentChecked = false;
    clientVersionReceived = false;
    clientVersionError.clear();
    clientServerRules = {};
    mapVote.cancel();
    voteState = netwire::VoteState::Idle;
    voteId = voteCaller = voteHeartbeat = 0;
    voteTarget.clear();
    clientVote = {};
    clientVoteOptions = {};
    clientVoteReceived = 0;
    voteCooldowns.clear();
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
    discordPresencePump();
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
                        if (!slots[p].peer && !slots[p].active && !slots[p].pendingDrop)
                        {
                            slot = p;
                            break;
                        }
                sendServerVersion(event.peer);
                if (event.data != netwire::Version)
                {
                    rejectClientVersion(event.peer, event.data);
                    continue;
                }
                if (slot < 0)
                {
                    sendReject(event.peer, "Server full");
                    continue;
                }
                uint32_t generation = slots[slot].generation + 1;
                if (!generation)
                    generation = 1;
                slots[slot]            = ClientSlot();
                slots[slot].generation = generation;
                slots[slot].peer       = event.peer;
                slots[slot].country = netCountryLookupIPv6(event.peer->address.host.s6_addr);
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
                        scoreboardDirty = true;
                    }
            }
            else
            {
                serverPeer          = nullptr;
                stopLocalCinematic();
                clientCinematic = {};
                scoreboard = {};
                clientSnapshotReady = false;
                mapDownloadDisconnected = clientDownloadingMap;
                netLagometerDisconnect(true);
                LOG_F(WARNING, "NET disconnected from server");
            }
        }
    }
    pumpMapDownloads();
    updateVote();
    updateServerCinematic();
    updateScoreboard();
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
    if (!clientVersionError.empty())
    {
        if (gGameMenuMgr.m_bActive) return;
        int y = 85;
        for (auto const& line : clientVersionError)
        {
            viewDrawText(3, line.c_str(), 160, y, -128, 0, 1, 1);
            y += 12;
        }
        return;
    }
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
void netQueueEpisodeOutro(int episode, int level)
{
    if (!netIsServer() || gGameOptions.nGameType != kGameTypeCoop || !pendingOutros.empty()) return;
    LevelCinematic scene;
    if (levelGetOutroCinematic(episode, level, scene))
        appendCinematic(pendingOutros, scene, netcinematic::Kind::Outro);
}
bool netCinematicActive()
{
    if (!netIsLive() || demoPlayback) return false;
    return netIsServer() ? serverCinematic.active && serverCinematic.participants != 0 : clientCinematic.active;
}
void netDrawCinematic()
{
    if (gDedicated || demoPlayback || !netIsLive() || !clientCinematic.active || !gGameStarted || gStartNewGame
        || (netIsClient() && !clientLoaded)) return;
    if (myconnectindex < 0 || !(clientCinematic.participants & (1u << myconnectindex)))
    {
        credCinematicDrawWaiting();
        return;
    }
    if (localCinematicId != clientCinematic.id)
    {
        stopLocalCinematic();
        localCinematicId = clientCinematic.id;
        sndStopSong();
        sndKillAllSounds();
        sfxKillAllSounds();
        ambKillAll();
        auto const& scene = clientCinematic.descriptor;
        localCinematicOpened = credCinematicBegin(scene.video.c_str(), scene.audio.c_str(), scene.audioResource);
        localCinematicStart = timerGetTicks();
        LOG_F(INFO, "NET cinematic local id=%u state=%s reason=%s kind=%s video=%s",
              localCinematicId, localCinematicOpened ? "playing" : "waiting", localCinematicOpened ? "started" : "missing",
              cinematicKind(scene.kind), scene.video.c_str());
        if (!localCinematicOpened) finishLocalCinematic("missing");
    }
    if (!localCinematicFinished)
    {
        uint32_t const now = timerGetTicks();
        if (cinematicSkip.update(now, KB_KeyDown[sc_Space] != 0, appactive != 0))
            finishLocalCinematic("skipped");
        else if (!credCinematicFrame(now - localCinematicStart))
            finishLocalCinematic("completed");
        else
            credCinematicDrawSkip(cinematicSkip.progress(now));
    }
    if (localCinematicFinished) credCinematicDrawWaiting();
}
void netOnLevelChanging()
{
    clientVersionError.clear();
    if (!netIsServer()) return;
    coopReconnects.clear();
    unsigned const lives = gDedicated && netServerSettings().gameType == kGameTypeCoop ? netServerSettings().coopLives : 0;
    for (auto& slot : slots)
    {
        slot.coopLives.reset(lives);
        slot.coopDowned = false;
        slot.coopGiveUpTicks = 0;
    }
    stopLocalCinematic();
    serverCinematic = clientCinematic = {};
    cinematicQueue.clear();
    cinematicBarrier.reset();
    cancelVote("Map vote cancelled because the server is changing levels");
    mapVote.cancel();
    voteState = netwire::VoteState::Idle;
    voteTarget.clear();
    clientVote = {};
    for (auto& slot : slots) slot.ready = false;
}
void netOnPlayerStarting(int p)
{
    if (!netIsServer() || p < 0 || p >= 8 || !slots[p].active) return;
    if (gGameOptions.nGameType != kGameTypeTeams)
    {
        slots[p].team = -1;
        return;
    }
    // playerInit assigns slot parity on a fresh map. Restore the server's
    // session assignment before playerStart chooses a team spawn/palette.
    if (slots[p].team >= 0) gPlayer[p].teamId = slots[p].team;
    else slots[p].team = gPlayer[p].teamId;
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
        scoreboardDirty = true;
        if (!mapGeneration)
            mapGeneration = 1;
        serverTick = snapshotId = lastSnapshotTick = 0;
        snapshotBudget                             = 30;
        snapshotCache.clear();
        nextTickTime = timerGetTicks();
        if (gGameOptions.nGameType == kGameTypeCoop)
        {
            cinematicQueue.swap(pendingOutros);
            std::vector<LevelCinematic> intros;
            levelCollectIntroCinematics(gGameOptions.nEpisode, gGameOptions.nLevel, intros);
            for (const auto& scene : intros)
                appendCinematic(cinematicQueue, scene, scene.mapScene ? netcinematic::Kind::Map : netcinematic::Kind::Intro);
            if (!cinematicQueue.empty()) startQueuedCinematic(cinematicParticipants());
        }
        pendingOutros.clear();
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
        if (!gDedicated) { sendVoteOptions(myconnectindex); sendVoteStatus(myconnectindex); }
        if (serverCinematic.active) publishCinematicState();
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
                netOnPlayerStarting(p);
                playerStart(p, 1);
            }
            c.ready         = true;
            c.lastInput     = {};
            c.lastInputTime = now;
            scoreboardDirty = true;
            LOG_F(INFO, "NET joined slot=%d generation=%u players=%d tick=%u", p, c.generation, numplayers, serverTick);
            sendVoteOptions(p);
            sendVoteStatus(p);
            sendCinematicState(p);
            lastSnapshotTick = 0;
        }
        if (!c.active)
            continue;
        if (serverCinematic.active)
        {
            c.commands = {};
            c.lastInput = {};
            gFifoInput[gNetFifoTail & 255][p] = {};
            gNetFifoHead[p] = gNetFifoTail + 1;
            continue;
        }
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
            if (c.coopDowned) input.keyFlags.action = c.lastInput.keyFlags.action;
            input.useFlags.byte = 0;
            input.newWeapon     = 0;
            input.q16turn       = 0;
            input.q16mlook      = 0;
        }
        if (netCoopPlayerIsSpectator(p)) input = {};
        gFifoInput[gNetFifoTail & 255][p] = input;
        gNetFifoHead[p]                   = gNetFifoTail + 1;
    }
    updateServerCinematic();
    // An empty dedicated server can keep ticking, but its first admitted
    // player still receives the queued intro before gameplay begins.
    if (serverCinematic.active && serverCinematic.participants) return false;
    serverInTick = true;
    balanceTeams();
    return true;
}
void netServerEndTick()
{
    serverInTick = false;
    if (!netIsServer())
        return;
    serverTick++;
    if (netCoopLifeLimit() && !gStartNewGame && !(gGameOptions.uGameFlags & kGameFlagContinuing))
    {
        bool anyActive = false, exhausted = true;
        for (auto const& client : slots)
        {
            if (client.pendingDrop || (!client.active && !client.admitted)) continue;
            anyActive |= client.active;
            if (!client.coopLives.exhausted()) exhausted = false;
        }
        if (anyActive && exhausted)
        {
            LOG_F(INFO, "NET coop lives exhausted: restarting map=%s", gGameOptions.zLevelName);
            for (int p = connecthead; p >= 0; p = connectpoint2[p])
                netMessageForPlayer(p, "All players are out of lives. Restarting the level.");
            netServerRestartLevel();
        }
    }
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
    if (netCinematicActive())
    {
        gInput = gNetInput = {};
        return;
    }
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
    // A connected client's assigned name comes from the server. Clearing the
    // local name field must not blank it between authoritative snapshots.
    if (!(netIsClient() && clientSnapshotReady && !szPlayerName[0]))
        Bstrncpyz(gProfile[p].name, szPlayerName, sizeof(gProfile[p].name));
    if (netIsServer())
    {
        ensureServerPlayerName(p);
        if (p == myconnectindex)
            Bstrncpyz(szPlayerName, gProfile[p].name, sizeof(szPlayerName));
        scoreboardDirty = true;
    }
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
namespace {
netwire::VoteStatusPayload displayedVote()
{
    if (netIsServer()) return voteStatus(myconnectindex);
    auto status = clientVote;
    uint32_t const elapsed = timerGetTicks() - clientVoteReceived;
    status.remainingMs -= std::min(elapsed, status.remainingMs);
    status.cooldownMs -= std::min(elapsed, status.cooldownMs);
    return status;
}
void submitVoteRequest(const netwire::VoteRequestPayload& request)
{
    if (demoPlayback || !netIsLive() || !gGameStarted || (netIsClient() && !clientSnapshotReady))
    {
        OSD_Printf("Map voting requires an active multiplayer connection.\n");
        return;
    }
    if (netIsServer())
    {
        if (gDedicated || myconnectindex < 0)
            OSD_Printf("Only connected players can call or cast map votes.\n");
        else
            handleVoteRequest(myconnectindex, request);
        return;
    }
#ifndef NETCODE_DISABLE
    netwire::Writer w;
    if (!netwire::writeVoteRequest(w, request))
    {
        OSD_Printf("Invalid map vote request or map name.\n");
        return;
    }
    sendTo(serverPeer, netwire::VoteRequest, w.data);
#endif
}
}
void netCallMapVote(const char* action, const char* mapName)
{
    netwire::VoteRequestPayload request;
    if (action && !Bstrcasecmp(action, "map")) request.command = netwire::VoteCommand::Map;
    else if (action && !Bstrcasecmp(action, "restart")) request.command = netwire::VoteCommand::Restart;
    else if (action && !Bstrcasecmp(action, "next")) request.command = netwire::VoteCommand::Next;
    else { OSD_Printf("Use callvote map <name>, callvote restart, or callvote next.\n"); return; }
    request.map = mapName ? mapName : "";
    if (!netwire::validVoteRequest(request)) { OSD_Printf("Invalid map vote request or map name.\n"); return; }
    submitVoteRequest(request);
}
void netCastMapVote(bool yes)
{
    auto status = displayedVote();
    if (status.state != netwire::VoteState::Active)
    {
        OSD_Printf("No map vote is active.\n");
        return;
    }
    netwire::VoteRequestPayload request;
    request.command = yes ? netwire::VoteCommand::Yes : netwire::VoteCommand::No;
    request.id = status.id;
    submitVoteRequest(request);
}
void netListVoteMaps()
{
    if (netIsServer()) { showVoteOptions(voteOptions(true)); return; }
    if (demoPlayback) { auto options = clientVoteOptions; options.listRequested = 1; showVoteOptions(options); return; }
    netwire::VoteRequestPayload request;
    request.command = netwire::VoteCommand::List;
    submitVoteRequest(request);
}
void netPrintVoteStatus()
{
    if (netIsServer() || demoPlayback) { logVoteStatus(displayedVote()); return; }
    netwire::VoteRequestPayload request;
    request.command = netwire::VoteCommand::QueryStatus;
    submitVoteRequest(request);
}
void netDrawMapVote()
{
    if (gDedicated || !netIsLive() || !gGameStarted) return;
    auto status = displayedVote();
    if (status.state != netwire::VoteState::Active) return;
    char heading[128], tally[128], threshold[64];
    Bsnprintf(heading, sizeof(heading), "MAP VOTE: %.100s", status.action == 1 ? status.target.c_str()
              : status.action == 2 ? "Restart level" : "Next level");
    Bsnprintf(tally, sizeof(tally), "%us left | YES %u  NO %u", (status.remainingMs + 999) / 1000, status.yes, status.no);
    Bsnprintf(threshold, sizeof(threshold), "%u of %u YES required", status.required, status.eligible);
    const char* prompt = demoPlayback ? "Recorded map vote" : status.voted ? "Your vote has been counted"
        : status.canVote ? "Console: vote yes / vote no" : "You joined after this vote started";
    auto drawLine = [](const char* text, int y)
    {
        char line[128];
        Bsnprintf(line, sizeof(line), "%s", text);
        int width = 0;
        viewGetFontInfo(3, line, &width, nullptr);
        // Keep the panel left of the aiming area, even in a 320-wide view.
        for (size_t length = Bstrlen(line); width > 144 && length > 3;)
        {
            --length;
            Bmemcpy(line + length - 3, "...", 4);
            viewGetFontInfo(3, line, &width, nullptr);
        }
        viewDrawText(3, line, 8, y, -128, 0, 0, 1, RS_ALIGN_L);
    };
    // The map title occupies the center of the screen at y=50 after a load.
    drawLine(heading, 70);
    drawLine(tally, 79);
    drawLine(threshold, 88);
    drawLine(prompt, 97);
}
void netPrintStatus()
{
    auto const& scene = netIsServer() ? serverCinematic : clientCinematic;
    LOG_F(INFO, "NET cinematic status id=%u state=%s kind=%s participants=%u done=%u remaining=%d local=%s",
          scene.id, scene.active ? "active" : "end", cinematicKind(scene.descriptor.kind), unsigned(scene.participants),
          unsigned(scene.done), cinematicRemaining(scene), localCinematicId ? (localCinematicFinished ? "waiting" : "playing") : "idle");
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
    if (!gDedicated)
        LOG_F(INFO, "NET view chase=%d target=%d spectator=%d", int(gViewPos != VIEWPOS_0), gViewIndex,
              int(netCoopPlayerIsSpectator(myconnectindex)));
    for (int p = connecthead; p >= 0; p = connectpoint2[p])
        if (gPlayer[p].pSprite && gPlayer[p].pXSprite)
        {
            auto *s = gPlayer[p].pSprite;
            int64_t ammo = 0, armor = 0, pack = 0;
            unsigned keys = 0, weapons = 0;
            for (int value : gPlayer[p].ammoCount) ammo += value;
            for (int value : gPlayer[p].armor) armor += value;
            for (auto const& item : gPlayer[p].packSlots) pack += item.curAmount;
            for (unsigned i = 0; i < ARRAY_SIZE(gPlayer[p].hasKey); ++i) if (gPlayer[p].hasKey[i]) keys |= 1u << i;
            for (unsigned i = 0; i < ARRAY_SIZE(gPlayer[p].hasWeapon); ++i) if (gPlayer[p].hasWeapon[i]) weapons |= 1u << i;
            LOG_F(INFO, "NET player slot=%d x=%d y=%d z=%d health=%d ack=%u autoaim=%d skill=%d team=%d join_order=%llu palette=%d frags=%d weapon=%d ammo_sum=%lld armor_sum=%lld keys_mask=%u weapons_mask=%u pack_sum=%lld lives=%d spectator=%d downed=%d giveup=%d",
                  p, int(s->x), int(s->y), int(s->z), int(gPlayer[p].pXSprite->health), netAppliedCommandSequence(p), gProfile[p].nAutoAim, gProfile[p].skill,
                  gPlayer[p].teamId, (unsigned long long)(netIsServer() ? slots[p].joinOrder : 0), int(s->pal), gPlayer[p].fragCount,
                  int(gPlayer[p].curWeapon), (long long)ammo, (long long)armor, keys, weapons, (long long)pack,
                  netCoopLivesRemaining(p), int(netCoopPlayerIsSpectator(p)), int(netCoopPlayerIsDowned(p)), netCoopGiveUpProgress(p));
        }
    if (netIsServer()) scoreboard = collectScoreboard();
    for (auto const& row : scoreboard.rows)
        LOG_F(INFO, "NET scoreboard slot=%u generation=%u score=%d ping_ms=%u minutes=%u country=%s team=%u lives=%d downed=%d giveup=%u name=\"%s\"",
              unsigned(row.slot), row.generation, row.score, row.pingMs, row.elapsedMinutes,
              row.country.empty() ? "--" : row.country.c_str(), unsigned(row.team), row.livesRemaining,
              int(row.downed), unsigned(row.giveUpTicks), row.name.c_str());
    if (netIsLive() && gGameOptions.nGameType == kGameTypeTeams)
        LOG_F(INFO, "NET team scores blue=%d red=%d", gPlayerScores[0], gPlayerScores[1]);
    if (netIsLive())
    {
        auto const rules = netIsServer() ? serverRules() : clientServerRules;
        LOG_F(INFO, "NET rules gametype=%d timelimit=%u scorelimit=%u maxplayers=%u autoaim=%u weaponsstay=%d difficulty=%d losekeys=%d episode=%d level=%d rotation_index=%u rotation_count=%u mapvote=%u friendly_fire=%d holster_weapon=%d chase_view=%d coop_lives=%d",
              int(gGameOptions.nGameType), rules.timeLimitMinutes, rules.scoreLimit, rules.maxPlayers, rules.autoAim,
              int(gGameOptions.nWeaponSettings == 1), int(gGameOptions.nDifficulty), int(gGameOptions.bPlayerKeys == LOSTONDEATH),
              gGameOptions.nEpisode + 1, gGameOptions.nLevel + 1, rules.rotationIndex, rules.rotationCount, rules.mapVoteEnabled,
              int(gGameOptions.bFriendlyFire), int(netAllowHolsterWeapon()), int(netAllowChaseView()), netCoopLifeLimit());
        int monsters = 0, pickups = 0;
        for (int n = headspritestat[kStatDude]; n >= 0; n = nextspritestat[n])
            if (!IsPlayerSprite(&sprite[n]) && sprite[n].extra > 0 && xsprite[sprite[n].extra].health > 0) ++monsters;
        for (int n = headspritestat[kStatItem]; n >= 0; n = nextspritestat[n]) ++pickups;
        LOG_F(INFO, "NET world monsters_alive=%d pickups=%d", monsters, pickups);
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
    return recordLoopBaseline() && recordVoteBaseline() && recordScoreboardBaseline();
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
        || (h.type != netwire::Welcome && h.type != netwire::Snapshot && h.type != netwire::Event && h.type != netwire::Chat && h.type != netwire::Taunt
            && h.type != netwire::VoteStatus && h.type != netwire::VoteOptions && h.type != netwire::Scoreboard))
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
