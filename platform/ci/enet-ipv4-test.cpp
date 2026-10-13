// The XP package uses the same IPv4 socket translation on Windows and Unix.
// Test the actual kernel socket family, address round-trips and unsupported
// IPv6 rejection, without needing an obsolete Windows image in CI.
#include "enet.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)

static void mapped(const ENetAddress& address)
{
    const unsigned char prefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 255, 255};
    CHECK(std::memcmp(&address.host, prefix, sizeof(prefix)) == 0);
    CHECK(address.sin6_scope_id == 0);
}

static void transfer(ENetSocketType type)
{
    ENetSocket server = enet_socket_create(type), client = enet_socket_create(type);
    CHECK(server != ENET_SOCKET_NULL && client != ENET_SOCKET_NULL);
    CHECK(enet_socket_bind(server, nullptr) == 0);
    ENetAddress destination = {};
    CHECK(enet_socket_get_address(server, &destination) == 0 && destination.port);
    mapped(destination);
    sockaddr_storage kernel = {};
    socklen_t length = sizeof(kernel);
    CHECK(getsockname(server, reinterpret_cast<sockaddr*>(&kernel), &length) == 0);
    CHECK(kernel.ss_family == AF_INET);
    char loopback[] = "127.0.0.1";
    CHECK(enet_address_set_host_ip(&destination, loopback) == 0);
    CHECK(destination.port != 0);
    ENetSocket receiver = server;
    if (type == ENET_SOCKET_TYPE_STREAM)
    {
        CHECK(enet_socket_listen(server, 1) == 0);
        CHECK(enet_socket_connect(client, &destination) == 0);
        ENetAddress remote = {};
        receiver = enet_socket_accept(server, &remote);
        CHECK(receiver != ENET_SOCKET_NULL);
        mapped(remote);
    }
    char message[] = "XP IPv4 transport";
    ENetBuffer outgoing = {};
    outgoing.data = message;
    outgoing.dataLength = sizeof(message);
    CHECK(enet_socket_send(client, type == ENET_SOCKET_TYPE_DATAGRAM ? &destination : nullptr, &outgoing, 1) == sizeof(message));
    enet_uint32 condition = ENET_SOCKET_WAIT_RECEIVE;
    CHECK(enet_socket_wait(receiver, &condition, 2000) == 0 && (condition & ENET_SOCKET_WAIT_RECEIVE));
    char bytes[64] = {};
    ENetBuffer incoming = {};
    incoming.data = bytes;
    incoming.dataLength = sizeof(bytes);
    ENetAddress source = {};
    CHECK(enet_socket_receive(receiver, type == ENET_SOCKET_TYPE_DATAGRAM ? &source : nullptr, &incoming, 1) == sizeof(message));
    CHECK(std::memcmp(bytes, message, sizeof(message)) == 0);
    if (type == ENET_SOCKET_TYPE_DATAGRAM)
    {
        mapped(source);
        char address[64] = {};
        CHECK(enet_address_get_host_ip(&source, address, sizeof(address)) == 0);
        CHECK(std::strcmp(address, loopback) == 0);
        CHECK(enet_socket_send(server, &source, &outgoing, 1) == sizeof(message));
        condition = ENET_SOCKET_WAIT_RECEIVE;
        CHECK(enet_socket_wait(client, &condition, 2000) == 0 && (condition & ENET_SOCKET_WAIT_RECEIVE));
        CHECK(enet_socket_receive(client, &source, &incoming, 1) == sizeof(message));
        CHECK(source.port == destination.port);
        mapped(source);
    }
    else enet_socket_destroy(receiver);
    enet_socket_destroy(client);
    enet_socket_destroy(server);
}

int main()
{
    CHECK(enet_initialize() == 0);
    ENetAddress address = {};
    address.port = 23513;
    char localhost[] = "localhost", ipv6[] = "::1", invalid[] = "256.0.0.1";
    CHECK(enet_address_set_host(&address, localhost) == 0);
    mapped(address);
    CHECK(address.port == 23513);
    CHECK(enet_address_set_host(&address, ipv6) < 0);
    CHECK(enet_address_set_host_ip(&address, ipv6) < 0);
    CHECK(enet_address_set_host_ip(&address, invalid) < 0);
    transfer(ENET_SOCKET_TYPE_DATAGRAM);
    transfer(ENET_SOCKET_TYPE_STREAM);
    ENetSocket socket = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
    CHECK(socket != ENET_SOCKET_NULL);
    std::memset(&address.host, 0, sizeof(address.host));
    reinterpret_cast<unsigned char*>(&address.host)[15] = 1;
    CHECK(enet_socket_bind(socket, &address) < 0);
    CHECK(enet_socket_connect(socket, &address) < 0);
    char bytes[64] = {};
    ENetBuffer buffer = {};
    buffer.data = bytes;
    buffer.dataLength = sizeof(bytes);
    CHECK(enet_socket_send(socket, &address, &buffer, 1) < 0);
    CHECK(enet_address_get_host_ip(&address, bytes, sizeof(bytes)) < 0);
    enet_socket_destroy(socket);
    enet_deinitialize();
    std::puts("PASS: IPv4-only UDP/TCP sockets, mapped-address roundtrip, DNS and IPv6 rejection");
}
