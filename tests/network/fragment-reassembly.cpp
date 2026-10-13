// Delayed UDP fragments must remain alive until their payload has been copied.
// Include the production receive path to schedule valid fragments deterministically,
// without relying on kernel packet ordering or timers.
#define UNREFERENCED_PARAMETER(value) ((void)(value))
#define ENET_IMPLEMENTATION
#include "enet.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)

namespace {
constexpr unsigned fragmentSize = 1368;
constexpr unsigned fragmentCount = 3;
constexpr unsigned packetSize = fragmentSize * fragmentCount;

unsigned char payload(unsigned sequence, unsigned offset)
{
    return static_cast<unsigned char>(sequence * 17 + offset * 13);
}

struct Receiver
{
    ENetHost* host;
    ENetPeer* peer;

    Receiver()
    {
        host = enet_host_create(nullptr, 1, 1, 0, 0);
        CHECK(host);
        ENetAddress address = {};
        char loopback[] = "127.0.0.1";
        CHECK(enet_address_set_host(&address, loopback) == 0);
        address.port = 9;
        peer = enet_host_connect(host, &address, 1, 0);
        CHECK(peer);
        // Set up the receiving end without sending the queued connect command.
        enet_peer_on_connect(peer);
        peer->state = ENET_PEER_STATE_CONNECTED;
        host->maximumWaitingData = 4 * packetSize;
    }

    ~Receiver() { enet_host_destroy(host); }

    void fragment(unsigned sequence, unsigned number, unsigned reliable = 0)
    {
        alignas(ENetProtocol) unsigned char bytes[sizeof(ENetProtocolSendFragment) + fragmentSize] = {};
        auto* command = reinterpret_cast<ENetProtocol*>(bytes);
        command->header.command = ENET_PROTOCOL_COMMAND_SEND_UNRELIABLE_FRAGMENT;
        command->header.channelID = 0;
        command->header.reliableSequenceNumber = static_cast<enet_uint16>(reliable);
        command->sendFragment.startSequenceNumber = ENET_HOST_TO_NET_16(sequence);
        command->sendFragment.dataLength = ENET_HOST_TO_NET_16(fragmentSize);
        command->sendFragment.fragmentCount = ENET_HOST_TO_NET_32(fragmentCount);
        command->sendFragment.fragmentNumber = ENET_HOST_TO_NET_32(number);
        command->sendFragment.totalLength = ENET_HOST_TO_NET_32(packetSize);
        command->sendFragment.fragmentOffset = ENET_HOST_TO_NET_32(number * fragmentSize);
        for (unsigned i = 0; i < fragmentSize; ++i)
            bytes[sizeof(ENetProtocolSendFragment) + i] = payload(sequence, number * fragmentSize + i);
        host->receivedData = bytes;
        host->receivedDataLength = sizeof(bytes);
        enet_uint8* current = bytes + sizeof(ENetProtocolSendFragment);
        CHECK(enet_protocol_handle_send_unreliable_fragment(host, peer, command, &current) == 0);
        CHECK(current == bytes + sizeof(bytes));
        host->receivedData = nullptr;
        host->receivedDataLength = 0;
    }

    void receive(unsigned sequence)
    {
        enet_uint8 channel = 255;
        ENetPacket* packet = enet_peer_receive(peer, &channel);
        CHECK(packet && channel == 0 && packet->dataLength == packetSize);
        for (unsigned i = 0; i < packetSize; ++i)
            CHECK(packet->data[i] == payload(sequence, i));
        enet_packet_destroy(packet);
    }

    void noPacket() { CHECK(enet_peer_receive(peer, nullptr) == nullptr); }
};

void reorderedFragments()
{
    Receiver receiver;
    // Two newer packets have arrived partly when an older packet's middle
    // fragment arrives late. Queue cleanup must not free that new command
    // before the fragment handler copies its data into the reassembly buffer.
    receiver.fragment(2, 0);
    receiver.fragment(3, 0);
    receiver.fragment(1, 1);
    receiver.noPacket();
    receiver.fragment(1, 1); // Duplicate fragments must not complete a packet.
    receiver.fragment(1, 0);
    receiver.noPacket();
    receiver.fragment(1, 2);
    receiver.receive(1);
    receiver.noPacket();

    receiver.fragment(3, 2);
    receiver.fragment(3, 1);
    receiver.receive(3); // This newer complete packet discards incomplete #2.
    receiver.fragment(2, 1);
    receiver.fragment(2, 2);
    receiver.fragment(3, 1);
    receiver.noPacket();
    CHECK(receiver.peer->totalWaitingData == 0);
}

void repeatedLoss()
{
    Receiver receiver;
    // Discarded incomplete packets must release their receive-buffer budget.
    // A small budget makes a leak visible without a long-running load test.
    for (unsigned sequence = 1; sequence < 65; sequence += 2)
    {
        receiver.fragment(sequence, 0);
        receiver.fragment(sequence + 1, 2);
        receiver.fragment(sequence + 1, 0);
        receiver.fragment(sequence + 1, 1);
        receiver.receive(sequence + 1);
        receiver.noPacket();
        CHECK(receiver.peer->totalWaitingData == 0);
    }
}

void reliableBoundary()
{
    Receiver receiver;
    receiver.fragment(2, 0, 1);
    receiver.fragment(3, 0, 1);
    receiver.fragment(1, 1, 1);
    receiver.fragment(1, 0, 1);
    receiver.fragment(1, 2, 1);
    receiver.noPacket(); // These packets wait for the preceding reliable data.
    ENetProtocol command = {};
    command.header.command = ENET_PROTOCOL_COMMAND_SEND_RELIABLE;
    command.header.reliableSequenceNumber = 1;
    unsigned char marker = 42;
    CHECK(enet_peer_queue_incoming_command(receiver.peer, &command, &marker, 1,
                                           ENET_PACKET_FLAG_RELIABLE, 0));
    ENetPacket* reliable = enet_peer_receive(receiver.peer, nullptr);
    CHECK(reliable && reliable->dataLength == 1 && reliable->data[0] == marker);
    enet_packet_destroy(reliable);
    receiver.receive(1);
    receiver.fragment(3, 1, 1);
    receiver.fragment(3, 2, 1);
    receiver.receive(3);
    receiver.noPacket();
    CHECK(receiver.peer->totalWaitingData == 0);
}
}

int main()
{
    CHECK(enet_initialize() == 0);
    reorderedFragments();
    repeatedLoss();
    reliableBoundary();
    enet_deinitialize();
    std::puts("ENet fragment reassembly: delayed delivery, duplicates, loss and reliable ordering passed");
}
