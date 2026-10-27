// Native client/server wire primitives. No engine types or native struct layouts.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace netwire
{
constexpr uint32_t Magic      = 0x314e424e;  // NBN1
constexpr uint32_t Version    = 9;
constexpr unsigned WeaponCount = 14; // Includes None; valid indices are 0..13.
constexpr size_t   MaxPayload = 16u * 1024u * 1024u + 1024u;
constexpr size_t   HeaderSize = 28;
constexpr uint32_t MaxMapBytes = 16u * 1024u * 1024u;
constexpr uint32_t MapChunkBytes = 16u * 1024u;
constexpr uint32_t MapWindowChunks = 4;
enum Type : uint32_t
{
    Hello = 1,
    Welcome,
    Ready,
    Commands,
    Snapshot,
    SnapshotAck,
    Chat,
    Taunt,
    Reject,
    Profile,
    Event,
    RequestMap,
    MapData,
    ContentManifest,
    ContentReady,
    VoteRequest,
    VoteStatus,
    VoteOptions,
    CinematicState,
    CinematicDone,
    Scoreboard,
    PublicEndpoint, // Optional live discovery metadata; never recorded in demos.
    Intermission, // Final scoreboard and remaining match-end pause.
    TeamChat, // Private team channel; the payload retains the existing Chat layout.
    AddonManifest, // Authenticated addon negotiation precedes content and map loading.
    RequestAddon,
    AddonData,
    AddonReady,
    TypeCount
};
// Build angles wrap at 2048<<16. Normalize before signed game arithmetic.
inline int32_t normalizeTurn(int32_t turn)
{ return int32_t(uint32_t(turn) & 0x07ffffffu); }
inline bool newer(uint32_t a, uint32_t b)
{ return int32_t(a - b) > 0; }
struct Writer
{
    std::vector<uint8_t> data;
    void                 u8(uint8_t v)
    { data.push_back(v); }
    void u16(uint16_t v)
    {
        u8(v);
        u8(v >> 8);
    }
    void u32(uint32_t v)
    {
        u16(v);
        u16(v >> 16);
    }
    void bytes(const void *p, size_t n)
    {
        if (!n)
            return;
        const auto *b = static_cast<const uint8_t *>(p);
        data.insert(data.end(), b, b + n);
    }
    void text(const std::string &s, size_t max)
    {
        const size_t n = s.size() < max ? s.size() : max;
        u16(uint16_t(n));
        bytes(s.data(), n);
    }
};
struct Reader
{
    const uint8_t *data;
    size_t         size, pos = 0;
    bool           ok = true;
    Reader(const void *d, size_t n) : data(static_cast<const uint8_t *>(d)), size(n)
    {
    }
    uint8_t u8()
    {
        if (pos >= size)
        {
            ok = false;
            return 0;
        }
        return data[pos++];
    }
    uint16_t u16()
    {
        uint16_t a = u8();
        return uint16_t(a | (uint16_t(u8()) << 8));
    }
    uint32_t u32()
    {
        uint32_t a = u16();
        return a | (uint32_t(u16()) << 16);
    }
    bool bytes(void *p, size_t n)
    {
        if (n > size - pos)
        {
            ok = false;
            return false;
        }
        if (n)
            std::memcpy(p, data + pos, n);
        pos += n;
        return true;
    }
    std::string text(size_t max)
    {
        size_t n = u16();
        if (n > max || n > size - pos)
        {
            ok = false;
            return {};
        }
        std::string s(reinterpret_cast<const char *>(data + pos), n);
        pos += n;
        if (s.find('\0') != std::string::npos)
            ok = false;
        return s;
    }
    bool end() const
    { return ok && pos == size; }
};
struct Header
{ uint32_t type = 0, session = 0, generation = 0, sequence = 0; };
inline std::vector<uint8_t> encode(const Header &h, const std::vector<uint8_t> &body)
{
    if (body.size() > MaxPayload || h.type < Hello || h.type >= TypeCount)
        return {};
    Writer w;
    w.u32(Magic);
    w.u32(Version);
    w.u32(h.type);
    w.u32(h.session);
    w.u32(h.generation);
    w.u32(h.sequence);
    w.u32(uint32_t(body.size()));
    w.bytes(body.data(), body.size());
    return w.data;
}
inline bool decode(const void *bytes, size_t size, Header &h, Reader &body)
{
    if (size < HeaderSize || size > HeaderSize + MaxPayload)
        return false;
    Reader r(bytes, size);
    if (r.u32() != Magic || r.u32() != Version)
        return false;
    h.type       = r.u32();
    h.session    = r.u32();
    h.generation = r.u32();
    h.sequence   = r.u32();
    uint32_t n   = r.u32();
    if (!r.ok || h.type < Hello || h.type >= TypeCount || n != size - HeaderSize)
        return false;
    body = Reader(static_cast<const uint8_t *>(bytes) + HeaderSize, n);
    return true;
}
struct MapRequestPayload
{
    uint32_t slotGeneration = 0, mapCRC = 0, offset = 0;
};
inline bool validMapRequest(const MapRequestPayload &request)
{
    return request.offset <= MaxMapBytes && request.offset % MapChunkBytes == 0;
}
inline bool writeMapRequest(Writer &w, const MapRequestPayload &request)
{
    if (!validMapRequest(request))
        return false;
    w.u32(request.slotGeneration);
    w.u32(request.mapCRC);
    w.u32(request.offset);
    return true;
}
inline bool readMapRequest(Reader &r, MapRequestPayload &request)
{
    MapRequestPayload candidate;
    candidate.slotGeneration = r.u32();
    candidate.mapCRC = r.u32();
    candidate.offset = r.u32();
    if (!r.end() || !validMapRequest(candidate))
        return false;
    request = candidate;
    return true;
}
struct MapDataPayload
{
    uint32_t mapCRC = 0, totalBytes = 0, offset = 0, length = 0;
    // Borrowed packet bytes; valid only while the Reader's backing storage lives.
    const uint8_t *data = nullptr;
};
inline bool validMapData(const MapDataPayload &chunk)
{
    if (!chunk.totalBytes || chunk.totalBytes > MaxMapBytes ||
        chunk.offset >= chunk.totalBytes || chunk.offset % MapChunkBytes != 0)
        return false;
    const uint32_t remaining = chunk.totalBytes - chunk.offset;
    return chunk.length == (remaining < MapChunkBytes ? remaining : MapChunkBytes);
}
inline bool writeMapData(Writer &w, const MapDataPayload &chunk)
{
    if (!validMapData(chunk) || !chunk.data)
        return false;
    w.u32(chunk.mapCRC);
    w.u32(chunk.totalBytes);
    w.u32(chunk.offset);
    w.u32(chunk.length);
    w.bytes(chunk.data, chunk.length);
    return true;
}
inline bool readMapData(Reader &r, MapDataPayload &chunk)
{
    MapDataPayload candidate;
    candidate.mapCRC = r.u32();
    candidate.totalBytes = r.u32();
    candidate.offset = r.u32();
    candidate.length = r.u32();
    if (!r.ok || !validMapData(candidate) || candidate.length != r.size - r.pos)
        return false;
    candidate.data = r.data + r.pos;
    r.pos += candidate.length;
    chunk = candidate;
    return true;
}
// Session rules accompany each map bootstrap, independently of the legacy
// GAMEOPTIONS layout used by original input demos and saved games.
struct ServerRules
{
    uint32_t timeLimitMinutes = 0, scoreLimit = 0, maxPlayers = 8, autoAim = 1;
    uint32_t rotationIndex = 0, rotationCount = 0;
    uint32_t mapVoteEnabled = 1;
    // Effective permissions: only dedicated Bloodbath overrides these defaults.
    uint32_t allowHolsterWeapon = 1, allowChaseView = 1;
    // Zero means unlimited; only dedicated cooperative servers set a limit.
    uint32_t coopLives = 0;
};
constexpr size_t ServerRulesBytes = 10 * sizeof(uint32_t);
inline void writeServerRules(Writer& w, const ServerRules& rules)
{
    w.u32(rules.timeLimitMinutes); w.u32(rules.scoreLimit);
    w.u32(rules.maxPlayers); w.u32(rules.autoAim);
    w.u32(rules.rotationIndex); w.u32(rules.rotationCount);
    w.u32(rules.mapVoteEnabled);
    w.u32(rules.allowHolsterWeapon); w.u32(rules.allowChaseView);
    w.u32(rules.coopLives);
}
inline bool readServerRules(Reader& r, ServerRules& rules)
{
    ServerRules candidate;
    candidate.timeLimitMinutes = r.u32(); candidate.scoreLimit = r.u32();
    candidate.maxPlayers = r.u32(); candidate.autoAim = r.u32();
    candidate.rotationIndex = r.u32(); candidate.rotationCount = r.u32();
    candidate.mapVoteEnabled = r.u32();
    candidate.allowHolsterWeapon = r.u32(); candidate.allowChaseView = r.u32();
    candidate.coopLives = r.u32();
    if (!r.ok || candidate.timeLimitMinutes > 10080 || candidate.scoreLimit > INT32_MAX
        || !candidate.maxPlayers || candidate.maxPlayers > 8 || candidate.autoAim > 3
        || candidate.rotationCount > 256 || candidate.mapVoteEnabled > 1
        || candidate.allowHolsterWeapon > 1 || candidate.allowChaseView > 1
        || candidate.coopLives > 255
        || (candidate.rotationCount ? candidate.rotationIndex >= candidate.rotationCount : candidate.rotationIndex != 0))
        return false;
    rules = candidate;
    return true;
}
enum class VoteCommand : uint32_t { List = 1, Map, Restart, Next, Yes, No, QueryStatus, Shuffle = 8 };
enum class VoteState : uint32_t { Idle, Active, Passed, Failed, Cancelled };
constexpr size_t VoteNameBytes = 255;
inline bool voteText(const std::string& text, size_t maximum)
{
    if (text.size() > maximum) return false;
    for (unsigned char c : text) if (c < 32 || c == 127) return false;
    return true;
}
struct VoteRequestPayload
{
    VoteCommand command = VoteCommand::QueryStatus;
    uint32_t id = 0;
    std::string map;
};
inline bool validVoteRequest(const VoteRequestPayload& request)
{
    uint32_t const command = uint32_t(request.command);
    bool const ballot = request.command == VoteCommand::Yes || request.command == VoteCommand::No;
    return command >= 1 && command <= 8 && (ballot ? request.id != 0 : request.id == 0)
        && voteText(request.map, VoteNameBytes)
        && (request.command == VoteCommand::Map ? !request.map.empty() : request.map.empty());
}
inline bool writeVoteRequest(Writer& w, const VoteRequestPayload& request)
{
    if (!validVoteRequest(request)) return false;
    w.u32(uint32_t(request.command)); w.u32(request.id); w.text(request.map, VoteNameBytes);
    return true;
}
inline bool readVoteRequest(Reader& r, VoteRequestPayload& request)
{
    VoteRequestPayload candidate;
    candidate.command = VoteCommand(r.u32()); candidate.id = r.u32(); candidate.map = r.text(VoteNameBytes);
    if (!r.end() || !validVoteRequest(candidate)) return false;
    request = candidate;
    return true;
}
struct VoteStatusPayload
{
    uint32_t id = 0;
    VoteState state = VoteState::Idle;
    uint32_t action = 0, remainingMs = 0, eligible = 0, yes = 0, no = 0, required = 0; // map=1, restart=2, next=3, shuffle=4
    uint32_t caller = 0, canVote = 0, voted = 0, cooldownMs = 0, rejected = 0;
    std::string target, notice;
};
inline bool validVoteStatus(const VoteStatusPayload& s)
{
    if (uint32_t(s.state) > 4 || s.action > 4 || s.remainingMs > 30000 || s.eligible > 8
        || s.yes > s.eligible || s.no > s.eligible - s.yes || s.required != (s.eligible * 51 + 99) / 100
        || s.caller >= 8 || s.canVote > 1 || s.voted > 1 || s.cooldownMs > 120000 || s.rejected > 1
        || !voteText(s.target, VoteNameBytes) || !voteText(s.notice, 160)) return false;
    if (s.state == VoteState::Idle)
        return !s.id && !s.action && !s.eligible && !s.remainingMs && !s.canVote && !s.voted && s.target.empty();
    return s.id && s.action && s.eligible && !s.target.empty()
        && (s.state == VoteState::Active || (!s.remainingMs && !s.canVote));
}
inline bool writeVoteStatus(Writer& w, const VoteStatusPayload& s)
{
    if (!validVoteStatus(s)) return false;
    w.u32(s.id); w.u32(uint32_t(s.state)); w.u32(s.action); w.u32(s.remainingMs);
    w.u32(s.eligible); w.u32(s.yes); w.u32(s.no); w.u32(s.required);
    w.u32(s.caller); w.u32(s.canVote); w.u32(s.voted); w.u32(s.cooldownMs); w.u32(s.rejected);
    w.text(s.target, VoteNameBytes); w.text(s.notice, 160);
    return true;
}
inline bool readVoteStatus(Reader& r, VoteStatusPayload& s)
{
    VoteStatusPayload candidate;
    candidate.id = r.u32(); candidate.state = VoteState(r.u32()); candidate.action = r.u32(); candidate.remainingMs = r.u32();
    candidate.eligible = r.u32(); candidate.yes = r.u32(); candidate.no = r.u32(); candidate.required = r.u32();
    candidate.caller = r.u32(); candidate.canVote = r.u32(); candidate.voted = r.u32(); candidate.cooldownMs = r.u32(); candidate.rejected = r.u32();
    candidate.target = r.text(VoteNameBytes); candidate.notice = r.text(160);
    if (!r.end() || !validVoteStatus(candidate)) return false;
    s = candidate;
    return true;
}
struct VoteOptionsPayload
{
    uint32_t enabled = 0, actions = 0, listRequested = 0; // action bits: map=1, restart=2, next=4, shuffle=8
    std::vector<std::string> maps;
};
inline bool validVoteOptions(const VoteOptionsPayload& options)
{
    if (options.enabled > 1 || options.actions > 15 || options.listRequested > 1 || options.maps.size() > 256
        || (!options.enabled && (options.actions || !options.maps.empty()))
        || (bool(options.actions & 1) != !options.maps.empty())) return false;
    for (const auto& map : options.maps) if (map.empty() || !voteText(map, VoteNameBytes)) return false;
    return true;
}
inline bool writeVoteOptions(Writer& w, const VoteOptionsPayload& options)
{
    if (!validVoteOptions(options)) return false;
    w.u32(options.enabled); w.u32(options.actions); w.u32(options.listRequested); w.u32(uint32_t(options.maps.size()));
    for (const auto& map : options.maps) w.text(map, VoteNameBytes);
    return true;
}
inline bool readVoteOptions(Reader& r, VoteOptionsPayload& options)
{
    VoteOptionsPayload candidate;
    candidate.enabled = r.u32(); candidate.actions = r.u32(); candidate.listRequested = r.u32();
    uint32_t const count = r.u32();
    if (!r.ok || count > 256) return false;
    for (uint32_t n = 0; n < count && r.ok; ++n) candidate.maps.push_back(r.text(VoteNameBytes));
    if (!r.end() || !validVoteOptions(candidate)) return false;
    options = candidate;
    return true;
}
struct Command
{
    uint32_t sequence = 0;
    uint32_t viewTick = 0;
    uint16_t viewFraction = 0;
    uint8_t  sync = 0, buttons = 0, use = 0, weapon = 0;
    uint16_t keys    = 0;
    int16_t  forward = 0, strafe = 0;
    int32_t  turn = 0, mlook = 0;
};
inline void writeCommand(Writer &w, const Command &c)
{
    w.u32(c.sequence);
    w.u32(c.viewTick);
    w.u16(c.viewFraction);
    w.u8(c.sync);
    w.u16(uint16_t(c.forward));
    w.u32(uint32_t(c.turn));
    w.u16(uint16_t(c.strafe));
    w.u8(c.buttons);
    w.u16(c.keys);
    w.u8(c.use);
    w.u8(c.weapon);
    w.u32(uint32_t(c.mlook));
}
inline bool readCommand(Reader &r, Command &c)
{
    c.sequence = r.u32();
    c.viewTick = r.u32();
    c.viewFraction = r.u16();
    c.sync     = r.u8();
    c.forward  = int16_t(r.u16());
    c.turn     = int32_t(r.u32());
    c.strafe   = int16_t(r.u16());
    c.buttons  = r.u8();
    c.keys     = r.u16();
    c.use      = r.u8();
    c.weapon   = r.u8();
    c.mlook    = int32_t(r.u32());
    // Pause/quit/restart are local/server controls, never remotely executable.
    return r.ok && c.sequence != 0 && c.forward >= -2048 && c.forward <= 2048 && c.strafe >= -2048 && c.strafe <= 2048 && c.mlook >= -(127 << 14)
           && c.mlook <= (127 << 14) && c.weapon < WeaponCount && (c.buttons & ~63) == 0 && (c.use & ~15) == 0 && (c.sync & ~63) == 0 && (c.keys & 0x7000) == 0;
}
// A finite receive window prevents duplicates/replays and excessive client lead.
struct CommandQueue
{
    struct Entry
    {
        Command command;
        bool    valid = false;
    } entries[256];
    uint32_t acknowledged = 0;
    bool     push(const Command &c)
    {
        uint32_t distance = c.sequence - acknowledged;
        if (!distance || distance > 255)
            return false;
        Entry &e = entries[c.sequence & 255];
        if (e.valid && e.command.sequence == c.sequence)
            return false;
        e.command = c;
        e.valid   = true;
        return true;
    }
    // Called only after a transport-observed input outage. Keep the old ACK
    // until the recovery command has actually been selected for a server tick.
    bool resync(const Command &c)
    {
        if (!newer(c.sequence, acknowledged))
            return false;
        for (auto &e : entries) e.valid = false;
        Entry &e  = entries[c.sequence & 255];
        e.command = c;
        e.valid   = true;
        return true;
    }
    bool pop(Command &c)
    {
        Entry   *oldest   = nullptr;
        uint32_t distance = UINT32_MAX;
        for (auto &e : entries)
            if (e.valid && newer(e.command.sequence, acknowledged) && e.command.sequence - acknowledged < distance)
            {
                oldest   = &e;
                distance = e.command.sequence - acknowledged;
            }
        if (!oldest)
            return false;
        c             = oldest->command;
        oldest->valid = false;
        acknowledged  = c.sequence;
        return true;
    }
};
}  // namespace netwire
