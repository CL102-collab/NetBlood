// Exercise ENet's production protocol over a transport with no native socket.
#include "enet.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <thread>
#include <vector>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)

namespace {
using Bytes = std::vector<enet_uint8>;
struct Datagram { ENetAddress source; Bytes bytes; };
struct Endpoint {
    ENetAddress address = {};
    Endpoint* destination = nullptr;
    std::deque<Datagram> incoming;
    std::vector<Bytes> wire;
    bool loss = false, failSend = false, failReceive = false, failWait = false;
    unsigned closed = 0, waited = 0, reordered = 0, truncated = 0, dropped = 0;
};

int ENET_CALLBACK send(void* context, ENetAddress* address, ENetBuffer* buffers, size_t count)
{
    auto& endpoint = *static_cast<Endpoint*>(context);
    if (endpoint.failSend) return -1;
    CHECK(!endpoint.closed && endpoint.destination);
    CHECK(address->port == endpoint.destination->address.port);
    CHECK(std::memcmp(&address->host, &endpoint.destination->address.host, sizeof(address->host)) == 0);
    Bytes bytes;
    for (size_t i = 0; i < count; ++i)
    {
        auto* begin = static_cast<enet_uint8*>(buffers[i].data);
        bytes.insert(bytes.end(), begin, begin + buffers[i].dataLength);
    }
    const int length = static_cast<int>(bytes.size());
    endpoint.wire.push_back(bytes);
    // Drop the first two physical copies. Reliable handshakes and fragmented
    // application messages must still arrive once through the third copy.
    if (endpoint.loss && endpoint.wire.size() % 3)
    {
        ++endpoint.dropped;
        return length;
    }
    Datagram packet = { endpoint.address, bytes };
    auto& queue = endpoint.destination->incoming;
    if (endpoint.loss && !queue.empty() && endpoint.wire.size() % 2)
    {
        queue.push_front(packet);
        ++endpoint.reordered;
    }
    else queue.push_back(packet);
    return length;
}

int ENET_CALLBACK receive(void* context, ENetAddress* address, ENetBuffer* buffers, size_t count)
{
    auto& endpoint = *static_cast<Endpoint*>(context);
    if (endpoint.failReceive) return -1;
    CHECK(!endpoint.closed);
    if (endpoint.incoming.empty()) return 0;
    Datagram packet = endpoint.incoming.front();
    endpoint.incoming.pop_front();
    size_t capacity = 0;
    for (size_t i = 0; i < count; ++i) capacity += buffers[i].dataLength;
    if (packet.bytes.size() > capacity)
    {
        ++endpoint.truncated;
        return -2;
    }
    *address = packet.source;
    size_t offset = 0;
    for (size_t i = 0; i < count && offset < packet.bytes.size(); ++i)
    {
        size_t length = std::min(buffers[i].dataLength, packet.bytes.size() - offset);
        std::memcpy(buffers[i].data, packet.bytes.data() + offset, length);
        offset += length;
    }
    return static_cast<int>(offset);
}

int ENET_CALLBACK wait(void* context, enet_uint32* condition, enet_uint64 timeout)
{
    auto& endpoint = *static_cast<Endpoint*>(context);
    ++endpoint.waited;
    if (endpoint.failWait) return -1;
    if (endpoint.incoming.empty() && timeout)
        std::this_thread::sleep_for(std::chrono::milliseconds(std::min<enet_uint64>(timeout, 2)));
    *condition = endpoint.incoming.empty() ? ENET_SOCKET_WAIT_NONE : ENET_SOCKET_WAIT_RECEIVE;
    return 0;
}

void ENET_CALLBACK destroy(void* context)
{
    auto& endpoint = *static_cast<Endpoint*>(context);
    CHECK(++endpoint.closed == 1);
    endpoint.incoming.clear();
}

ENetTransport transport(Endpoint& endpoint)
{
    ENetTransport callbacks = { &endpoint, send, receive, wait, destroy };
    return callbacks;
}

ENetAddress address(unsigned index)
{
    ENetAddress result = {};
#ifdef ENET_IPV4_ONLY
    char host[] = "127.0.0.1";
#else
    char host[] = "fd00::1";
#endif
    CHECK(enet_address_set_host_ip(&result, host) == 0);
    result.port = static_cast<enet_uint16>(23513 + index);
    return result;
}

void sameCopies(const std::vector<Bytes>& wire, unsigned copies)
{
    CHECK(!wire.empty() && wire.size() % copies == 0);
    for (size_t i = 0; i < wire.size(); i += copies)
        for (unsigned copy = 1; copy < copies; ++copy) CHECK(wire[i] == wire[i + copy]);
}

void customSession(bool loss)
{
    Endpoint endpoints[2];
    ENetHost* hosts[2];
    for (unsigned i = 0; i < 2; ++i)
    {
        endpoints[i].address = address(i);
        endpoints[i].destination = &endpoints[1 - i];
        endpoints[i].loss = loss;
        ENetTransport callbacks = transport(endpoints[i]);
        hosts[i] = enet_host_create_with_transport(&endpoints[i].address, 2, 3, 0, 0, &callbacks);
        CHECK(hosts[i] && hosts[i]->socket == ENET_SOCKET_NULL);
        CHECK(hosts[i]->address.port == endpoints[i].address.port);
        hosts[i]->checksum = enet_crc32;
        hosts[i]->packetCopies = loss ? 3 : 1;
        // The host must copy the callback table, rather than retaining this stack object.
        std::memset(&callbacks, 0, sizeof(callbacks));
    }

    // A truncated virtual datagram is discarded without ending the service pass.
    endpoints[0].incoming.push_back({ endpoints[1].address, Bytes(hosts[0]->mtu + 1) });
    ENetEvent event;
    CHECK(enet_host_service(hosts[0], &event, 0) == 0);
    CHECK(endpoints[0].truncated == 1);

    // Raw datagrams must use the custom backend, including the sliced variant.
    enet_uint8 raw[] = { 11, 22, 33, 44 };
    CHECK(enet_host_send_raw(hosts[0], &endpoints[1].address, raw, sizeof(raw)) == 4);
    CHECK(enet_host_send_raw_ex(hosts[0], &endpoints[1].address, raw, 1, 2) == 2);
    CHECK(endpoints[0].wire[0] == Bytes(raw, raw + 4));
    CHECK(endpoints[0].wire[1] == Bytes(raw + 1, raw + 3));
    endpoints[0].wire.clear(); endpoints[1].incoming.clear();

    ENetPeer* client = enet_host_connect(hosts[1], &endpoints[0].address, 3, 77);
    CHECK(client);
    bool connected[2] = {}, sent = false;
    unsigned received[2][3] = {};
    std::vector<Bytes> payloads;
    payloads.emplace_back(90000); payloads.emplace_back(64000); payloads.emplace_back(413);
    for (size_t packet = 0; packet < payloads.size(); ++packet)
        for (size_t byte = 0; byte < payloads[packet].size(); ++byte)
            payloads[packet][byte] = static_cast<enet_uint8>(packet * 73 + byte * 17);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    auto completeAt = deadline;
    while (std::chrono::steady_clock::now() < deadline)
    {
        for (unsigned endpoint = 0; endpoint < 2; ++endpoint)
        {
            int status;
            while ((status = enet_host_service(hosts[endpoint], &event, 0)) > 0)
            {
                if (event.type == ENET_EVENT_TYPE_CONNECT)
                {
                    connected[endpoint] = true;
                    if (!endpoint) CHECK(event.data == 77);
                }
                else if (event.type == ENET_EVENT_TYPE_RECEIVE)
                {
                    CHECK(event.channelID < payloads.size());
                    auto& bytes = payloads[event.channelID];
                    CHECK(++received[endpoint][event.channelID] == 1);
                    CHECK(event.packet->dataLength == bytes.size());
                    CHECK(std::memcmp(event.packet->data, bytes.data(), bytes.size()) == 0);
                    if (!endpoint)
                        CHECK(enet_peer_send(event.peer, event.channelID,
                            enet_packet_create(bytes.data(), bytes.size(), event.channelID == 2 ? 0 : ENET_PACKET_FLAG_RELIABLE)) == 0);
                    enet_packet_destroy(event.packet);
                }
                else CHECK(event.type != ENET_EVENT_TYPE_DISCONNECT);
            }
            CHECK(status >= 0);
        }
        if (connected[0] && connected[1] && !sent)
        {
            for (unsigned i = 0; i < payloads.size(); ++i)
                CHECK(enet_peer_send(client, static_cast<enet_uint8>(i),
                    enet_packet_create(payloads[i].data(), payloads[i].size(), i == 2 ? 0 : ENET_PACKET_FLAG_RELIABLE)) == 0);
            enet_host_flush(hosts[1]);
            sent = true;
        }
        if (received[1][0] && received[1][1] && received[1][2] && completeAt == deadline)
            completeAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
        if (std::chrono::steady_clock::now() >= completeAt) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(sent && connected[0] && connected[1]);
    for (unsigned i = 0; i < 2; ++i)
    {
        for (unsigned packet = 0; packet < 3; ++packet) CHECK(received[i][packet] == 1);
        sameCopies(endpoints[i].wire, loss ? 3 : 1);
        CHECK(hosts[i]->totalSentPackets == endpoints[i].wire.size());
        if (loss) CHECK(endpoints[i].dropped > 100 && endpoints[i].reordered > 0);
        // Exercise the wait callback when no packets remain.
        endpoints[i].incoming.clear();
        CHECK(enet_host_service(hosts[i], &event, 3) == 0);
        CHECK(endpoints[i].waited > 0);
        enet_host_destroy(hosts[i]);
        CHECK(endpoints[i].closed == 1);
    }
    std::printf("PASS: custom transport connect, raw send, scatter/gather, fragmented reliable/unreliable delivery, copies=%u loss/reorder=%d\n",
        loss ? 3 : 1, int(loss));
}

unsigned allocationCall = 0, failAllocation = 0, allocations = 0, memoryWarnings = 0;
void* ENET_CALLBACK allocate(size_t bytes)
{
    if (++allocationCall == failAllocation) return nullptr;
    void* result = std::malloc(bytes);
    if (result) ++allocations;
    return result;
}
void ENET_CALLBACK release(void* memory)
{
    if (memory) { CHECK(allocations > 0); --allocations; }
    std::free(memory);
}
void ENET_CALLBACK noMemory() { ++memoryWarnings; }

void failures()
{
    Endpoint endpoint;
    endpoint.address = address(0);
    ENetTransport callbacks = transport(endpoint);
    CHECK(!enet_host_create_with_transport(&endpoint.address, ENET_PROTOCOL_MAXIMUM_PEER_ID + 1, 3, 0, 0, &callbacks));
    ENetTransport incomplete = callbacks;
    incomplete.receive = nullptr;
    CHECK(!enet_host_create_with_transport(&endpoint.address, 1, 3, 0, 0, &incomplete));
    CHECK(endpoint.closed == 0);

    ENetCallbacks memory = { allocate, release, noMemory };
    CHECK(enet_initialize_with_callbacks(ENET_VERSION, &memory) == 0);
    for (unsigned failure = 1; failure <= 2; ++failure)
    {
        allocationCall = 0; failAllocation = failure;
        CHECK(!enet_host_create_with_transport(&endpoint.address, 1, 3, 0, 0, &callbacks));
        CHECK(endpoint.closed == 0 && allocations == 0);
    }
    CHECK(memoryWarnings == 2);
    ENetCallbacks defaults = { std::malloc, std::free, std::abort };
    CHECK(enet_initialize_with_callbacks(ENET_VERSION, &defaults) == 0);

    ENetHost* host = enet_host_create_with_transport(&endpoint.address, 1, 3, 0, 0, &callbacks);
    CHECK(host);
    endpoint.failReceive = true;
    ENetEvent event;
    CHECK(enet_host_service(host, &event, 0) == -1);
    endpoint.failReceive = false;
    endpoint.failWait = true;
    CHECK(enet_host_service(host, &event, 100) == -1);
    endpoint.failWait = false;
    endpoint.failSend = true;
    enet_uint8 byte = 1;
    CHECK(enet_host_send_raw(host, &endpoint.address, &byte, 1) == -1);
    CHECK(enet_host_connect(host, &endpoint.address, 3, 0));
    CHECK(enet_host_service(host, &event, 0) == -1);
    enet_host_destroy(host);
    CHECK(endpoint.closed == 1);

    // The close callback is optional for transports that do not own resources.
    callbacks.destroy = nullptr;
    host = enet_host_create_with_transport(nullptr, 1, 3, 0, 0, &callbacks);
    CHECK(host);
    enet_host_destroy(host);
    CHECK(endpoint.closed == 1);
    std::puts("PASS: callback validation, constructor allocation failures retain ownership, I/O error propagation, optional destroy");
}

void nativeSession()
{
    ENetAddress address = {};
    char loopback[] = "127.0.0.1";
    CHECK(enet_address_set_host(&address, loopback) == 0);
    ENetHost* server = enet_host_create(&address, 1, 1, 0, 0);
    ENetHost* client = enet_host_create_with_transport(nullptr, 1, 1, 0, 0, nullptr);
    CHECK(server && client && server->socket != ENET_SOCKET_NULL && client->socket != ENET_SOCKET_NULL);
    CHECK(!server->transport.send && !client->transport.send);
    CHECK(enet_host_connect(client, &server->address, 1, 0));
    bool connected[2] = {};
    ENetHost* hosts[] = { server, client };
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while ((!connected[0] || !connected[1]) && std::chrono::steady_clock::now() < deadline)
    {
        for (unsigned i = 0; i < 2; ++i)
        {
            ENetEvent event;
            int status = enet_host_service(hosts[i], &event, 1);
            CHECK(status >= 0);
            if (status > 0) { CHECK(event.type == ENET_EVENT_TYPE_CONNECT); connected[i] = true; }
        }
    }
    CHECK(connected[0] && connected[1]);
    enet_host_destroy(client); enet_host_destroy(server);
    std::puts("PASS: native socket constructor and NULL transport retain UDP behavior");
}
}

int main()
{
    CHECK(enet_initialize() == 0);
    customSession(false); customSession(true);
    failures(); nativeSession();
    enet_deinitialize();
}
