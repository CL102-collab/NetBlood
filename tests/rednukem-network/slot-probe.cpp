// SPDX-License-Identifier: GPL-2.0-only
// Hold authenticated real ENet connections while a graphical client uses slot15.
#define UNREFERENCED_PARAMETER(value) ((void)(value))
#define ENET_IMPLEMENTATION
#include "enet.h"
#include "netprotocol.h"
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <unistd.h>

int main(int argc, char** argv)
{
    if (argc != 3 || enet_initialize()) return 2;
    unsigned count = unsigned(std::atoi(argv[2]));
    if (!count || count > 15) return 2;
    ENetHost* host = enet_host_create(nullptr, count, 3, 0, 0);
    if (!host) return 2;
    ENetAddress address{}; address.port = uint16_t(std::atoi(argv[1]));
    char loopback[] = "127.0.0.1"; enet_address_set_host(&address, loopback);
    for (unsigned i = 0; i < count; ++i) if (!enet_host_connect(host, &address, 3, 0)) return 2;
    uint32_t const deadline = enet_time_get() + 90000;
    unsigned welcomeMask = 0, welcomed = 0;
    fcntl(STDIN_FILENO, F_SETFL, fcntl(STDIN_FILENO, F_GETFL) | O_NONBLOCK);
    setvbuf(stdout, nullptr, _IONBF, 0);
    bool stop = false;
    while (!stop && int32_t(deadline - enet_time_get()) > 0)
    {
        char control; if (read(STDIN_FILENO, &control, 1) > 0) break;
        ENetEvent event{};
        if (enet_host_service(host, &event, 50) <= 0) continue;
        if (event.type != ENET_EVENT_TYPE_RECEIVE) continue;
        netwire::Reader in(event.packet->data, event.packet->dataLength);
        uint32_t magic = in.u32(), protocol = in.u32(), map = in.u32(), tick = in.u32(); uint8_t type = in.u8();
        if (magic == 0x314d4e52 && protocol == 1 && type == 1)
        {
            netwire::Writer out; out.u32(magic); out.u32(protocol); out.u32(map); out.u32(tick); out.u8(2);
            out.u32(1); out.text("Holder" + std::to_string(event.peer->incomingPeerID), 31); out.text("fixture-password", 128);
            ENetPacket* packet = enet_packet_create(out.data.data(), out.data.size(), ENET_PACKET_FLAG_RELIABLE);
            enet_peer_send(event.peer, 0, packet);
        }
        else if (magic == 0x314d4e52 && type == 4)
        {
            unsigned slot = in.u8();
            if (slot < 16 && !(welcomeMask & (1u << slot)))
            {
                welcomeMask |= 1u << slot; ++welcomed;
                if (welcomed == count) std::printf("READY holders=%u mask=%04x\n", welcomed, welcomeMask);
            }
        }
        else if (type == 3) { std::fprintf(stderr, "REJECT %s\n", in.text(1024).c_str()); stop = true; }
        enet_packet_destroy(event.packet);
    }
    for (unsigned i = 0; i < count; ++i) enet_peer_disconnect_now(&host->peers[i], 0);
    enet_host_destroy(host); enet_deinitialize();
    return welcomed == count ? 0 : 1;
}
