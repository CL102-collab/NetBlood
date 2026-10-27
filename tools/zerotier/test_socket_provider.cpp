// SPDX-License-Identifier: MIT
// Test-only socket provider. NEVER link or distribute this as the companion SDK.
// Implements the pinned SDK interface over native loopback sockets so the
// complete companion IPC/queue/address boundary can be tested without a controller.
#include "ZeroTierSockets.h"
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

extern "C"
{
int zts_errno = 0;
int zts_init_from_storage(const char*) { return 0; }
int zts_init_set_event_handler(void (*)(void*)) { return 0; }
int zts_node_start() { return 0; }
int zts_node_free() { return 0; }
int zts_node_is_online() { return 1; }
uint64_t zts_node_get_id() { return UINT64_C(0x123456789a); }
int zts_net_join(uint64_t) { return 0; }
int zts_net_get_status(uint64_t) { return ZTS_NETWORK_STATUS_OK; }
int zts_net_transport_is_ready(uint64_t) { return 1; }
int zts_addr_get_str(uint64_t, unsigned family, char* out, unsigned length)
{
    if (family != ZTS_AF_INET || length < 10) return ZTS_ERR_ARG;
    std::strcpy(out, "127.0.0.1");
    return 0;
}
int zts_inet_pton(int family, const char* text, void* out)
{ return family == ZTS_AF_INET ? inet_pton(AF_INET, text, out) : -1; }

int zts_bsd_socket(int, int, int) { return socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); }
int zts_bsd_close(int fd) { return close(fd); }
int zts_bsd_fcntl(int fd, int, int flags) { return fcntl(fd, F_SETFL, flags & ZTS_O_NONBLOCK ? O_NONBLOCK : 0); }
int zts_bsd_setsockopt(int fd, int, int option, const void* value, zts_socklen_t size)
{ return option == ZTS_SO_BROADCAST ? setsockopt(fd, SOL_SOCKET, SO_BROADCAST, value, size) : -1; }

sockaddr_in nativeAddress(const zts_sockaddr* address)
{
    const auto* from = reinterpret_cast<const zts_sockaddr_in*>(address);
    sockaddr_in to{};
    to.sin_family = AF_INET; to.sin_port = from->sin_port;
    std::memcpy(&to.sin_addr, &from->sin_addr, 4);
    return to;
}
void virtualAddress(const sockaddr_in& from, zts_sockaddr* address, zts_socklen_t* size)
{
    auto* to = reinterpret_cast<zts_sockaddr_in*>(address);
    *to = {};
    to->sin_len = sizeof(*to); to->sin_family = ZTS_AF_INET; to->sin_port = from.sin_port;
    std::memcpy(&to->sin_addr, &from.sin_addr, 4);
    *size = sizeof(*to);
}
int zts_bsd_bind(int fd, const zts_sockaddr* address, zts_socklen_t)
{
    const auto to = nativeAddress(address);
    const int result = bind(fd, reinterpret_cast<const sockaddr*>(&to), sizeof(to));
    zts_errno = errno;
    return result;
}
int zts_bsd_getsockname(int fd, zts_sockaddr* address, zts_socklen_t* size)
{
    sockaddr_in from{};
    socklen_t length = sizeof(from);
    const int result = getsockname(fd, reinterpret_cast<sockaddr*>(&from), &length);
    if (result == 0) virtualAddress(from, address, size);
    zts_errno = errno;
    return result;
}
ssize_t zts_bsd_sendto(int fd, const void* data, size_t size, int, const zts_sockaddr* address, zts_socklen_t)
{
    const auto to = nativeAddress(address);
    const auto result = sendto(fd, data, size, MSG_DONTWAIT, reinterpret_cast<const sockaddr*>(&to), sizeof(to));
    zts_errno = errno == EAGAIN ? ZTS_EAGAIN : errno;
    return result;
}
ssize_t zts_bsd_recvfrom(int fd, void* data, size_t size, int, zts_sockaddr* address, zts_socklen_t* length)
{
    sockaddr_in from{};
    socklen_t fromSize = sizeof(from);
    const auto result = recvfrom(fd, data, size, MSG_DONTWAIT, reinterpret_cast<sockaddr*>(&from), &fromSize);
    if (result >= 0) virtualAddress(from, address, length);
    zts_errno = errno == EAGAIN ? ZTS_EAGAIN : errno;
    return result;
}
}
