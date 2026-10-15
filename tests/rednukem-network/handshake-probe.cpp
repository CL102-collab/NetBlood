// SPDX-License-Identifier: GPL-2.0-only
// Real ENet peer: malformed/unauthorized requests must fail before joining.
#define UNREFERENCED_PARAMETER(value) ((void)(value))
#define ENET_IMPLEMENTATION
#include "enet.h"
#include "netprotocol.h"
#include <cstdio>
#include <cstdlib>
#include <string>

int main(int argc, char** argv)
{
    if (argc != 3 || enet_initialize()) return 2;
    std::string const mode = argv[2];
    ENetHost* host = enet_host_create(nullptr, 1, 3, 0, 0);
    if (!host) return 2;
    ENetAddress address{};
    address.port = uint16_t(std::atoi(argv[1]));
    char loopback[] = "127.0.0.1";
    enet_address_set_host(&address, loopback);
    ENetPeer* peer = enet_host_connect(host, &address, 3, 0);
    uint32_t const deadline = enet_time_get() + 10000;
    std::string rejection;
    bool unexpectedWelcome = false;
    while (peer && int32_t(deadline - enet_time_get()) > 0 && rejection.empty() && !unexpectedWelcome)
    {
        ENetEvent event{};
        if (enet_host_service(host, &event, 100) <= 0) continue;
        if (event.type != ENET_EVENT_TYPE_RECEIVE) continue;
        netwire::Reader message(event.packet->data, event.packet->dataLength);
        uint32_t const magic = message.u32(), protocol = message.u32(), map = message.u32(), tick = message.u32();
        uint8_t const type = message.u8();
        if (magic == 0x314d4e52 && protocol == 1 && type == 1)
        {
            netwire::Writer reply;
            reply.u32(magic); reply.u32(protocol); reply.u32(map); reply.u32(tick); reply.u8(2);
            if (mode != "malformed")
            {
                reply.u32(mode == "version" ? 999 : 1);
                reply.text("Untrusted", 31);
                reply.text(mode == "missing-password" ? "" : "incorrect", 128);
            }
            ENetPacket* packet = enet_packet_create(reply.data.data(), reply.data.size(), ENET_PACKET_FLAG_RELIABLE);
            enet_peer_send(peer, 0, packet);
            enet_host_flush(host);
        }
        else if (magic == 0x314d4e52 && type == 3)
            rejection = message.text(1024);
        else if (magic == 0x314d4e52 && type == 4)
            unexpectedWelcome = true;
        enet_packet_destroy(event.packet);
    }
    enet_peer_disconnect_now(peer, 0);
    enet_host_destroy(host);
    enet_deinitialize();
    std::string const expected = mode == "malformed" ? "Invalid authentication" : mode == "version" ? "protocol mismatch"
        : mode == "missing-password" ? "password required" : "Incorrect server password";
    if (unexpectedWelcome || rejection.find(expected) == std::string::npos)
    {
        std::fprintf(stderr, "FAIL mode=%s welcome=%d rejection=%s\n", mode.c_str(), unexpectedWelcome, rejection.c_str());
        return 1;
    }
    std::printf("PASS mode=%s rejection=%s\n", mode.c_str(), rejection.c_str());
    return 0;
}
