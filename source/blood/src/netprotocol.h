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
constexpr uint32_t Version    = 1;
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
struct Command
{
    uint32_t sequence = 0;
    uint8_t  sync = 0, buttons = 0, use = 0, weapon = 0;
    uint16_t keys    = 0;
    int16_t  forward = 0, strafe = 0;
    int32_t  turn = 0, mlook = 0;
};
inline void writeCommand(Writer &w, const Command &c)
{
    w.u32(c.sequence);
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
