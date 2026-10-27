// SPDX-License-Identifier: GPL-2.0-or-later
// Authoritative Rednukem sessions. Offline simulation and legacy demos keep
// their original input pipeline; only this module advances an online server.
#include "duke3d.h"
#include "game.h"
#include "premap.h"
#include "gamedef.h"
#include "net.h"
#include "cmdline.h"
#include "screens.h"
#include "demo.h"
#include "rnet.h"
#include "rnet_snapshot.h"
#include "rnet_prediction.h"
#include "rnet_content.h"
#include "rnet_social.h"
#include "rnet_sound.h"
#include "rnet_rules.h"
#include "rnet_presence.h"
#include "rnet_uri.h"
#include "rnet_demo.h"
#include "zerotier.h"
#include "../../blood/src/netprotocol.h"
#include "../../blood/src/netsnapshot.h"
#include <algorithm>
#include <array>
#include <deque>
#include <cstdlib>
#include <cstring>
#include <limits>
#ifndef NETCODE_DISABLE
#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

namespace rnet {
bool zeroTierSelected = false;
char zeroTierNetwork[17] = ZEROTIER_NETWORK_ID;
namespace {
constexpr uint32_t Magic = 0x314d4e52; // RNM1: deliberately distinct from Blood and lockstep.
constexpr size_t HeaderSize = 17;
constexpr size_t MaxPacketBytes = MaxMessage + 4096; // Codec and envelope overhead.
constexpr unsigned HistorySize = 32;
struct Command { uint32_t sequence = 0; input_t input{}; };
struct Slot {
    ENetPeer* peer = nullptr;
    bool authenticated = false, ready = false, contentPrepared = false;
    uint32_t joined = 0, lastInput = 0, applied = 0, acknowledged = 0, rateAt = 0;
    unsigned rateCount = 0;
    std::deque<Command> commands;
    input_t held{};
    uint32_t pendingBaseline = 0;
    std::vector<uint8_t> baselineBytes;
};
struct State { uint32_t sequence = 0; std::vector<uint8_t> bytes; };
std::array<Slot, MAXPLAYERS> slots;
std::array<State, HistorySize> history;
bool initialized = false, running = false, isDedicated = false, loaded = false;
bool pumping = false, loading = false, isPlayback = false;
int capacity = MAXPLAYERS, local = 0, recordView = -1;
int32_t packetCopies = 3, snapshotRate = 20;
uint32_t generation = 0, serverTick = 0, snapshotSequence = 0, inputSequence = 0;
uint32_t lastSnapshot = 0, connectStart = 0, playbackStart = 0, lastApplied = 0;
double nextTick = 0, nextSnapshot = 0;
std::string remote, pendingConnect, currentMap, pendingMap, lastError, pendingError, loadedMapPath;
std::string passwordRetryAddress;
bool passwordRetryZeroTier = false;
std::string passwordRetryNetwork;
bool passwordPromptQueued = false, passwordWasIncorrect = false;
bool mapQueued = false, freshMap = false, haveWelcome = false, transportConnected = false;
bool recordWelcomePending = false, recordWelcomeRebase = false, playbackFinished = false;
bool recordMapPending = false;
bool activeZeroTier = false, pendingZeroTierHost = false;
std::string zeroTierOption;
std::string serviceNetwork, activeZeroTierNetwork, pendingZeroTierNetwork;
std::string zeroTierError;
std::string configuredZeroTierNetwork() { return zeroTierNetwork[0] ? zeroTierNetwork : ZEROTIER_NETWORK_ID; }
bool contentLoadPending = false;
int pendingVolume = 0, pendingLevel = 0, pendingSkill = 1;
std::vector<uint8_t> welcomeBytes;
std::deque<Command> localCommands;
uint32_t received = 0, sent = 0;

bool validSlot(int p) { return p >= 0 && p < MAXPLAYERS; }
bool supportedGame() { return !REALITY && !DEER && !NAM && !WW2GI; }
std::vector<uint8_t> envelope(uint8_t type, const std::vector<uint8_t>& body) {
    netwire::Writer out;
    out.u32(Magic); out.u32(Protocol); out.u32(generation); out.u32(serverTick); out.u8(type);
    out.bytes(body.data(), body.size()); return std::move(out.data);
}
bool transmit(ENetPeer* p, uint8_t type, const std::vector<uint8_t>& body, bool reliable) {
    if (!p || body.size() > MaxPacketBytes - HeaderSize) return false;
    auto bytes = envelope(type, body);
    ENetPacket* packet = enet_packet_create(bytes.data(), bytes.size(), reliable ? ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT);
    if (!packet) return false;
    if (enet_peer_send(p, reliable ? 0 : (type == Input ? 1 : 2), packet) < 0) {
        enet_packet_destroy(packet);
        OSD_Printf("NET could not queue message type=%u bytes=%u\n", unsigned(type), unsigned(bytes.size()));
        return false;
    }
    ++sent; return true;
}
void remember(uint8_t type, const std::vector<uint8_t>& body, bool rebase = false) {
    if ((recordMapPending || (client() && recordWelcomePending)) && type != Welcome) return;
    if (netDemoIsRecording() && type < PresenceFirst && !(type >= ContentFirst && type < SocialFirst)) {
        if (type == Welcome && client()) {
            // Cache paths are local, generated identities. Recording their
            // resolved name permits offline playback after disconnect/restart.
            netwire::Reader in(body.data(), body.size()); netwire::Writer out;
            out.u8(in.u8()); out.u8(in.u8()); out.u32(in.u32()); out.u32(in.u32());
            out.u16(in.u16()); out.u16(in.u16()); out.u16(in.u16());
            std::string name = in.text(BMAX_PATH - 1);
            if (in.end()) {
                out.text(content::resolveMap(name.c_str()), BMAX_PATH - 1);
                netDemoRecordPacket(serverTick, envelope(type, out.data), rebase);
            }
        } else netDemoRecordPacket(serverTick, envelope(type, body), rebase);
    }
}
void rememberState() {
    remember(RulesFirst, rules::demoState());
    for (const auto& event : social::demoState()) remember(event.first, event.second);
    remember(audio::Baseline, audio::demoState());
}
void roster() {
    connecthead = -1;
    int previous = -1, count = 0;
    for (int i = 0; i < MAXPLAYERS; ++i) {
        connectpoint2[i] = -1;
        g_player[i].playerquitflag = playing(i);
        if (!playing(i)) continue;
        if (previous < 0) connecthead = i; else connectpoint2[previous] = i;
        previous = i; ++count;
    }
    // The simulation's closest-player helpers require a valid sentinel even
    // when the server is empty. It is hidden and excluded from player updates.
    if (connecthead < 0) connecthead = 0;
    numplayers = count;
    g_mostConcurrentPlayers = MAXPLAYERS;
    ud.multimode = MAXPLAYERS;
    if (server() && dedicated()) myconnectindex = screenpeek = connecthead;
}
void hidePlayer(int p) {
    int const s = g_player[p].ps->i;
    if (s >= 0 && s < MAXSPRITES && sprite[s].statnum < MAXSTATUS) {
        sprite[s].cstat = 32768; sprite[s].extra = 0;
        changespritestat(s, STAT_NETALLOC);
    }
}
void provisionPlayers() {
    DukePlayer_t const initial = *g_player[0].ps;
    spritetype const initialSprite = sprite[initial.i];
    actor_t const initialActor = actor[initial.i];
    for (int p = 1; p < MAXPLAYERS; ++p) {
        *g_player[p].ps = initial;
        int const s = insertsprite(initial.cursectnum, STAT_NETALLOC);
        if (s < 0) { fail("No free sprites for multiplayer players"); return; }
        sprite[s] = initialSprite;
        sprite[s].statnum = STAT_NETALLOC;
        sprite[s].yvel = p; sprite[s].owner = s;
        actor[s] = initialActor;
#ifdef POLYMER
        actor[s].lightptr = nullptr; actor[s].lightId = -1;
#endif
        g_player[p].ps->i = s;
        g_player[p].ps->frag_ps = p;
        // Co-op fixed respawn must also work on maps with fewer spawn points.
        if (p >= g_playerSpawnCnt) g_playerSpawnPoints[p] = g_playerSpawnPoints[p % std::max(1, g_playerSpawnCnt)];
    }
    for (int p = 0; p < MAXPLAYERS; ++p) hidePlayer(p);
}
void spawn(int p) {
    auto& player = *g_player[p].ps;
    changespritestat(player.i, STAT_PLAYER);
    player.gm = MODE_GAME;
    player.team = g_player[p].pteam;
    P_ResetPlayer(p);
    sprite[player.i].yvel = p;
    sprite[player.i].extra = player.max_player_health;
    player.last_extra = player.max_player_health;
}
void clearHistory() {
    for (auto& state : history) state = State{};
    snapshotSequence = lastApplied = inputSequence = 0;
    localCommands.clear();
    for (auto& slot : slots) {
        slot.acknowledged = slot.applied = 0;
        slot.commands.clear(); slot.held = input_t{};
        slot.pendingBaseline = 0; slot.baselineBytes.clear();
    }
}
std::vector<uint8_t> welcome(int player) {
    netwire::Writer out;
    out.u8(uint8_t(player)); out.u8(uint8_t(capacity));
    out.u32(g_gameType); out.u32(g_scriptcrc);
    out.u16(ud.volume_number); out.u16(ud.level_number); out.u16(ud.player_skill);
    out.text(currentMap, BMAX_PATH - 1);
    return std::move(out.data);
}
void beginJoin(int p) {
    slots[p].ready = false; slots[p].contentPrepared = false;
    transmit(slots[p].peer, Welcome, welcome(p), true);
    content::offer(p);
}
bool load(const std::string& requested, bool reset, int volume = -1, int level = -1, int skill = -1) {
    if (requested.empty() || requested.size() >= BMAX_PATH) { fail("Invalid multiplayer map name"); return false; }
    audio::reset();
    loading = true; loaded = false; loadedMapPath.clear();
    G_CloseDemoWrite(); ud.recstat = ud.m_recstat = 0;
    myconnectindex = screenpeek = 0;
    connecthead = 0; connectpoint2[0] = -1;
    numplayers = ud.multimode = g_mostConcurrentPlayers = 1;
    // Treat the selected cached map as a user map, while preserving server
    // episode/level metadata used by exits and cinematics.
    std::string path = requested;
    int episode = 0, mapLevel = 0; char extra = 0;
    if ((sscanf(path.c_str(), "E%dL%d%c", &episode, &mapLevel, &extra) == 2 ||
         sscanf(path.c_str(), "e%dl%d%c", &episode, &mapLevel, &extra) == 2) &&
        episode > 0 && episode <= MAXVOLUMES && mapLevel > 0 && mapLevel <= MAXLEVELS &&
        g_mapInfo[(episode-1)*MAXLEVELS + mapLevel-1].filename)
        path = g_mapInfo[(episode-1)*MAXLEVELS + mapLevel-1].filename;
    if (client() || playback()) path = content::resolveMap(requested.c_str());
    if (client() || playback()) {
        std::string error;
        if (path.empty() || !content::validateMapForLoad(path.c_str(), error)) {
            loading = false;
            fail("Invalid multiplayer map: " + (error.empty() ? requested : error));
            return false;
        }
    }
    int index = G_FindLevelByFile(path.c_str());
    if (index >= MAXLEVELS * MAXVOLUMES && !path.empty() && path.front() != '/' && path.front() != '\\')
        index = G_FindLevelByFile(("/" + path).c_str());
    if (index < MAXLEVELS * MAXVOLUMES) {
        volume = index / MAXLEVELS; level = index % MAXLEVELS;
        boardfilename[0] = 0;
    } else {
        volume = 0; level = 7;
        Bstrncpyz(boardfilename, path.c_str(), sizeof(boardfilename));
    }
    ud.m_volume_number = std::max(0, volume);
    ud.m_level_number = std::max(0, level);
    if (skill >= 0) ud.m_player_skill = skill;
    if (server()) rules::applySettings();
    if (reset || !generation || client() || playback())
        G_NewGame(ud.m_volume_number, ud.m_level_number, ud.m_player_skill);
    else { ud.volume_number = ud.m_volume_number; ud.level_number = ud.m_level_number; }
    // A new game may reset the connection list. Map preprocessing uses one
    // local bootstrap player; remaining slots are allocated after load.
    numplayers = ud.multimode = g_mostConcurrentPlayers = 1;
    connecthead = 0; connectpoint2[0] = -1;
    int const result = G_EnterLevel(reset ? MODE_GAME : MODE_EOL);
    if (result) { loading = false; fail("Could not load multiplayer map " + requested); return false; }
    currentMap = server() ? (loadedMapPath.empty() ? path : loadedMapPath) : requested;
    if (server()) {
        provisionPlayers();
        for (int p = 0; p < MAXPLAYERS; ++p) if (playing(p)) spawn(p);
        roster();
    }
    loaded = true; loading = false;
    ready2send = 1;
    nextTick = timerGetFractionalTicks(); nextSnapshot = nextTick;
    return true;
}
void changeMap() {
    std::string map = pendingMap; bool const restart = freshMap;
    mapQueued = false;
    recordMapPending = true;
    if (map.empty()) map = currentMap;
    rules::mapChanging(restart);
    if (!load(map, restart)) return;
    ++generation; clearHistory();
    content::mapChanged(); rules::mapStart();
    for (int p = 0; p < MAXPLAYERS; ++p) if (slots[p].authenticated) {
        if (slots[p].peer) beginJoin(p);
        else { slots[p].ready = true; spawn(p); }
    }
    roster(); social::tick(); presence::mapChanged();
    recordMapPending = false;
    if (netDemoIsRecording() && validSlot(recordView)) {
        remember(Welcome, welcome(recordView), true); rememberState();
    }
    OSD_Printf("NET map generation=%u map=%s\n", generation, currentMap.c_str());
}
void writeInput(netwire::Writer& out, const Command& cmd) {
    out.u32(cmd.sequence); out.u32(cmd.input.bits);
    out.u16(uint16_t(cmd.input.fvel)); out.u16(uint16_t(cmd.input.svel));
    out.u32(uint32_t(cmd.input.q16avel)); out.u32(uint32_t(cmd.input.q16horz)); out.u8(uint8_t(cmd.input.extbits));
}
Command readInput(netwire::Reader& in) {
    Command cmd; cmd.sequence = in.u32(); cmd.input.bits = in.u32();
    cmd.input.fvel = int16_t(in.u16()); cmd.input.svel = int16_t(in.u16());
    cmd.input.q16avel = int32_t(in.u32()); cmd.input.q16horz = int32_t(in.u32()); cmd.input.extbits = int8_t(in.u8());
    // Bound hostile input before the simulation performs signed arithmetic.
    cmd.input.fvel = std::max<int16_t>(-8192, std::min<int16_t>(8192, cmd.input.fvel));
    cmd.input.svel = std::max<int16_t>(-8192, std::min<int16_t>(8192, cmd.input.svel));
    cmd.input.q16avel = std::max(-F16(1024), std::min(F16(1024), cmd.input.q16avel));
    cmd.input.q16horz = std::max(-F16(256), std::min(F16(256), cmd.input.q16horz));
    cmd.input.bits &= ~(1u << SK_PAUSE);
    cmd.input.extbits &= 0x3f; // Team and disconnect control are server-owned.
    return cmd;
}
void snapshots() {
    std::vector<uint8_t> state; std::string error;
    if (!capture(state, error)) { fail("Snapshot capture: " + error); return; }
    uint32_t const seq = ++snapshotSequence;
    history[seq % HistorySize] = State{seq, std::move(state)};
    auto const& now = history[seq % HistorySize].bytes;
    for (int p = 0; p < MAXPLAYERS; ++p) if (playing(p) && slots[p].peer) {
        auto& slot = slots[p];
        // One outstanding reliable baseline is sufficient: ENet retransmits
        // it. Never accumulate full worlds for a slow/unresponsive peer.
        if (!slot.acknowledged && slot.pendingBaseline) continue;
        auto const& base = history[slot.acknowledged % HistorySize];
        bool delta = false; std::vector<uint8_t> packed;
        auto const* previous = slot.acknowledged ? (base.sequence == slot.acknowledged ? &base.bytes : &slot.baselineBytes) : nullptr;
        if (previous && previous->empty()) previous = nullptr;
        if (!netSnapshotPack(now, previous, packed, delta, error)) { fail("Snapshot compression: " + error); return; }
        netwire::Writer out;
        out.u32(seq); out.u32(delta ? slot.acknowledged : 0); out.u32(slot.applied);
        out.bytes(packed.data(), packed.size());
        if (transmit(slot.peer, Snapshot, out.data, !slot.acknowledged) && !slot.acknowledged) {
            slot.pendingBaseline = seq; slot.baselineBytes = now;
        }
    }
    if (netDemoIsRecording()) {
        if (!validSlot(recordView) || !playing(recordView)) {
            recordView = -1;
            for (int p = 0; p < MAXPLAYERS; ++p) if (playing(p)) { recordView = p; break; }
            if (recordView >= 0) { remember(Welcome, welcome(recordView)); rememberState(); }
        }
        if (recordView >= 0) {
            bool delta = false; std::vector<uint8_t> packed;
            if (netSnapshotPack(now, nullptr, packed, delta, error)) {
                netwire::Writer out; out.u32(seq); out.u32(0); out.u32(slots[recordView].applied);
                out.bytes(packed.data(), packed.size()); remember(Snapshot, out.data);
            }
        }
    }
}
void process(int sender, const uint8_t* bytes, size_t size, ENetPeer* source = nullptr) {
    if (size < HeaderSize || size > MaxPacketBytes) return;
    if (server()) {
        if (!validSlot(sender) || source != slots[sender].peer || size > 4096) return;
        Slot& slot = slots[sender];
        uint32_t now = timerGetTicks();
        if (now - slot.rateAt >= 1000) { slot.rateAt = now; slot.rateCount = 0; }
        if (++slot.rateCount > 240) return;
    }
    netwire::Reader head(bytes, size);
    if (head.u32() != Magic) return;
    uint32_t const protocol = head.u32(), map = head.u32(), time = head.u32();
    uint8_t const type = head.u8();
    std::vector<uint8_t> body(bytes + HeaderSize, bytes + size);
    netwire::Reader in(body.data(), body.size());
    if (protocol != Protocol && type != Hello && type != Reject && type != Authenticate) return;
    if (server()) {
        if (!validSlot(sender) || source != slots[sender].peer) return;
        auto& slot = slots[sender];
        if (type == Authenticate) {
            if (slot.authenticated) return;
            uint32_t const version = in.u32();
            std::string const name = in.text(31), password = in.text(128);
            if (!in.end()) { reject(sender, "Invalid authentication packet"); return; }
            if (version != Protocol) { reject(sender, "Network protocol mismatch. Update Rednukem and reconnect."); return; }
            if (password != g_netPassword) { reject(sender, password.empty() ? "Server password required" : "Incorrect server password"); return; }
            std::string clean = name;
            for (char& c : clean) if (uint8_t(c) < 32 || c == 127) c = ' ';
            slot.authenticated = true; slot.joined = timerGetTicks();
            clean = social::normalizeName(sender, clean);
            Bstrncpyz(g_player[sender].user_name, clean.c_str(), sizeof(g_player[sender].user_name));
            rules::playerJoined(sender); social::join(sender); presence::connected(sender);
            beginJoin(sender);
            OSD_Printf("NET authenticated slot=%d name=%s\n", sender, clean.c_str());
            return;
        }
        if (!slot.authenticated || map != generation) return;
        if (type == Ready) {
            if (!in.end() || !loaded || !slot.contentPrepared) return;
            if (!slot.ready) {
                slot.ready = true; spawn(sender); roster();
                OSD_Printf("NET joined slot=%d name=%s\n", sender, g_player[sender].user_name);
                social::ready(sender); rules::playerJoined(sender); audio::ready(sender);
                nextSnapshot = 0;
            }
            return;
        }
        if (type == Input) {
            if (!slot.ready) return;
            unsigned count = in.u8();
            if (!count || count > 8) return;
            std::vector<Command> batch;
            for (unsigned i = 0; i < count; ++i) batch.push_back(readInput(in));
            if (!in.end()) return;
            for (auto const& cmd : batch) {
                // Lost input does not stop ENet acknowledgements/snapshots.
                // Accept forward gaps so a long input-only outage can recover;
                // the bounded queue and one command per tick cap simulation.
                if (!netwire::newer(cmd.sequence, slot.applied)) continue;
                if (!slot.commands.empty() && !netwire::newer(cmd.sequence, slot.commands.back().sequence)) continue;
                if (slot.commands.size() == 64) slot.commands.pop_front();
                slot.commands.push_back(cmd);
            }
            slot.lastInput = timerGetTicks(); return;
        }
        if (type == SnapshotAck) {
            uint32_t seq = in.u32();
            if (!in.end()) return;
            if (!seq) {
                if (slot.acknowledged) { slot.acknowledged = 0; slot.pendingBaseline = 0; slot.baselineBytes.clear(); }
            } else if (netwire::newer(seq, slot.acknowledged) &&
                       (history[seq % HistorySize].sequence == seq || slot.pendingBaseline == seq)) {
                slot.acknowledged = seq;
                if (history[seq % HistorySize].sequence == seq) slot.baselineBytes = history[seq % HistorySize].bytes;
                slot.pendingBaseline = 0;
            }
            return;
        }
        if (type >= audio::Event && type <= audio::Baseline) audio::handle(sender, type, body);
        else if (type >= ContentFirst && type < SocialFirst) content::handle(sender, type, body.data(), body.size());
        else if (type >= SocialFirst && type < RulesFirst) social::handle(sender, type, body);
        else if (type >= RulesFirst && type < PresenceFirst) rules::message(sender, type, body.data(), body.size());
        return;
    }
    if (!client() && !playback()) return;
    if (type == Hello) {
        uint32_t const version = in.u32(); std::string const build = in.text(128);
        if (!in.end()) { fail("Invalid server version announcement"); return; }
        if (version != Protocol) {
            fail("Client protocol " + std::to_string(Protocol) + ", server requires " + std::to_string(version) + " (" + build + "). Update " + (version > Protocol ? "Rednukem" : "the server") + " and reconnect."); return;
        }
        netwire::Writer out; out.u32(Protocol);
        out.text(szPlayerName, 31); out.text(g_netPassword, 128);
        transmit(g_netClientPeer, Authenticate, out.data, true); return;
    }
    if (type == Reject) {
        std::string reason = in.text(1024);
        if (in.end()) {
            if (!haveWelcome && (reason == "Server password required" || reason == "Incorrect server password")) {
                passwordRetryAddress = remote;
                passwordRetryZeroTier = activeZeroTier;
                passwordRetryNetwork = activeZeroTierNetwork;
                passwordPromptQueued = true;
                passwordWasIncorrect = reason == "Incorrect server password";
            }
            fail(reason);
        }
        return;
    }
    if (type == Welcome) {
        int const player = in.u8(), maximum = in.u8();
        uint32_t const variant = in.u32(), script = in.u32();
        int const volume = in.u16(), level = in.u16(), skill = in.u16();
        std::string const name = in.text(BMAX_PATH - 1);
        if (!in.end() || !validSlot(player) || maximum < 1 || maximum > MAXPLAYERS || player >= maximum || volume >= MAXVOLUMES || level >= MAXLEVELS || skill > 4 || name.empty()) { fail("Invalid server map announcement"); return; }
        if (variant != uint32_t(g_gameType)) { fail("Server requires a different Rednukem game edition. Select its matching base game data."); return; }
        (void)script; // Resource manifest negotiates CON before loading state.
        bool const newMap = generation != map || currentMap != name;
        local = player; capacity = maximum; generation = map; serverTick = time;
        pendingVolume = volume; pendingLevel = level; pendingSkill = skill;
        currentMap = name; haveWelcome = true; loaded = false;
        welcomeBytes = body; clearHistory();
        recordWelcomePending = netDemoIsRecording(); recordWelcomeRebase = newMap;
        if (playback()) contentReady(-1);
        return;
    }
    if (!haveWelcome || map != generation) return;
    if (type == Snapshot) {
        if (!loaded) return;
        uint32_t const seq = in.u32(), baseline = in.u32(), applied = in.u32();
        if (!in.ok || !seq || !netwire::newer(seq, lastApplied)) return;
        auto const& base = history[baseline % HistorySize];
        std::vector<uint8_t> packed(body.begin() + in.pos, body.end()), canonical;
        std::string error;
        auto const* previous = baseline && base.sequence == baseline ? &base.bytes : nullptr;
        if (!netSnapshotUnpack(packed, previous, canonical, error)) {
            netwire::Writer ack; ack.u32(0); send(-1, SnapshotAck, ack.data); return;
        }
        // The animation owns engine presentation state while it runs. Still
        // validate and acknowledge baselines so reliable hot-join delivery
        // cannot stall until the next map.
        bool const cinematic = rules::cinematicPlaying();
        if (!(cinematic ? validate(canonical, error) : apply(canonical, local, error))) { fail("Invalid server snapshot: " + error); return; }
        history[seq % HistorySize] = State{seq, canonical};
        lastApplied = seq; serverTick = time; lastSnapshot = timerGetTicks();
        while (!localCommands.empty() && !netwire::newer(localCommands.front().sequence, applied)) localCommands.pop_front();
        if (!cinematic) for (auto const& cmd : localCommands) predict(cmd.input);
        netwire::Writer ack; ack.u32(seq); send(-1, SnapshotAck, ack.data);
        social::snapshot(seq, unsigned(localCommands.size()));
        if (!cinematic) audio::snapshot();
        if (netDemoIsRecording()) {
            bool delta = false; std::vector<uint8_t> full;
            if (netSnapshotPack(canonical, nullptr, full, delta, error)) {
                netwire::Writer out; out.u32(seq); out.u32(0); out.u32(applied); out.bytes(full.data(), full.size());
                remember(Snapshot, out.data);
            }
        }
        if (seq == 1 || !(seq % 300)) OSD_Printf("NET snapshot tick=%u sequence=%u hash=%08x local=%d\n", time, seq, netSnapshotHash(canonical), local);
        return;
    }
    bool accepted = false;
    if (loaded && type >= audio::Event && type <= audio::Baseline) accepted = audio::handle(-1, type, body);
    else if (type >= ContentFirst && type < SocialFirst) content::handle(-1, type, body.data(), body.size());
    else if (type >= SocialFirst && type < RulesFirst) accepted = social::handle(-1, type, body);
    else if (type >= RulesFirst && type < PresenceFirst) { rules::message(-1, type, body.data(), body.size()); accepted = true; }
    else if (type >= PresenceFirst) presence::handle(-1, type, body.data(), body.size());
    if (accepted) remember(type, body);
}
void remove(int p) {
    if (!validSlot(p)) return;
    bool const wasAuthenticated = slots[p].authenticated;
    if (wasAuthenticated) rules::playerLeft(p);
    slots[p] = Slot{};
    if (loaded) hidePlayer(p);
    if (wasAuthenticated) social::leave(p);
    roster();
}
void beginConnect() {
    if (zeroTierSelected && pendingZeroTierNetwork.empty()) pendingZeroTierNetwork = configuredZeroTierNetwork();
    if (zeroTierSelected && (!zerotier::ready() || serviceNetwork != pendingZeroTierNetwork)) {
        if (serviceNetwork != pendingZeroTierNetwork || zerotier::state() == zerotier::State::Stopped) {
            if (active()) {
                std::string const target = pendingConnect;
                disconnect();
                pendingConnect = target;
            }
            zerotier::Config config;
            config.game = zerotier::Game::Rednukem;
            config.networkId = pendingZeroTierNetwork;
            std::string error;
            if (!zerotier::start(config, error)) {
                zeroTierError = error;
                pendingConnect.clear();
                fail(error.empty() ? zerotier::status() : error);
            } else { serviceNetwork = pendingZeroTierNetwork; zeroTierError.clear(); }
        } else if (zerotier::state() == zerotier::State::Error || zerotier::state() == zerotier::State::Denied) {
            pendingConnect.clear();
            fail(zerotier::status());
        }
        return;
    }
    std::string target = pendingConnect; pendingConnect.clear();
    disconnect();
    activeZeroTier = zeroTierSelected;
    activeZeroTierNetwork = activeZeroTier ? pendingZeroTierNetwork : std::string();
    pendingZeroTierNetwork.clear();
    if (!supportedGame()) { fail("Authoritative multiplayer requires Duke Nukem 3D or Redneck Rampage game data."); return; }
    remote = target;
    uri::Endpoint dest; std::string error;
    if (!uri::parse(target.find("://") == std::string::npos ? "rednukem://" + target : target, dest, error)) { fail(error); return; }
    if (target.find("://") == std::string::npos &&
        (target.find(':') == std::string::npos || (!target.empty() && target.back() == ']')))
        dest.port = g_netPort;
    remote = uri::address(dest);
    ENetAddress address{};
    if (enet_address_set_host(&address, &dest.host[0]) < 0) { fail("Cannot resolve server " + dest.host); return; }
    address.port = activeZeroTier ? zerotier::GamePort : dest.port;
    g_netClient = activeZeroTier ? zerotier::createHost(nullptr, 1, 3, 0, 0)
                                : enet_host_create(nullptr, 1, 3, 0, 0);
    if (!g_netClient) { fail("Could not create client UDP socket"); return; }
    g_netClient->packetCopies = packetCopies;
    g_netClient->maximumPacketSize = MaxPacketBytes;
    g_netClient->maximumWaitingData = 2 * MaxPacketBytes;
    g_netClientPeer = enet_host_connect(g_netClient, &address, 3, Protocol);
    if (!g_netClientPeer) { disconnect("Could not connect to server"); return; }
    enet_peer_timeout(g_netClientPeer, 16, 10000, 30000);
    running = true; g_networkMode = NET_CLIENT;
    connectStart = timerGetTicks(); transportConnected = false;
    loaded = false; lastError.clear(); nextTick = timerGetFractionalTicks();
    OSD_Printf("NET connecting %s\n", target.c_str());
}
void console() {
    if (!dedicated()) return;
    static std::string line;
#ifndef _WIN32
    static bool set = false;
    if (!set) { int flags = fcntl(STDIN_FILENO, F_GETFL); if (flags >= 0) fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK); set = true; }
    char bytes[1024]; ssize_t size;
    while ((size = read(STDIN_FILENO, bytes, sizeof(bytes))) > 0) for (ssize_t i = 0; i < size; ++i) {
        if (bytes[i] == '\n') { if (!line.empty()) OSD_Dispatch(line.c_str()); line.clear(); }
        else if (bytes[i] != '\r' && line.size() < 4096) line.push_back(bytes[i]);
    }
#else
    DWORD available = 0, count = 0; HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD const kind = GetFileType(input);
    if (kind == FILE_TYPE_CHAR && GetNumberOfConsoleInputEvents(input, &available) && available) {
        INPUT_RECORD events[64];
        if (ReadConsoleInputA(input, events, std::min<DWORD>(available, 64), &count))
            for (DWORD i = 0; i < count; ++i) {
                if (events[i].EventType != KEY_EVENT || !events[i].Event.KeyEvent.bKeyDown) continue;
                char const ch = events[i].Event.KeyEvent.uChar.AsciiChar;
                if (ch == '\r') { if (!line.empty()) OSD_Dispatch(line.c_str()); line.clear(); }
                else if (ch == '\b') { if (!line.empty()) line.pop_back(); }
                else if (ch >= 32 && ch != 127 && line.size() < 4096) line.push_back(ch);
            }
    } else if (kind == FILE_TYPE_PIPE && PeekNamedPipe(input, nullptr, 0, nullptr, &available, nullptr)) {
        char bytes[1024];
        if (available && ReadFile(input, bytes, std::min<DWORD>(available, sizeof(bytes)), &count, nullptr))
            for (DWORD i = 0; i < count; ++i) {
                if (bytes[i] == '\n') { if (!line.empty()) OSD_Dispatch(line.c_str()); line.clear(); }
                else if (bytes[i] != '\r' && line.size() < 4096) line.push_back(bytes[i]);
            }
    }
#endif
}
int command(osdcmdptr_t p) {
    std::string const name = p->name;
    if (name == "net_status") status();
    else if (name == "net_disconnect") { pendingConnect.clear(); pendingZeroTierNetwork.clear(); disconnect(); }
    else if (name == "net_stoprecord") stopRecord();
    else if (name == "net_record" && p->numparms == 1) record(p->parms[0]);
    else if (name == "net_playback" && p->numparms == 1) play(p->parms[0]);
    else return OSDCMD_SHOWHELP;
    return OSDCMD_OK;
}
int zeroTierSetting(osdcmdptr_t p) {
    if (p->numparms) {
        if (active() || pendingZeroTierHost || !pendingConnect.empty()) {
            OSD_Printf("Disconnect before changing the ZeroTier network.\n"); return OSDCMD_OK;
        }
        std::string const value = p->parms[0];
        if (!value.empty() && (value.size() != 16 || value.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)) {
            OSD_Printf("The ZeroTier network ID must contain 16 hexadecimal digits.\n");
            return OSDCMD_OK;
        }
    }
    return osdcmd_cvar_set(p);
}
}

