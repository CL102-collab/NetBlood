// Keep IPv6 loopback working in the default, dual-stack transport.
#include "enet.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)

int main()
{
    CHECK(enet_initialize() == 0);
    ENetAddress address = {};
    char loopback[] = "::1";
    CHECK(enet_address_set_host(&address, loopback) == 0);
    ENetSocket server = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
    ENetSocket client = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
    CHECK(server != ENET_SOCKET_NULL && client != ENET_SOCKET_NULL);
    CHECK(enet_socket_bind(server, &address) == 0);
    CHECK(enet_socket_get_address(server, &address) == 0 && address.port);
    sockaddr_storage kernel = {};
    socklen_t length = sizeof(kernel);
    CHECK(getsockname(server, reinterpret_cast<sockaddr*>(&kernel), &length) == 0);
    CHECK(kernel.ss_family == AF_INET6);
    char message[] = "Modern IPv6 transport", received[64] = {};
    ENetBuffer sendBuffer = {}, receiveBuffer = {};
    sendBuffer.data = message;
    sendBuffer.dataLength = sizeof(message);
    receiveBuffer.data = received;
    receiveBuffer.dataLength = sizeof(received);
    CHECK(enet_socket_send(client, &address, &sendBuffer, 1) == sizeof(message));
    enet_uint32 condition = ENET_SOCKET_WAIT_RECEIVE;
    CHECK(enet_socket_wait(server, &condition, 2000) == 0 && (condition & ENET_SOCKET_WAIT_RECEIVE));
    ENetAddress source = {};
    CHECK(enet_socket_receive(server, &source, &receiveBuffer, 1) == sizeof(message));
    CHECK(std::memcmp(message, received, sizeof(message)) == 0);
    CHECK(std::memcmp(&source.host, &address.host, sizeof(address.host)) == 0);
    char formatted[64] = {};
    CHECK(enet_address_get_host_ip(&source, formatted, sizeof(formatted)) == 0);
    CHECK(std::strcmp(formatted, loopback) == 0);
    CHECK(enet_socket_send(server, &source, &sendBuffer, 1) == sizeof(message));
    condition = ENET_SOCKET_WAIT_RECEIVE;
    CHECK(enet_socket_wait(client, &condition, 2000) == 0 && (condition & ENET_SOCKET_WAIT_RECEIVE));
    CHECK(enet_socket_receive(client, &source, &receiveBuffer, 1) == sizeof(message));
    CHECK(source.port == address.port);
    enet_socket_destroy(client);
    enet_socket_destroy(server);
    enet_deinitialize();
    std::puts("PASS: default IPv6 UDP socket and loopback address/data roundtrip");
}
