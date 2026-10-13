// SPDX-License-Identifier: GPL-2.0-or-later
// Production STUN discovery on real ENet sockets with local mock servers.
// No Internet, rendezvous service, game data, or firewall privileges required.
#include "netstun_client.h"
#include "netstun.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <thread>
#include <vector>

namespace
{
unsigned checks = 0;
void check(bool okay, const char* expression, int line)
{
    ++checks;
    if (!okay)
    {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, expression);
        std::exit(1);
    }
}
#define CHECK(value) check(bool(value), #value, __LINE__)
using Bytes = std::vector<uint8_t>;
ENetAddress loopback(uint16_t port = 0)
{
    ENetAddress address{};
    char host[] = "127.0.0.1";
    CHECK(enet_address_set_host(&address, host) == 0);
    address.port = port;
    return address;
}
bool same(const ENetAddress& left, const ENetAddress& right)
{ return left.port == right.port && in6_equal(left.host, right.host); }
struct Socket
{
    ENetSocket socket = ENET_SOCKET_NULL;
    ENetAddress address{};
    Socket()
    {
        socket = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
        CHECK(socket != ENET_SOCKET_NULL);
        auto bind = loopback();
        CHECK(enet_socket_set_option(socket, ENET_SOCKOPT_IPV6_V6ONLY, 0) == 0);
        CHECK(enet_socket_bind(socket, &bind) == 0);
        CHECK(enet_socket_set_option(socket, ENET_SOCKOPT_NONBLOCK, 1) == 0);
        CHECK(enet_socket_set_option(socket, ENET_SOCKOPT_RCVBUF, 4 * 1024 * 1024) == 0);
        CHECK(enet_socket_get_address(socket, &address) == 0);
    }
    ~Socket() { enet_socket_destroy(socket); }
    bool receive(Bytes& bytes, ENetAddress& source)
    {
        uint8_t data[2048];
        ENetBuffer buffer; buffer.data = data; buffer.dataLength = sizeof(data);
        const int length = enet_socket_receive(socket, &source, &buffer, 1);
        CHECK(length >= 0);
        if (!length) return false;
        bytes.assign(data, data + length);
        return true;
    }
    void send(const ENetAddress& destination, const Bytes& bytes)
    {
        auto to = destination;
        ENetBuffer buffer; buffer.data = const_cast<uint8_t*>(bytes.data()); buffer.dataLength = bytes.size();
        CHECK(enet_socket_send(socket, &to, &buffer, 1) == int(bytes.size()));
    }
};
struct Endpoint;
std::map<ENetHost*, Endpoint*> endpoints;
struct Endpoint
{
    ENetHost* host = nullptr;
    netstun::Client discovery;
    ENetAddress address{};
    std::vector<Bytes> raw;
    unsigned consumed = 0;
    bool connected = false;
    ENetPeer* peer = nullptr;
    std::vector<Bytes> received;
    Endpoint()
    {
        auto bind = loopback();
        host = enet_host_create(&bind, 8, 3, 0, 0);
        CHECK(host);
        host->checksum = enet_crc32;
        host->packetCopies = 3;
        address = host->address;
        CHECK(address.port);
        CHECK(enet_socket_set_option(host->socket, ENET_SOCKOPT_RCVBUF, 4 * 1024 * 1024) == 0);
        endpoints[host] = this;
        enet_host_set_intercept(host, &intercept);
    }
    ~Endpoint()
    {
        discovery.stop();
        CHECK(!discovery.active());
        endpoints.erase(host);
        enet_host_destroy(host);
    }
    static int ENET_CALLBACK intercept(ENetHost* host, void*)
    {
        auto& endpoint = *endpoints.at(host);
        const int result = endpoint.discovery.intercept(host);
        if (result) ++endpoint.consumed;
        else endpoint.raw.emplace_back(host->receivedData, host->receivedData + host->receivedDataLength);
        return result;
    }
    void start(const std::vector<ENetAddress>& servers, uint32_t now)
    {
        std::string error;
        CHECK(discovery.start(host, now));
        CHECK(discovery.configure(servers, now, error));
        CHECK(error.empty());
    }
    void service(bool echo = false)
    {
        ENetEvent event;
        int result;
        while ((result = enet_host_service(host, &event, 0)) > 0)
        {
            if (event.type == ENET_EVENT_TYPE_CONNECT)
            {
                connected = true;
                peer = event.peer;
            }
            else if (event.type == ENET_EVENT_TYPE_RECEIVE)
            {
                received.emplace_back(event.packet->data, event.packet->data + event.packet->dataLength);
                if (echo)
                {
                    CHECK(enet_peer_send(event.peer, event.channelID,
                        enet_packet_create(event.packet->data, event.packet->dataLength,
                                           event.packet->flags & ENET_PACKET_FLAG_RELIABLE ?
                                           ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT)) == 0);
                }
                enet_packet_destroy(event.packet);
            }
            else CHECK(event.type != ENET_EVENT_TYPE_DISCONNECT);
        }
        CHECK(result >= 0);
    }
};