bool active() { return running || isPlayback; }
bool server() { return running && g_netServer != nullptr; }
bool client() { return running && g_netClient != nullptr; }
bool dedicated() { return isDedicated; }
bool playback() { return isPlayback; }
bool inGame() { return loaded && !loading && (server() || lastApplied); }
bool zeroTier() { return activeZeroTier && active(); }
bool waitingForZeroTier() {
    return zeroTierSelected && (pendingZeroTierHost || !pendingConnect.empty() || !pendingError.empty());
}
std::string zeroTierNetworkId() { return activeZeroTierNetwork; }
bool zeroTierReady() { return zerotier::ready() && serviceNetwork == configuredZeroTierNetwork(); }
std::string zeroTierStatus() { return zeroTierError.empty() ? zerotier::status() : zeroTierError; }
bool selectZeroTier(bool enabled) {
    if ((active() && enabled != activeZeroTier) ||
        ((pendingZeroTierHost || !pendingConnect.empty()) && enabled != zeroTierSelected)) {
        OSD_Printf("Disconnect before changing the connection type.\n");
        return false;
    }
    zeroTierSelected = enabled;
    return true;
}
bool startZeroTier() {
    if (activeZeroTier && active()) return true;
    zerotier::Config config;
    config.game = zerotier::Game::Rednukem;
    config.networkId = configuredZeroTierNetwork();
    std::string error;
    bool result = zerotier::start(config, error);
    zeroTierError = result ? std::string() : error;
    if (result) {
        serviceNetwork = config.networkId;
        Bstrncpyz(zeroTierNetwork, config.networkId.c_str(), sizeof(zeroTierNetwork));
    }
    if (!result && !error.empty()) OSD_Printf("ZeroTier: %s\n", error.c_str());
    return result;
}
bool hostZeroTier(int players, const char* password) {
    if (active()) { OSD_Printf("Disconnect before hosting a new game.\n"); return false; }
    if (!selectZeroTier(true) || !startZeroTier() || !zeroTierReady()) return false;
    setMaxPlayers(players);
    rules::useMenuSettings();
    Bstrncpyz(g_netPassword, password ? password : "", sizeof(g_netPassword));
    g_networkMode = NET_SERVER;
    pendingZeroTierHost = false;
    startup();
    return server();
}
uint32_t tick() { return serverTick; }
uint32_t mapSerial() { return generation; }
int localPlayer() { return dedicated() ? -1 : local; }
int maxPlayers() { return capacity; }
bool connected(int p) { return validSlot(p) && (server() ? slots[p].authenticated : g_player[p].playerquitflag != 0); }
bool playing(int p) { return validSlot(p) && (server() ? slots[p].authenticated && slots[p].ready : g_player[p].playerquitflag != 0); }
uint32_t connectedSince(int p) { return validSlot(p) ? slots[p].joined : 0; }
uint32_t ping(int p) { return server() ? (validSlot(p) && slots[p].peer ? slots[p].peer->roundTripTime : 0) : (g_netClientPeer ? g_netClientPeer->roundTripTime : 0); }
std::string address(int p) {
    char value[128] = {};
    ENetPeer* target = peer(p);
    if (target) enet_address_get_host_ip(&target->address, value, sizeof(value));
    return value;
}
std::string endpoint() { return remote; }
ENetHost* host() { return g_netServer ? g_netServer : g_netClient; }
ENetPeer* peer(int p) { return server() ? (validSlot(p) ? slots[p].peer : nullptr) : g_netClientPeer; }
std::string mapName() { return currentMap; }
void mapLoaded(const char* path) { if (server() && loading && path) loadedMapPath = path; }
void setDedicated(bool value) { isDedicated = value; }
void setMaxPlayers(int value) { capacity = std::max(1, std::min(MAXPLAYERS, value)); }
void send(int p, uint8_t type, const std::vector<uint8_t>& data, bool reliable) {
    if (server()) {
        if (p < 0) { broadcast(type, data, reliable); return; }
        if (validSlot(p) && slots[p].authenticated) {
            transmit(slots[p].peer, type, data, reliable);
            if (p == recordView) remember(type, data);
        }
    } else if (client()) transmit(g_netClientPeer, type, data, reliable);
}
void broadcast(uint8_t type, const std::vector<uint8_t>& data, bool reliable) {
    if (!server()) { send(-1, type, data, reliable); return; }
    for (auto const& slot : slots) if (slot.authenticated && slot.peer) transmit(slot.peer, type, data, reliable);
    remember(type, data);
}
void fail(const std::string& reason) { pendingError = reason; }
void reject(int p, const std::string& reason) {
    if (!validSlot(p) || !slots[p].peer) return;
    netwire::Writer out; out.text(reason, 1024);
    transmit(slots[p].peer, Reject, out.data, true);
    enet_peer_disconnect_later(slots[p].peer, 1);
}
void cancelPasswordPrompt() { passwordRetryAddress.clear(); passwordPromptQueued = false; passwordRetryNetwork.clear(); }
bool passwordRetryPending() { return !passwordRetryAddress.empty(); }
void retryPassword() {
    std::string target = passwordRetryAddress, network = passwordRetryNetwork;
    bool const overlay = passwordRetryZeroTier;
    cancelPasswordPrompt();
    if (!target.empty()) connect(target, overlay, network);
}
void connect(const std::string& target, bool useZeroTier, const std::string& network) {
    cancelPasswordPrompt();
    zeroTierSelected = useZeroTier;
    pendingZeroTierNetwork = useZeroTier ? (network.empty() ? configuredZeroTierNetwork() : network) : std::string();
    pendingConnect = target;
}
void queueMap(const std::string& map, bool restart) {
    if (!server()) { OSD_Printf("Only the server operator can change maps. Use callvote.\n"); return; }
    pendingMap = map; freshMap = restart; mapQueued = true;
}
void contentReady(int p) {
    if (server()) { if (validSlot(p)) slots[p].contentPrepared = true; return; }
    if (!haveWelcome || (loaded && !loading)) return;
    if (rules::cinematicPlaying()) { contentLoadPending = true; return; }
    contentLoadPending = false;
    std::string name = currentMap;
    if (load(name, true, pendingVolume, pendingLevel, pendingSkill)) {
        myconnectindex = screenpeek = local;
        g_player[local].ps->gm = MODE_GAME;
        resetPrediction();
        if (recordWelcomePending) { remember(Welcome, welcomeBytes, recordWelcomeRebase); recordWelcomePending = false; }
        send(-1, Ready, {});
        OSD_Printf("NET content ready map=%s local=%d\n", currentMap.c_str(), local);
    }
    (void)p;
}
void startup() {
    if (!zeroTierOption.empty()) Bstrncpyz(zeroTierNetwork, zeroTierOption.c_str(), sizeof(zeroTierNetwork));
    initialized = true; social::init(); rules::startup(); content::startup(); presence::startup();
    if (g_networkMode == NET_SERVER && pendingConnect.empty()) {
        if (zeroTierSelected && !zeroTierReady()) {
            pendingZeroTierHost = startZeroTier();
            if (!pendingZeroTierHost) fail(zeroTierStatus());
            return;
        }
        activeZeroTier = zeroTierSelected;
        activeZeroTierNetwork = activeZeroTier ? zeroTierNetwork : std::string();
        if (!supportedGame()) { G_GameExit("Authoritative multiplayer requires Duke Nukem 3D or Redneck Rampage game data."); return; }
        ENetAddress address{}; address.host = ENET_HOST_ANY; address.port = activeZeroTier ? zerotier::GamePort : g_netPort;
        // Keep one transport slot available to explain a full gameplay roster.
        g_netServer = activeZeroTier ? zerotier::createHost(&address, capacity + 1, 3, 0, 0)
                                   : enet_host_create(&address, capacity + 1, 3, 0, 0);
        if (!g_netServer) { G_GameExit("Could not create server UDP socket"); return; }
        g_netServer->packetCopies = packetCopies;
        g_netServer->maximumPacketSize = MaxPacketBytes;
        g_netServer->maximumWaitingData = 2 * MaxPacketBytes;
        running = true; local = 0;
        if (!dedicated()) {
            slots[0].authenticated = slots[0].ready = true; slots[0].joined = timerGetTicks();
            std::string name = social::normalizeName(0, szPlayerName);
            Bstrncpyz(g_player[0].user_name, name.c_str(), sizeof(g_player[0].user_name));
            social::join(0);
        }
        std::string name = boardfilename;
        if (name.empty() && !rules::rotation().empty()) name = rules::rotation().front();
        if (name.empty()) {
            int index = ud.m_volume_number * MAXLEVELS + ud.m_level_number;
            if (index >= 0 && index < MAXLEVELS * MAXVOLUMES && g_mapInfo[index].filename) name = g_mapInfo[index].filename;
        }
        pendingMap = name; freshMap = true; mapQueued = true;
        changeMap();
        OSD_Printf("NET server role=%s port=%u capacity=%d protocol=%u\n", dedicated() ? "dedicated" : "listen", g_netPort, capacity, Protocol);
    }
    if (!pendingConnect.empty()) beginConnect();
}
void disconnect(const std::string& reason) {
    zerotier::advertise(nullptr);
    pendingZeroTierHost = false;
    cancelPasswordPrompt();
    presence::detachHost(); rules::shutdown(); content::shutdown(); netDemoStopRecording(); netDemoStopPlayback();
    if (host()) {
        for (size_t p = 0; p < host()->peerCount; ++p) if (host()->peers[p].state == ENET_PEER_STATE_CONNECTED) enet_peer_disconnect_now(&host()->peers[p], 0);
        enet_host_destroy(host());
    }
    g_netServer = g_netClient = nullptr; g_netClientPeer = nullptr;
    running = loaded = isPlayback = haveWelcome = transportConnected = false;
    mapQueued = contentLoadPending = playbackFinished = recordMapPending = false; welcomeBytes.clear(); recordView = -1; recordWelcomePending = false;
    for (auto& slot : slots) slot = Slot{};
    clearHistory(); social::init(); audio::reset(); currentMap.clear(); generation = serverTick = 0;
    myconnectindex = screenpeek = local = 0;
    connecthead = 0; connectpoint2[0] = -1;
    numplayers = ud.multimode = g_mostConcurrentPlayers = 1;
    for (int p = 0; p < MAXPLAYERS; ++p) g_player[p].playerquitflag = p == 0;
    g_networkMode = NET_CLIENT;
    if (initialized && !dedicated()) {
        g_player[0].ps->gm = MODE_MENU; Menu_Open(0); Menu_Change(MENU_MAIN);
    }
    if (!reason.empty()) { lastError = reason; OSD_Printf("NET error: %s\n", reason.c_str()); }
}
void shutdown() { presence::shutdown(); disconnect(); content::shutdown(); zerotier::stop(); initialized = false; }
void pump() {
    zerotier::pump();
    if (!initialized || pumping) return;
    pumping = true;
    if (activeZeroTier && active() && !zerotier::ready() &&
        (zerotier::state() == zerotier::State::Error || zerotier::state() == zerotier::State::Denied))
        fail(zerotier::status());
    if (host()) {
        host()->packetCopies = std::max(1, std::min(8, packetCopies));
        ENetEvent event{}; unsigned budget = 256;
        while (budget-- && host() && enet_host_service(host(), &event, 0) > 0) {
            if (event.type == ENET_EVENT_TYPE_CONNECT) {
                if (server()) {
                    event.peer->data = nullptr;
                    int p = 0;
                    while (p < capacity && (slots[p].peer || slots[p].authenticated)) ++p;
                    if (p == capacity) { netwire::Writer out; out.text("Server full", 1024); transmit(event.peer, Reject, out.data, true); enet_peer_disconnect_later(event.peer, 2); continue; }
                    slots[p] = Slot{}; slots[p].peer = event.peer; slots[p].lastInput = timerGetTicks();
                    event.peer->data = reinterpret_cast<void*>(intptr_t(p + 1));
                    enet_peer_timeout(event.peer, 16, 10000, 30000);
                    netwire::Writer out; out.u32(Protocol); out.text(s_buildRev, 128); transmit(event.peer, Hello, out.data, true);
                } else { transportConnected = true; OSD_Printf("NET transport connected\n"); }
            } else if (event.type == ENET_EVENT_TYPE_RECEIVE) {
                ++received;
                int p = server() ? int(intptr_t(event.peer->data)) - 1 : -1;
                process(p, event.packet->data, event.packet->dataLength, event.peer);
                enet_packet_destroy(event.packet);
            } else if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
                if (server()) remove(int(intptr_t(event.peer->data)) - 1);
                else fail(lastError.empty() ? "Server disconnected" : lastError);
            }
        }
        if (server()) for (int p = 0; p < capacity; ++p) if (slots[p].peer && !slots[p].authenticated && timerGetTicks() - slots[p].lastInput > 15000) reject(p, "Authentication timed out");
    }
    if (client() && !transportConnected && timerGetTicks() - connectStart > 15000)
        fail(activeZeroTier ? "The ZeroTier host did not reply. Check that the game is still running and both players are connected."
            : "No response from server. Check its address and UDP port forwarding; STUN cannot bypass restrictive NAT or CGNAT.");
    if (playback() && !rules::cinematicPlaying()) {
        std::vector<uint8_t> packet; uint32_t time; unsigned budget = 128;
        while (budget-- && netDemoNextPacket(timerGetTicks() - playbackStart, packet, time)) process(-1, packet.data(), packet.size());
        if (!netDemoIsPlaying()) {
            if (!netDemoLastError().empty()) fail(netDemoLastError());
            else {
                if (!playbackFinished) OSD_Printf("NET playback finished tick=%u sequence=%u\n", serverTick, lastApplied);
                playbackFinished = true;
            }
        }
    }
    if (server() && activeZeroTier && loaded) {
        zerotier::Advertisement game;
        game.map = currentMap;
        game.mode = rules::coop() ? "Co-op" : rules::teams() ? "Teams" : "Dukematch";
        game.maxPlayers = capacity;
        game.password = g_netPassword[0] != 0;
        game.protocol = Protocol;
        for (int p = 0; p < MAXPLAYERS; ++p) game.players += connected(p);
        zerotier::advertise(&game);
    }
    content::pump(); presence::pump(); console();
    if (host()) enet_host_flush(host());
    pumping = false;
    if (!pendingError.empty()) {
        std::string error; error.swap(pendingError);
        std::string const retryAddress = passwordRetryAddress;
        std::string const retryNetwork = passwordRetryNetwork;
        bool const retryZeroTier = passwordRetryZeroTier;
        bool const prompt = passwordPromptQueued;
        bool const fatalOverlay = dedicated() && (zeroTier() || zeroTierSelected);
        disconnect(error);
        if (fatalOverlay) { G_GameExit(error.c_str()); return; }
        if (prompt && !dedicated()) {
            passwordRetryAddress = retryAddress; passwordRetryNetwork = retryNetwork;
            passwordRetryZeroTier = retryZeroTier; passwordPromptQueued = true;
        }
    }
}
void gameTick() {
    if (!initialized) return;
    if (pendingZeroTierHost && zeroTierReady()) { pendingZeroTierHost = false; startup(); }
    else if (pendingZeroTierHost && (zerotier::state() == zerotier::State::Error || zerotier::state() == zerotier::State::Denied)) {
        pendingZeroTierHost = false;
        fail(zerotier::status());
    }
    if (passwordPromptQueued) {
        passwordPromptQueued = false;
        Menu_NetworkPasswordPrompt(passwordWasIncorrect);
    }
    presence::applyPendingJoin();
    if (!pendingConnect.empty()) beginConnect();
    if (contentLoadPending && !rules::cinematicPlaying()) contentReady(-1);
    if (mapQueued && server() && !loading) changeMap();
    if (!active() || loading) return;
    double now = timerGetFractionalTicks();
    if (nextTick < now - 250) nextTick = now;
    unsigned budget = 8;
    while (budget-- && now >= nextTick) {
        nextTick += 1000.0 / TickRate;
        if (!server()) G_TickQuotes();
        if (!dedicated() && !playback() && loaded && (server() || lastApplied)) {
            if (RRRA && g_player[local].ps->on_motorcycle) P_GetInputMotorcycle(local);
            else if (RRRA && g_player[local].ps->on_boat) P_GetInputBoat(local);
            else P_GetInput(local);
            Command cmd; cmd.sequence = ++inputSequence; cmd.input = localInput;
            netwire::Writer encoded; writeInput(encoded, cmd);
            netwire::Reader decoded(encoded.data.data(), encoded.data.size()); cmd = readInput(decoded);
            if (g_player[local].ps->gm & (MODE_MENU | MODE_TYPE)) cmd.input = input_t{};
            if (client()) {
                localCommands.push_back(cmd);
                if (localCommands.size() > 64) localCommands.pop_front();
                netwire::Writer out; size_t const count = std::min<size_t>(8, localCommands.size()); out.u8(uint8_t(count));
                for (size_t i = localCommands.size() - count; i < localCommands.size(); ++i) writeInput(out, localCommands[i]);
                send(-1, Input, out.data, false);
                predict(cmd.input);
            } else { slots[local].held = cmd.input; slots[local].applied = cmd.sequence; }
        }
        if (server() && loaded) {
            ++serverTick;
            for (int p = 0; p < MAXPLAYERS; ++p) {
                auto& slot = slots[p];
                if (slot.peer) {
                    if (!slot.commands.empty()) { auto cmd = slot.commands.front(); slot.commands.pop_front(); slot.held = cmd.input; slot.applied = cmd.sequence; }
                    else if (timerGetTicks() - slot.lastInput > 250) slot.held = input_t{};
                }
                inputfifo[movefifoplc & (MOVEFIFOSIZ - 1)][p] = playing(p) ? slot.held : input_t{};
            }
            if (!rules::frozen()) { ud.pause_on = 0; audio::beginTick(); G_DoMoveThings(); audio::endTick(); }
            rules::tick(); social::tick();
            if (now >= nextSnapshot) { snapshots(); nextSnapshot = now + 1000.0 / std::max(1, std::min(30, snapshotRate)); }
        }
        if (server()) ototalclock = totalclock;
    }
}
void draw() {
    if (dedicated()) return;
    rules::draw();
    if (active() && loaded) social::draw(BUTTON(gamefunc_Show_DukeMatch_Scores));
    if (client() && !inGame()) {
        printext256(16, std::max(16, ydim / 2), whitecol, blackcol, "CONNECTING / LOADING SERVER CONTENT", 0);
    }
    if (!lastError.empty() && !active()) printext256(8, std::max(16, ydim / 2), whitecol, blackcol, lastError.c_str(), 1);
}
void status() {
    rules::status();
    unsigned count = 0; for (int p = 0; p < MAXPLAYERS; ++p) count += connected(p);
    OSD_Printf("NET status role=%s tick=%u map_generation=%u players=%u local=%d map=%s state=%s rx=%u tx=%u\n",
        playback() ? "playback" : server() ? (dedicated() ? "dedicated" : "server") : client() ? "client" : "offline",
        serverTick, generation, count, localPlayer(), currentMap.c_str(), inGame() ? "playing" : "loading", received, sent);
    for (int p = 0; p < MAXPLAYERS; ++p) if (connected(p)) {
        auto const& v = *g_player[p].ps;
        OSD_Printf("NET player slot=%d x=%d y=%d z=%d health=%d name=%s ready=%d ack=%u team=%d\n", p, v.pos.x, v.pos.y, v.pos.z,
            (unsigned)v.i < MAXSPRITES ? sprite[v.i].extra : 0, g_player[p].user_name, playing(p), slots[p].applied, g_player[p].pteam);
    }
    if (lastApplied && history[lastApplied % HistorySize].sequence == lastApplied)
        OSD_Printf("NET snapshot tick=%u sequence=%u hash=%08x local=%d\n", serverTick, lastApplied, netSnapshotHash(history[lastApplied % HistorySize].bytes), local);
}
bool record(const char* path) {
    if (!inGame() || playback()) { OSD_Printf("Join a live game before recording.\n"); return false; }
    recordView = dedicated() ? -1 : local;
    if (dedicated()) for (int p = 0; p < MAXPLAYERS; ++p) if (playing(p)) { recordView = p; break; }
    if (recordView < 0) { OSD_Printf("Recording needs an active player's viewpoint.\n"); return false; }
    NetDemoInfo info; info.protocolVersion = Protocol; info.tickRate = TickRate; info.localSlot = recordView; info.mapName = currentMap;
    std::string error;
    if (!netDemoStartRecording(path, info, error)) { OSD_Printf("NET record error: %s\n", error.c_str()); return false; }
    remember(Welcome, server() ? welcome(recordView) : welcomeBytes);
    rememberState();
    if (server()) snapshots();
    else {
        auto const& state = history[lastApplied % HistorySize];
        bool delta = false; std::vector<uint8_t> full;
        if (state.sequence && netSnapshotPack(state.bytes, nullptr, full, delta, error)) {
            netwire::Writer out; out.u32(state.sequence); out.u32(0); out.u32(inputSequence); out.bytes(full.data(), full.size()); remember(Snapshot, out.data);
        }
    }
    OSD_Printf("NET recording %s\n", path); return true;
}
void stopRecord() { netDemoStopRecording(); OSD_Printf("NET recording stopped\n"); }
bool play(const char* path) {
    if (active()) { OSD_Printf("Disconnect before packet demo playback.\n"); return false; }
    if (!supportedGame()) { OSD_Printf("Packet demos require Duke Nukem 3D or Redneck Rampage game data.\n"); return false; }
    NetDemoInfo info; std::string error;
    if (!netDemoStartPlayback(path, info, error)) { OSD_Printf("NET playback error: %s\n", error.c_str()); return false; }
    if (info.protocolVersion != Protocol) { netDemoStopPlayback(); OSD_Printf("Packet demo protocol mismatch\n"); return false; }
    isPlayback = true; playbackFinished = false; local = info.localSlot; playbackStart = timerGetTicks(); lastError.clear();
    OSD_Printf("NET playback %s\n", path); return true;
}
void registerCommands() {
    for (const char* name : {"net_status", "net_disconnect", "net_record", "net_stoprecord", "net_playback"})
        OSD_RegisterFunction(name, name, command);
    static osdcvardata_t vars[] = {
        {"net_packetcopies", "Total UDP datagram copies (1-8)", (void*)&packetCopies, CVAR_INT, 1, 8},
        {"net_snapshotrate", "Server snapshots per second", (void*)&snapshotRate, CVAR_INT, 1, 30},
        {"net_zerotier_network", "Shared ZeroTier network ID (16 hexadecimal digits)", zeroTierNetwork, CVAR_STRING | CVAR_FUNCPTR, 0, sizeof(zeroTierNetwork)},
    };
    for (auto& var : vars) OSD_RegisterCvar(&var, !Bstrcmp(var.name, "net_zerotier_network") ? zeroTierSetting : osdcmd_cvar_set);
    social::registerCommands(); audio::registerCommands(); rules::registerCommands(); presence::registerCommands();
}
bool earlyArguments(int& argc, const char* const*& argv, int& code) {
    std::string error;
    if (!content::earlyArguments(argc, argv, error)) {
        if (!error.empty()) { initprintf("%s\n", error.c_str()); code = 1; return true; }
    }
    bool done = presence::earlyArguments(argc, argv, code);
    if (!done) content::captureArguments(argc, argv);
    return done;
}
bool argument(int argc, const char* const* argv, int& i) {
    if (presence::parseArgument(argc, argv, i)) return true;
    if (rules::parseArgument(argc, argv, i)) { ++i; return true; }
    const char* arg = argv[i]; if (arg[0] != '-') return false;
    if (!Bstrcasecmp(arg, "-zerotier")) {
        ++i;
        if (i < argc && argv[i][0] != '-') {
            if (std::strlen(argv[i]) != 16 || std::strspn(argv[i], "0123456789abcdefABCDEF") != 16)
                G_GameExit("-zerotier requires a 16-digit hexadecimal network ID");
            zeroTierOption = argv[i++];
            Bstrncpyz(zeroTierNetwork, zeroTierOption.c_str(), sizeof(zeroTierNetwork));
        }
        zeroTierSelected = true;
        return true;
    }
    if (!Bstrcasecmp(arg, "-dedicated")) {
        isDedicated = true; g_networkMode = NET_SERVER; g_noSetup = g_noLogo = true; ++i; return true;
    }
    if (!Bstrcasecmp(arg, "-server")) {
        g_networkMode = NET_SERVER; g_noSetup = g_noLogo = true; ++i;
        if (i < argc && argv[i][0] >= '0' && argv[i][0] <= '9') { int n = std::atoi(argv[i++]); if (n < 1 || n > MAXPLAYERS) G_GameExit("Server capacity must be 1..16"); setMaxPlayers(n); }
        return true;
    }
    if (!Bstrcasecmp(arg, "-client") || !Bstrcasecmp(arg, "-connect")) {
        if (++i >= argc) G_GameExit("Missing server address");
        pendingConnect = argv[i++]; g_noSetup = g_noLogo = true; return true;
    }
    if (!Bstrcasecmp(arg, "-port")) {
        if (++i >= argc) G_GameExit("Missing UDP port");
        char* end = nullptr; long value = std::strtol(argv[i++], &end, 10);
        if (!end || *end || value < 1 || value > 65535) G_GameExit("Invalid UDP port");
        g_netPort = uint16_t(value); return true;
    }
    if (!Bstrcasecmp(arg, "-password")) {
        if (++i >= argc || std::strlen(argv[i]) >= sizeof(g_netPassword)) G_GameExit("Missing or overly long server password");
        Bstrncpyz(g_netPassword, argv[i++], sizeof(g_netPassword)); return true;
    }
    return false;
}
} // namespace rnet

#endif // NETCODE_DISABLE
