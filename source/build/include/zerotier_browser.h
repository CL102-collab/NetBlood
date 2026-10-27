// Bounded, game-specific discovery carried over the companion's generic UDP API.
#ifndef BUILD_ZEROTIER_BROWSER_H
#define BUILD_ZEROTIER_BROWSER_H
#include "zerotier.h"
#include "zerotier_protocol.h"

#include <algorithm>
#include <map>
#include <utility>

namespace ztbrowse
{
using Bytes                     = ztipc::Bytes;
constexpr size_t   MaxServers   = 128;
constexpr uint64_t ExpiryMillis = 6500;
constexpr uint64_t QueryMillis  = 1500;
struct Query
{
    uint8_t  game  = 0;
    uint32_t nonce = 0;
};
struct Reply
{
    Query                   query;
    zerotier::Advertisement game;
    uint16_t                port = zerotier::GamePort;
};
inline bool textValid(const std::string &text, size_t maximum)
{
    if (text.empty() || text.size() > maximum)
        return false;
    for (unsigned char ch : text)
        if (ch < 32 || ch > 126)
            return false;
    return true;
}
inline bool headerValid(const uint8_t *p, size_t n, uint8_t type)
{ return p && n >= 12 && !std::memcmp(p, "ZTGB", 4) && p[4] == 1 && p[5] == type && p[6] >= 1 && p[6] <= 2; }
inline Bytes query(uint8_t game, uint32_t nonce)
{
    if (game < 1 || game > 2 || !nonce)
        return {};
    Bytes b { 'Z', 'T', 'G', 'B', 1, 1, game, 0 };
    ztipc::put32(b, nonce);
    return b;
}
inline bool readQuery(const uint8_t *p, size_t n, Query &out)
{
    if (!headerValid(p, n, 1) || n != 12 || p[7] || !ztipc::get32(p + 8))
        return false;
    Query next;
    next.game  = p[6];
    next.nonce = ztipc::get32(p + 8);
    out        = next;
    return true;
}
inline Bytes reply(const Query &request, const zerotier::Advertisement &game)
{
    if (request.game < 1 || request.game > 2 || !request.nonce || game.players < 0 || game.maxPlayers < 1 || game.maxPlayers > 64 || game.players > game.maxPlayers
        || !game.protocol || !textValid(game.map, 64) || !textValid(game.mode, 16))
        return {};
    Bytes b { 'Z', 'T', 'G', 'B', 1, 2, request.game, uint8_t(game.password ? 1 : 0) };
    ztipc::put32(b, request.nonce);
    ztipc::put32(b, game.protocol);
    ztipc::put16(b, zerotier::GamePort);
    b.push_back(uint8_t(game.players));
    b.push_back(uint8_t(game.maxPlayers));
    b.push_back(uint8_t(game.map.size()));
    b.push_back(uint8_t(game.mode.size()));
    b.insert(b.end(), game.map.begin(), game.map.end());
    b.insert(b.end(), game.mode.begin(), game.mode.end());
    return b;
}
inline bool readReply(const uint8_t *p, size_t n, Reply &out)
{
    if (!headerValid(p, n, 2) || n < 24 || n > 102 || (p[7] & ~1) || !ztipc::get32(p + 8) || !ztipc::get32(p + 12) || ztipc::get16(p + 16) != zerotier::GamePort
        || p[19] < 1 || p[19] > 64 || p[18] > p[19] || size_t(22 + p[20] + p[21]) != n)
        return false;
    Reply next;
    next.query.game      = p[6];
    next.query.nonce     = ztipc::get32(p + 8);
    next.game.protocol   = ztipc::get32(p + 12);
    next.port            = ztipc::get16(p + 16);
    next.game.players    = p[18];
    next.game.maxPlayers = p[19];
    next.game.password   = p[7] != 0;
    next.game.map.assign(reinterpret_cast<const char *>(p + 22), p[20]);
    next.game.mode.assign(reinterpret_cast<const char *>(p + 22 + p[20]), p[21]);
    if (!textValid(next.game.map, 64) || !textValid(next.game.mode, 16))
        return false;
    out = std::move(next);
    return true;
}
class Browser
{
    struct Entry
    {
        zerotier::Server server;
        uint64_t         seen = 0;
    };
    std::map<std::string, Entry>               entries;
    std::vector<std::pair<uint32_t, uint64_t>> queries;
    std::vector<zerotier::Server>              visible;
    uint8_t                                    game;
    void                                       publish()
    {
        visible.clear();
        for (const auto &entry : entries) visible.push_back(entry.second.server);
    }

  public:
    explicit Browser(zerotier::Game game) : game(uint8_t(game))
    {
    }
    void clear()
    {
        entries.clear();
        queries.clear();
        visible.clear();
    }
    void sent(uint32_t nonce, uint64_t now)
    {
        queries.erase(std::remove_if(queries.begin(), queries.end(),
                                     [=](const std::pair<uint32_t, uint64_t> &q)
                                     {
                                         return now - q.second > ExpiryMillis;
                                     }),
                      queries.end());
        if (queries.size() >= 8)
            queries.erase(queries.begin());
        queries.push_back({ nonce, now });
    }
    bool receive(const std::string &address, const uint8_t *p, size_t n, uint64_t now)
    {
        Reply next;
        if (!readReply(p, n, next) || next.query.game != game || address.empty() || address.size() > 15)
            return false;
        auto found = std::find_if(queries.begin(), queries.end(),
                                  [&](const std::pair<uint32_t, uint64_t> &q)
                                  {
                                      return q.first == next.query.nonce;
                                  });
        if (found == queries.end() || now < found->second || now - found->second > ExpiryMillis)
            return false;
        std::string key   = address + ":" + std::to_string(next.port);
        auto        entry = entries.find(key);
        if (entry == entries.end() && entries.size() >= MaxServers)
            return false;
        Entry value;
        static_cast<zerotier::Advertisement &>(value.server) = std::move(next.game);
        value.server.address                                 = address;
        value.server.port                                    = next.port;
        value.server.ping                                    = int(now - found->second);
        value.seen                                           = now;
        entries[key]                                         = std::move(value);
        publish();
        return true;
    }
    void expire(uint64_t now)
    {
        bool changed = false;
        for (auto it = entries.begin(); it != entries.end();)
            if (now < it->second.seen || now - it->second.seen > ExpiryMillis)
            {
                it      = entries.erase(it);
                changed = true;
            }
            else
                ++it;
        if (changed)
            publish();
    }
    const std::vector<zerotier::Server> &servers() const
    { return visible; }
};
}  // namespace ztbrowse
#endif
