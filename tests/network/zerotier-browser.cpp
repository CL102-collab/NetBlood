#include "zerotier_browser.h"

#include <cstdio>
#include <cstdlib>
#define CHECK(test)                                                 \
    do                                                              \
    {                                                               \
        if (!(test))                                                \
        {                                                           \
            std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #test); \
            std::exit(1);                                           \
        }                                                           \
    } while (0)
int main()
{
    ztipc::Token token {};
    for (size_t i = 0; i < token.size(); i++) token[i] = uint8_t(i * 7 + 1);
    CHECK(ztipc::parseToken(ztipc::tokenText(token), token));
    ztipc::Token unchanged = token;
    CHECK(!ztipc::parseToken("bad", unchanged) && unchanged == token);
    ztipc::Message original;
    original.type         = ztipc::Type::Send;
    original.socket       = 3;
    original.request      = 42;
    original.payload      = { 0, 1, 2, 0, 255, 128 };
    auto           packet = ztipc::encode(original, token);
    ztipc::Message parsed;
    CHECK(ztipc::decode(packet.data(), packet.size(), token, parsed));
    CHECK(parsed.payload == original.payload && parsed.request == 42 && parsed.socket == 3);
    for (size_t size = 0; size < packet.size(); size++) CHECK(!ztipc::decode(packet.data(), size, token, parsed));
    for (size_t byte = 8; byte < 24; byte++)
    {
        auto corrupt = packet;
        corrupt[byte] ^= 1;
        CHECK(!ztipc::decode(corrupt.data(), corrupt.size(), token, parsed));
    }
    packet[30] = 1;
    CHECK(!ztipc::decode(packet.data(), packet.size(), token, parsed));
    original.payload.resize(ztipc::MaxPayload + 1);
    CHECK(ztipc::encode(original, token).empty());

    ztbrowse::Query request;
    request.game          = 1;
    request.nonce         = 1234;
    auto            query = ztbrowse::query(request.game, request.nonce);
    ztbrowse::Query decoded;
    CHECK(ztbrowse::readQuery(query.data(), query.size(), decoded) && decoded.nonce == 1234);
    query.push_back(0);
    CHECK(!ztbrowse::readQuery(query.data(), query.size(), decoded));
    zerotier::Advertisement game;
    game.players    = 8;
    game.maxPlayers = 8;
    game.password   = true;
    game.protocol   = 8;
    game.map        = "custom.map";
    game.mode       = "BLOODBATH";
    packet          = ztbrowse::reply(request, game);
    CHECK(!packet.empty());
    ztbrowse::Reply reply;
    CHECK(ztbrowse::readReply(packet.data(), packet.size(), reply) && reply.game.players == 8 && reply.game.password);
    for (size_t size = 0; size < packet.size(); size++)
    {
        reply.game.map = "unchanged";
        CHECK(!ztbrowse::readReply(packet.data(), size, reply));
        CHECK(reply.game.map == "unchanged");
    }
    auto corrupt = packet;
    corrupt[18]  = 9;
    CHECK(!ztbrowse::readReply(corrupt.data(), corrupt.size(), reply));
    corrupt    = packet;
    corrupt[7] = 2;
    CHECK(!ztbrowse::readReply(corrupt.data(), corrupt.size(), reply));
    corrupt     = packet;
    corrupt[22] = 1;
    CHECK(!ztbrowse::readReply(corrupt.data(), corrupt.size(), reply));
    corrupt     = packet;
    corrupt[20] = 65;
    CHECK(!ztbrowse::readReply(corrupt.data(), corrupt.size(), reply));

    ztbrowse::Browser browser(zerotier::Game::NBlood);
    CHECK(!browser.receive("10.0.0.1", packet.data(), packet.size(), 100));  // No outstanding query.
    browser.sent(request.nonce, 100);
    CHECK(browser.receive("10.0.0.1", packet.data(), packet.size(), 120));
    CHECK(browser.servers().size() == 1 && browser.servers()[0].players == 8 && browser.servers()[0].maxPlayers == 8);
    CHECK(browser.servers()[0].password && browser.servers()[0].ping == 20);  // Full games remain visible.
    game.players = 7;
    game.map     = "next.map";
    packet       = ztbrowse::reply(request, game);
    CHECK(browser.receive("10.0.0.1", packet.data(), packet.size(), 130));
    CHECK(browser.servers().size() == 1 && browser.servers()[0].players == 7 && browser.servers()[0].map == "next.map");
    request.game    = 2;
    game.players    = 16;
    game.maxPlayers = 16;
    packet          = ztbrowse::reply(request, game);
    CHECK(!browser.receive("10.0.0.2", packet.data(), packet.size(), 130));
    ztbrowse::Browser red(zerotier::Game::Rednukem);
    red.sent(request.nonce, 100);
    CHECK(red.receive("10.0.0.2", packet.data(), packet.size(), 130) && red.servers()[0].maxPlayers == 16);
    request.game    = 1;
    game.players    = 8;
    game.maxPlayers = 8;
    packet          = ztbrowse::reply(request, game);
    for (size_t i = 2; i <= ztbrowse::MaxServers; i++) CHECK(browser.receive("10.0.1." + std::to_string(i), packet.data(), packet.size(), 150));
    CHECK(browser.servers().size() == ztbrowse::MaxServers);
    CHECK(!browser.receive("10.0.2.1", packet.data(), packet.size(), 150));
    browser.expire(150 + ztbrowse::ExpiryMillis + 1);
    CHECK(browser.servers().empty());
    CHECK(!browser.receive("10.0.2.1", packet.data(), packet.size(), 150 + ztbrowse::ExpiryMillis + 1));
    uint32_t random = 0x12345;
    for (size_t test = 0; test < 30000; test++)
    {
        auto mutation = packet;
        random        = random * 1664525 + 1013904223;
        for (size_t i = 0; i < 1 + (test % 4); i++)
        {
            random = random * 1664525 + 1013904223;
            mutation[random % mutation.size()] ^= uint8_t(random >> 16);
        }
        ztbrowse::readReply(mutation.data(), mutation.size(), reply);
        ztipc::decode(mutation.data(), mutation.size(), token, parsed);
    }
    std::puts("ZeroTier IPC/discovery codec, full hosts, game isolation, RTT, expiry and malformed-packet tests passed");
}
