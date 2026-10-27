#include "zerotier.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>
#define CHECK(test)                                                                                       \
    do                                                                                                    \
    {                                                                                                     \
        if (!(test))                                                                                      \
        {                                                                                                 \
            std::fprintf(stderr, "FAIL %d: %s status=%s\n", __LINE__, #test, zerotier::status().c_str()); \
            std::exit(1);                                                                                 \
        }                                                                                                 \
    } while (0)
uint64_t now()
{ return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); }
int main(int argc, char **argv)
{
    CHECK(argc == 4);
    bool             server = !std::strcmp(argv[1], "host");
    zerotier::Config config;
    config.networkId    = "1234567890abcdef";
    config.helperPath   = argv[2];
    config.identityPath = argv[3];
    std::string error;
    if (!server)
    {
        auto bad      = config;
        bad.networkId = "invalid";
        CHECK(!zerotier::start(bad, error));
        CHECK(zerotier::state() == zerotier::State::Error && !error.empty());
    }
    CHECK(zerotier::start(config, error));
    CHECK(error.empty());
    uint64_t deadline = now() + 4000;
    while (!zerotier::ready() && now() < deadline)
    {
        zerotier::pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(zerotier::ready());
    CHECK(zerotier::localAddress() == (server ? "10.7.0.1" : "10.7.0.2"));
    ENetAddress bound = {};
    bound.port        = zerotier::GamePort;
    ENetHost *host    = zerotier::createHost(server ? &bound : nullptr, 8, 3, 0, 0);
    CHECK(host && host->socket == ENET_SOCKET_NULL);
    zerotier::Advertisement ad;
    ad.map        = "hotjoin.map";
    ad.mode       = "COOP";
    ad.password   = true;
    ad.players    = 8;
    ad.maxPlayers = 8;
    ad.protocol   = 8;
    if (server)
        zerotier::advertise(&ad);
    else
        zerotier::browse(true);
    bool                 connected = false, received = false, updated = false;
    uint64_t             completed = 0;
    std::vector<uint8_t> expected(256 * 1024);
    for (size_t i = 0; i < expected.size(); i++) expected[i] = uint8_t(i * 37 + 9);
    deadline = now() + 20000;
    while (now() < deadline)
    {
        zerotier::pump();
        if (!server && !connected && !zerotier::servers().empty())
        {
            auto found = zerotier::servers()[0];
            CHECK(found.players == 8 && found.maxPlayers == 8 && found.password && found.ping >= 0 && found.map == "hotjoin.map" && found.mode == "COOP");
            CHECK(found.address == "10.7.0.1" && found.port == zerotier::GamePort);
            ENetAddress address = {};
            CHECK(!enet_address_set_host(&address, &found.address[0]));
            address.port = found.port;
            CHECK(enet_host_connect(host, &address, 3, 8));
            connected = true;
        }
        ENetEvent event;
        while (enet_host_service(host, &event, 1) > 0)
        {
            if (event.type == ENET_EVENT_TYPE_CONNECT && server)
            {
                auto *packet = enet_packet_create(expected.data(), expected.size(), ENET_PACKET_FLAG_RELIABLE);
                CHECK(packet);
                CHECK(!enet_peer_send(event.peer, 1, packet));
            }
            if (event.type != ENET_EVENT_TYPE_RECEIVE)
                continue;
            if (server)
            {
                CHECK(event.packet->dataLength == 8 && !std::memcmp(event.packet->data, "complete", 8));
                received   = true;
                ad.players = 7;
                zerotier::advertise(&ad);
                completed = now();
            }
            else
            {
                CHECK(event.packet->dataLength == expected.size());
                CHECK(!std::memcmp(event.packet->data, expected.data(), expected.size()));
                received         = true;
                char  complete[] = "complete";
                auto *packet     = enet_packet_create(complete, 8, ENET_PACKET_FLAG_RELIABLE);
                CHECK(packet);
                CHECK(!enet_peer_send(event.peer, 0, packet));
                enet_host_flush(host);
            }
            enet_packet_destroy(event.packet);
        }
        if (!server && received && !zerotier::servers().empty() && zerotier::servers()[0].players == 7)
        {
            updated = true;
            break;
        }
        if (server && received && now() - completed > 3500)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(received);
    CHECK(server || updated);
    enet_host_destroy(host);
    // Reusing a logical socket must not let the previous host's packets enter.
    host = zerotier::createHost(server ? &bound : nullptr, 8, 3, 0, 0);
    CHECK(host);
    enet_host_destroy(host);
    zerotier::advertise(nullptr);
    zerotier::browse(false);
    zerotier::stop();
    CHECK(zerotier::state() == zerotier::State::Stopped && !zerotier::ready());
    std::puts(server ? "ZeroTier service host passed" : "ZeroTier service client/browser passed");
}
