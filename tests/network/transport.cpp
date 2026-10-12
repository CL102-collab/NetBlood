// Real loopback UDP transport: verify on-wire copies and receiver deduplication.
#include "enet.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)
namespace {
using Bytes = std::vector<enet_uint8>;
char loopback[] = "127.0.0.1";
struct Relay {
    ENetSocket front = ENET_SOCKET_NULL, back = ENET_SOCKET_NULL;
    ENetAddress address = {}, server = {}, client = {};
    std::vector<Bytes> up, down;
    bool clientKnown = false, dropTwo = false;
    size_t dropped = 0;
    explicit Relay(ENetAddress destination, bool loss):server(destination),dropTwo(loss)
    {
        front = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
        back = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
        CHECK(front != ENET_SOCKET_NULL && back != ENET_SOCKET_NULL);
        CHECK(enet_socket_bind(front, nullptr) == 0 && enet_socket_bind(back, nullptr) == 0);
        CHECK(enet_socket_set_option(front, ENET_SOCKOPT_NONBLOCK, 1) == 0);
        CHECK(enet_socket_set_option(back, ENET_SOCKOPT_NONBLOCK, 1) == 0);
        CHECK(enet_socket_set_option(front, ENET_SOCKOPT_RCVBUF, 4*1024*1024) == 0);
        CHECK(enet_socket_set_option(back, ENET_SOCKOPT_RCVBUF, 4*1024*1024) == 0);
        CHECK(enet_socket_get_address(front, &address) == 0);
        CHECK(enet_address_set_host(&address, loopback) == 0);
    }
    ~Relay() { enet_socket_destroy(front); enet_socket_destroy(back); }
    void pumpDirection(ENetSocket input, ENetSocket output, bool upstream)
    {
        for (unsigned i = 0; i < 4096; ++i) {
            enet_uint8 bytes[65536]; ENetBuffer buffer; buffer.data = bytes; buffer.dataLength = sizeof(bytes);
            ENetAddress source = {};
            const int count = enet_socket_receive(input, &source, &buffer, 1);
            CHECK(count >= 0); if (!count) break;
            if (upstream) { client = source; clientKnown = true; }
            else CHECK(clientKnown);
            auto& capture = upstream ? up : down;
            capture.emplace_back(bytes, bytes+count);
            // Keep exactly one of three physical copies, including handshakes,
            // acknowledgements, reliable packets and fragmented snapshots.
            if (dropTwo && (capture.size()-1)%3 < 2) { ++dropped; continue; }
            buffer.dataLength = size_t(count);
            CHECK(enet_socket_send(output, upstream ? &server : &client, &buffer, 1) == count);
        }
    }
    void pump() { pumpDirection(front, back, true); pumpDirection(back, front, false); }
};
void groups(const std::vector<Bytes>& packets, unsigned copies)
{
    CHECK(!packets.empty() && packets.size()%copies == 0);
    for (size_t i = 0; i < packets.size(); i += copies)
        for (unsigned copy = 1; copy < copies; ++copy) CHECK(packets[i] == packets[i+copy]);
}
void session(unsigned configuredCopies, bool loss)
{
    const unsigned copies = std::max(1u, std::min(8u, configuredCopies));
    ENetAddress address = {}; CHECK(enet_address_set_host(&address, loopback) == 0);
    ENetHost* server = enet_host_create(&address, 2, 3, 0, 0);
    ENetHost* client = enet_host_create(nullptr, 1, 3, 0, 0);
    CHECK(server && client && server->packetCopies == 1 && client->packetCopies == 1);
    server->checksum = client->checksum = enet_crc32;
    CHECK(enet_socket_set_option(server->socket, ENET_SOCKOPT_RCVBUF, 4*1024*1024) == 0);
    CHECK(enet_socket_set_option(client->socket, ENET_SOCKOPT_RCVBUF, 4*1024*1024) == 0);
    server->packetCopies = configuredCopies; client->packetCopies = configuredCopies;
    Relay relay(server->address, loss);
    ENetPeer* peer = enet_host_connect(client, &relay.address, 3, 19);
    CHECK(peer);
    bool connected = false, accepted = false;
    ENetPeer* remote = nullptr;
    std::vector<Bytes> payloads;
    payloads.emplace_back(copies > 3 ? 4096 : 64000);
    payloads.emplace_back(413); payloads.emplace_back(copies > 3 ? 2048 : 32000);
    for (size_t packet = 0; packet != payloads.size(); ++packet)
        for (size_t i = 0; i != payloads[packet].size(); ++i) payloads[packet][i] = uint8_t(i*17 + packet*73);
    unsigned received[3] = {}, echoed[3] = {};
    bool sent = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    auto completeAt = deadline;
    while (std::chrono::steady_clock::now() < deadline) {
        relay.pump();
        for (unsigned endpoint = 0; endpoint < 2; ++endpoint) {
            ENetHost* host = endpoint ? client : server;
            ENetEvent event;
            int status;
            while ((status = enet_host_service(host, &event, 0)) > 0) {
                if (event.type == ENET_EVENT_TYPE_CONNECT) {
                    if (endpoint) connected = true;
                    else { accepted = true; remote = event.peer; CHECK(event.data == 19); }
                } else if (event.type == ENET_EVENT_TYPE_RECEIVE) {
                    CHECK(event.channelID < payloads.size());
                    auto& expected = payloads[event.channelID];
                    CHECK(event.packet->dataLength == expected.size());
                    CHECK(std::memcmp(event.packet->data, expected.data(), expected.size()) == 0);
                    if (endpoint) CHECK(++echoed[event.channelID] == 1);
                    else {
                        CHECK(++received[event.channelID] == 1);
                        const unsigned flags = event.channelID == 0 ? ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT;
                        CHECK(enet_peer_send(remote, event.channelID,
                            enet_packet_create(expected.data(), expected.size(), flags)) == 0);
                        enet_host_flush(server);
                    }
                    enet_packet_destroy(event.packet);
                } else CHECK(event.type != ENET_EVENT_TYPE_DISCONNECT);
            }
            CHECK(status >= 0);
        }
        if (connected && accepted && !sent) {
            for (unsigned channel = 0; channel < payloads.size(); ++channel) {
                const unsigned flags = channel == 0 ? ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT;
                CHECK(enet_peer_send(peer, uint8_t(channel),
                    enet_packet_create(payloads[channel].data(), payloads[channel].size(), flags)) == 0);
                enet_host_flush(client);
            }
            sent = true;
        }
        if (echoed[0] && echoed[1] && echoed[2] && completeAt == deadline)
            completeAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
        if (std::chrono::steady_clock::now() >= completeAt) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    relay.pump();
    CHECK(connected && accepted && sent);
    for (unsigned i = 0; i < 3; ++i) {
        if (received[i] != 1 || echoed[i] != 1)
            std::fprintf(stderr, "copies=%u channel=%u received=%u echoed=%u relay=%zu/%zu\n", copies, i, received[i], echoed[i], relay.up.size(), relay.down.size());
        CHECK(received[i] == 1 && echoed[i] == 1);
    }
    groups(relay.up, copies); groups(relay.down, copies);
    CHECK(client->totalSentPackets == relay.up.size());
    CHECK(server->totalSentPackets == relay.down.size());
    if (loss) CHECK(relay.dropped > 100);
    std::printf("PASS: actual UDP copies=%u (configured %u), identical bytes, reliable/unreliable/fragment dedup; datagrams=%zu/%zu dropped=%zu\n",
                copies, configuredCopies, relay.up.size(), relay.down.size(), relay.dropped);
    enet_host_destroy(client); enet_host_destroy(server);
}

size_t ENET_CALLBACK checkCompressionBounds(void* context, const ENetBuffer* buffers, size_t count,
                                           size_t inLimit, enet_uint8*, size_t outLimit)
{
    size_t bytes = 0;
    for (size_t i = 0; i < count; ++i) bytes += buffers[i].dataLength;
    CHECK(bytes == inLimit && bytes == outLimit);
    ++*static_cast<unsigned*>(context);
    return 0; // Exercise compressor bounds while retaining the original bytes.
}

void checksumMtuBoundary(bool compressionProbe)
{
    ENetAddress address = {}; CHECK(enet_address_set_host(&address, loopback) == 0);
    ENetHost* server = enet_host_create(&address, 8, 3, 0, 0);
    CHECK(server);
    server->checksum = enet_crc32;
    server->packetCopies = 3;
    unsigned compressionCalls = 0;
    ENetCompressor compressor = {};
    compressor.context = &compressionCalls;
    compressor.compress = checkCompressionBounds;
    if (compressionProbe) enet_host_compress(server, &compressor);
    std::array<ENetHost*, 3> clients = {};
    std::array<ENetPeer*, 3> remote = {};
    std::array<std::unique_ptr<Relay>, 3> relay;
    std::array<unsigned, 3> received = {};
    unsigned accepted = 0, connected = 0;
    bool sent = false;
    // Before checksum reservation these two reliable commands coalesced into
    // 1404 bytes for an MTU of 1400. Every retry repeated the truncation: larger
    // multi-player Welcome packets and their accompanying status could hang.
    const size_t firstLength = server->mtu - sizeof(ENetProtocolHeader) - sizeof(ENetProtocolSendFragment) - sizeof(enet_uint32);
    const size_t secondLength = sizeof(ENetProtocolSendFragment) + sizeof(enet_uint32) - 2 * sizeof(ENetProtocolSendReliable);
    std::array<Bytes, 2> payloads = {{Bytes(firstLength), Bytes(secondLength)}};
    for (unsigned i = 0; i < payloads.size(); ++i)
        for (size_t j = 0; j < payloads[i].size(); ++j) payloads[i][j] = uint8_t(i * 73 + j * 17);
    for (unsigned i = 0; i < clients.size(); ++i)
    {
        clients[i] = enet_host_create(nullptr, 1, 3, 0, 0);
        CHECK(clients[i]);
        clients[i]->checksum = enet_crc32;
        clients[i]->packetCopies = 3;
        if (compressionProbe) enet_host_compress(clients[i], &compressor);
        relay[i].reset(new Relay(server->address, false));
        CHECK(enet_host_connect(clients[i], &relay[i]->address, 3, i));
    }
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline)
    {
        for (auto& route : relay) route->pump();
        ENetEvent event;
        int status;
        while ((status = enet_host_service(server, &event, 0)) > 0)
        {
            CHECK(event.type == ENET_EVENT_TYPE_CONNECT);
            CHECK(event.data < remote.size() && !remote[event.data]);
            remote[event.data] = event.peer;
            ++accepted;
        }
        CHECK(status >= 0);
        for (unsigned i = 0; i < clients.size(); ++i)
        {
            while ((status = enet_host_service(clients[i], &event, 0)) > 0)
            {
                if (event.type == ENET_EVENT_TYPE_CONNECT) { ++connected; continue; }
                CHECK(event.type == ENET_EVENT_TYPE_RECEIVE && event.channelID == 0);
                CHECK(received[i] < payloads.size());
                const auto& expected = payloads[received[i]++];
                CHECK(event.packet->dataLength == expected.size());
                CHECK(std::memcmp(event.packet->data, expected.data(), expected.size()) == 0);
                enet_packet_destroy(event.packet);
            }
            CHECK(status >= 0);
        }
        if (accepted == clients.size() && connected == clients.size() && !sent)
        {
            for (auto* peer : remote)
                for (auto& payload : payloads)
                    CHECK(enet_peer_send(peer, 0, enet_packet_create(payload.data(), payload.size(), ENET_PACKET_FLAG_RELIABLE)) == 0);
            // Flush only after every command is queued, exercising coalescing
            // and all three peers in the same send_outgoing_commands pass.
            enet_host_flush(server);
            sent = true;
        }
        if (received[0] == 2 && received[1] == 2 && received[2] == 2) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(sent && accepted == 3 && connected == 3);
    for (unsigned i = 0; i < clients.size(); ++i)
    {
        CHECK(received[i] == 2);
        relay[i]->pump();
        for (const auto& packet : relay[i]->up) CHECK(packet.size() <= clients[i]->mtu);
        for (const auto& packet : relay[i]->down) CHECK(packet.size() <= server->mtu);
        groups(relay[i]->up, 3);
        groups(relay[i]->down, 3);
        enet_host_destroy(clients[i]);
    }
    enet_host_destroy(server);
    CHECK(!compressionProbe || compressionCalls > 0);
    std::printf("PASS: CRC MTU boundary, three simultaneous peers, coalesced reliable packets, triplicate copies, compressor_bounds=%d\n", int(compressionProbe));
}
}
int main()
{
    CHECK(enet_initialize() == 0);
    session(1, false); session(3, false); session(3, true);
    session(0, false); session(9, false);
    checksumMtuBoundary(false); checksumMtuBoundary(true);
    enet_deinitialize();
}