void groups(const std::vector<Bytes>& packets)
{
    CHECK(!packets.empty());
    CHECK(packets.size() % 3 == 0);
    for (size_t i = 0; i < packets.size(); i += 3)
        CHECK(packets[i] == packets[i + 1] && packets[i] == packets[i + 2]);
}
void advance(uint32_t& now)
{
    now += 10;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

Bytes stunResponse(const Bytes& request, uint16_t mappedPort)
{
    CHECK(request.size() == 20 && request[0] == 0 && request[1] == 1);
    Bytes response = request;
    response[0] = 1; response[3] = 12;
    const uint16_t port = mappedPort ^ 0x2112;
    response.insert(response.end(), {0,0x20,0,8,0,1,uint8_t(port >> 8),uint8_t(port),
                                    uint8_t(203 ^ 0x21),uint8_t(0 ^ 0x12),uint8_t(113 ^ 0xa4),uint8_t(10 ^ 0x42)});
    return response;
}

void stun()
{
    Socket responder, attacker;
    Endpoint endpoint;
    const std::vector<ENetAddress> config{responder.address};
    uint32_t now = 0xfffff000u; // Exercise timestamp wrap in a real exchange.
    endpoint.start(config, now);
    endpoint.discovery.pump(now);
    Bytes request; ENetAddress source{};
    CHECK(responder.receive(request, source));
    CHECK(same(source, endpoint.address)); // STUN uses the actual ENet game socket.
    const Bytes response = stunResponse(request, 45555);
    ENetAddress mapped{};
    attacker.send(source, response);
    endpoint.service();
    CHECK(!endpoint.discovery.discoveredAddress(mapped));
    auto wrong = response; wrong[8] ^= 1;
    responder.send(source, wrong); endpoint.service();
    CHECK(!endpoint.discovery.discoveredAddress(mapped));
    wrong = response; wrong[3] = 16;
    responder.send(source, wrong); endpoint.service();
    CHECK(!endpoint.discovery.discoveredAddress(mapped));
    responder.send(source, response); endpoint.service();
    CHECK(endpoint.discovery.discoveredAddress(mapped));
    auto expected = loopback(45555);
    char publicAddress[] = "203.0.113.10";
    CHECK(enet_address_set_host(&expected, publicAddress) == 0);
    CHECK(same(mapped, expected));
    CHECK(endpoint.consumed == 4 && endpoint.raw.empty());
    endpoint.discovery.pump(now + 59999);
    CHECK(endpoint.discovery.discoveredAddress(mapped));
    endpoint.discovery.pump(now + 60000);
    CHECK(!endpoint.discovery.discoveredAddress(mapped));
    endpoint.discovery.stop(); endpoint.discovery.pump(now + 60000);
    CHECK(!endpoint.discovery.discoveredAddress(mapped));
    CHECK(!endpoint.discovery.active());
    std::puts("PASS: same-socket STUN, wrong source/transaction/malformed rejection, cleanup");
}

void stunFallbacks()
{
    Socket primary, fallback;
    Endpoint endpoint;
    std::vector<ENetAddress> config{primary.address, fallback.address};
    uint32_t now = 0xffffe000u;
    const uint32_t started = now;
    endpoint.start(config, now);
    Bytes firstRequest, secondRequest, packet;
    ENetAddress source{};
    CHECK(primary.receive(firstRequest, source));
    CHECK(same(source, endpoint.address));
    unsigned firstRequests = 1;
    for (unsigned tick = 1; tick <= 100 && secondRequest.empty(); ++tick)
    {
        now = started + tick * 100;
        endpoint.discovery.pump(now);
        while (primary.receive(packet, source))
        {
            ++firstRequests;
            CHECK(packet == firstRequest && same(source, endpoint.address));
        }
        if (fallback.receive(secondRequest, source)) CHECK(same(source, endpoint.address));
    }
    CHECK(now - started == 10000 && firstRequests == 10 && !secondRequest.empty());
    CHECK(!std::equal(firstRequest.begin() + 8, firstRequest.end(), secondRequest.begin() + 8));
    const auto firstResponse = stunResponse(firstRequest, 45555);
    const auto secondResponse = stunResponse(secondRequest, 45556);
    ENetAddress mapped{};
    primary.send(endpoint.address, firstResponse); endpoint.service();
    CHECK(!endpoint.discovery.discoveredAddress(mapped)); // Late primary response.
    primary.send(endpoint.address, secondResponse); endpoint.service();
    CHECK(!endpoint.discovery.discoveredAddress(mapped)); // Correct transaction, wrong source.
    fallback.send(endpoint.address, firstResponse); endpoint.service();
    CHECK(!endpoint.discovery.discoveredAddress(mapped)); // Correct source, old transaction.
    fallback.send(endpoint.address, secondResponse); endpoint.service();
    CHECK(endpoint.discovery.discoveredAddress(mapped) && mapped.port == 45556);
    const uint32_t succeeded = now;
    endpoint.discovery.pump(now = succeeded + 30000);
    Bytes refreshed;
    CHECK(fallback.receive(refreshed, source)); // Refresh the successful endpoint first.
    CHECK(same(source, endpoint.address));
    CHECK(!std::equal(secondRequest.begin() + 8, secondRequest.end(), refreshed.begin() + 8));
    CHECK(!primary.receive(packet, source));
    fallback.send(endpoint.address, secondResponse); endpoint.service();
    CHECK(endpoint.discovery.discoveredAddress(mapped) && mapped.port == 45556);
    fallback.send(endpoint.address, stunResponse(refreshed, 45557)); endpoint.service();
    CHECK(endpoint.discovery.discoveredAddress(mapped) && mapped.port == 45557);
    endpoint.discovery.pump(now + 59999);
    CHECK(endpoint.discovery.discoveredAddress(mapped));
    endpoint.discovery.pump(now + 60000);
    CHECK(!endpoint.discovery.discoveredAddress(mapped));
    endpoint.discovery.stop();

    // A complete discovery cycle is bounded and backs off. It must neither
    // change gameplay connection state nor continue sending every frame.
    Socket deadPrimary, deadFallback;
    config = {deadPrimary.address, deadFallback.address};
    now = 12345;
    endpoint.start(config, now);
    unsigned sends[2]{};
    for (unsigned tick = 0; tick <= 25; ++tick)
    {
        endpoint.discovery.pump(now + tick * 1000);
        while (deadPrimary.receive(packet, source)) { ++sends[0]; CHECK(same(source, endpoint.address)); }
        while (deadFallback.receive(packet, source)) { ++sends[1]; CHECK(same(source, endpoint.address)); }
    }
    CHECK(sends[0] == 10 && sends[1] == 10);
    CHECK(!endpoint.discovery.error().empty());
    CHECK(endpoint.discovery.active() && !endpoint.discovery.discoveredAddress(mapped));
    endpoint.discovery.pump(now + 49999);
    CHECK(!deadPrimary.receive(packet, source) && !deadFallback.receive(packet, source));
    endpoint.discovery.pump(now + 50000);
    CHECK(deadPrimary.receive(packet, source) && same(source, endpoint.address));
    CHECK(!deadFallback.receive(packet, source));
    std::puts("PASS: STUN endpoint fallback, source/transaction isolation, refresh, expiry and bounded retry");
}

void configuration()
{
    netstun::Client detached;
    std::string error;
    CHECK(!detached.start(nullptr, 0));
    CHECK(!detached.configure({}, 0, error) && !error.empty());
    Socket first, second;
    Endpoint endpoint;
    uint32_t now = 100;
    endpoint.start({}, now);
    CHECK(!endpoint.discovery.active());
    CHECK(endpoint.discovery.configure({first.address}, now, error) && error.empty());
    CHECK(endpoint.discovery.active());
    Bytes request, packet; ENetAddress source{}, mapped{};
    CHECK(first.receive(request, source) && same(source, endpoint.address));
    const auto response = stunResponse(request, 45555);
    first.send(source, response); endpoint.service();
    CHECK(endpoint.discovery.discoveredAddress(mapped));
    CHECK(!endpoint.discovery.configure(std::vector<ENetAddress>(9, first.address), now, error));
    CHECK(!error.empty() && endpoint.discovery.active() && endpoint.discovery.discoveredAddress(mapped));
    CHECK(!endpoint.discovery.configure({ENetAddress{}}, now, error));
    auto invalid = loopback(12345);
    char multicast[] = "224.0.0.1";
    CHECK(enet_address_set_host(&invalid, multicast) == 0);
    CHECK(!endpoint.discovery.configure({invalid}, now, error));
    CHECK(endpoint.discovery.discoveredAddress(mapped));

    CHECK(endpoint.discovery.configure({}, now, error) && error.empty());
    CHECK(!endpoint.discovery.active() && !endpoint.discovery.discoveredAddress(mapped));
    const unsigned consumed = endpoint.consumed;
    first.send(endpoint.address, response); endpoint.service();
    CHECK(endpoint.consumed == consumed + 1 && endpoint.raw.empty());
    CHECK(!endpoint.discovery.discoveredAddress(mapped));
    endpoint.discovery.pump(now += 100000);
    CHECK(!first.receive(packet, source));
    CHECK(endpoint.discovery.configure({second.address}, now, error) && error.empty());
    CHECK(second.receive(request, source) && same(source, endpoint.address));
    first.send(endpoint.address, response); endpoint.service();
    CHECK(!endpoint.discovery.discoveredAddress(mapped));
    second.send(endpoint.address, stunResponse(request, 45557)); endpoint.service();
    CHECK(endpoint.discovery.discoveredAddress(mapped) && mapped.port == 45557);
    std::puts("PASS: deferred configuration, invalid settings preserve state, opt-out and stale response isolation");
}

void directConnection(unsigned mode)
{
    Socket responder;
    Endpoint server, client;
    uint32_t now = 1000;
    // Discovery succeeds, times out, or is disabled. All three leave ordinary
    // ENet connections, packet duplication and game payloads unchanged.
    server.start(mode == 2 ? std::vector<ENetAddress>{} : std::vector<ENetAddress>{responder.address}, now);
    client.start({}, now);
    Bytes request; ENetAddress source{};
    if (mode != 2)
    {
        CHECK(responder.receive(request, source) && same(source, server.address));
        if (mode == 0)
        {
            responder.send(source, stunResponse(request, 45555)); server.service();
            ENetAddress observed{};
            CHECK(server.discovery.discoveredAddress(observed));
        }
        else
        {
            server.discovery.pump(now += 10000);
            CHECK(!server.discovery.error().empty());
        }
    }
    client.peer = enet_host_connect(client.host, &server.address, 3, 19);
    CHECK(client.peer);
    std::vector<Bytes> expected{Bytes(32000), Bytes(413), Bytes(8000)};
    for (size_t p = 0; p < expected.size(); ++p)
        for (size_t i = 0; i < expected[p].size(); ++i) expected[p][i] = uint8_t(i * 17 + p * 71);
    bool sent = false;
    uint32_t finishAt = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline)
    {
        server.discovery.pump(now); client.discovery.pump(now);
        server.service(true); client.service();
        if (server.connected && client.connected && !sent)
        {
            // Reconfigure discovery while connected, then deliver its response
            // alongside fragmented game data through the same receive callback.
            std::string error;
            CHECK(server.discovery.configure(mode == 0 ? std::vector<ENetAddress>{responder.address} :
                                             std::vector<ENetAddress>{}, now, error));
            CHECK(server.peer->state == ENET_PEER_STATE_CONNECTED && client.peer->state == ENET_PEER_STATE_CONNECTED);
            if (mode == 0)
            {
                CHECK(responder.receive(request, source) && same(source, server.address));
                responder.send(source, stunResponse(request, 45557));
            }
            for (unsigned channel = 0; channel < expected.size(); ++channel)
            {
                CHECK(enet_peer_send(client.peer, uint8_t(channel),
                    enet_packet_create(expected[channel].data(), expected[channel].size(), channel == 0 ?
                                       ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT)) == 0);
                enet_host_flush(client.host);
            }
            sent = true;
        }
        if (client.received.size() == expected.size() && !finishAt) finishAt = now + 300;
        if (finishAt && int32_t(now - finishAt) >= 0) break;
        advance(now);
    }
    CHECK(server.connected && client.connected && sent);
    CHECK(server.received.size() == expected.size() && client.received.size() == expected.size());
    for (const auto& payload : expected)
    {
        CHECK(std::count(server.received.begin(), server.received.end(), payload) == 1);
        CHECK(std::count(client.received.begin(), client.received.end(), payload) == 1);
    }
    groups(server.raw); groups(client.raw);
    CHECK(server.host->packetCopies == 3 && client.host->packetCopies == 3);
    CHECK(server.host->totalSentPackets == client.raw.size());
    CHECK(client.host->totalSentPackets == server.raw.size());
    if (mode == 0)
    {
        ENetAddress mapped{};
        CHECK(server.discovery.discoveredAddress(mapped) && mapped.port == 45557);
    }
    std::string error;
    CHECK(server.discovery.configure({}, now, error));
    CHECK(server.peer->state == ENET_PEER_STATE_CONNECTED && client.peer->state == ENET_PEER_STATE_CONNECTED);
    std::printf("PASS: direct ENet with STUN %s; byte-identical copies=3 and exact-once payload delivery\n",
                mode == 0 ? "successful" : mode == 1 ? "timed out" : "disabled");
}
} // namespace

int main()
{
    CHECK(enet_initialize() == 0);
    stun(); stunFallbacks(); configuration();
    for (unsigned mode = 0; mode < 3; ++mode) directConnection(mode);
    CHECK(endpoints.empty());
    enet_deinitialize();
    std::printf("PASS: %u STUN client/ENet integration checks\n", checks);
}
