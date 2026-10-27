// SPDX-License-Identifier: MIT
// Copyright (c) 2026 NetBlood contributors. See LICENSE in this directory.
// Generic datagram bridge: deliberately independent of any game or game protocol.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
using NativeSocket = SOCKET;
constexpr NativeSocket InvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
using NativeSocket = int;
constexpr NativeSocket InvalidSocket = -1;
#endif
#include "ZeroTierSockets.h"
#include "zerotier_protocol.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <string>
#include <thread>

namespace
{
using Clock = std::chrono::steady_clock;
using Address = std::array<uint8_t, 4>;
struct Options
{
    uint16_t parentPort = 0;
    uint64_t network = 0;
    ztipc::Token token{};
    std::string identity;
};

bool parseUnsigned(const char* value, unsigned base, uint64_t limit, uint64_t& result)
{
    if (!value || !*value) return false;
    uint64_t n = 0;
    for (const char* p = value; *p; ++p)
    {
        unsigned v = *p >= '0' && *p <= '9' ? unsigned(*p - '0')
            : *p >= 'a' && *p <= 'f' ? unsigned(*p - 'a' + 10)
            : *p >= 'A' && *p <= 'F' ? unsigned(*p - 'A' + 10) : base;
        if (v >= base || n > (limit - v) / base) return false;
        n = n * base + v;
    }
    result = n;
    return true;
}

bool parseOptions(int argc, char** argv, Options& options)
{
    bool tokenSeen = false, networkSeen = false;
    for (int i = 1; i < argc; ++i)
    {
        std::string const key = argv[i];
        if (i + 1 >= argc) return false;
        char const* value = argv[++i];
        uint64_t number = 0;
        if (key == "--ipc-port")
        {
            if (options.parentPort || !parseUnsigned(value, 10, 65535, number) || !number) return false;
            options.parentPort = uint16_t(number);
        }
        else if (key == "--network")
        {
            if (networkSeen || std::strlen(value) != 16 || !parseUnsigned(value, 16, UINT64_MAX, number) || !number) return false;
            options.network = number;
            networkSeen = true;
        }
        else if (key == "--token-env")
        {
            if (tokenSeen || !*value || std::strlen(value) > 80) return false;
            for (const char* p = value; *p; ++p)
                if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')
                    || (*p >= '0' && *p <= '9') || *p == '_')) return false;
            const char* const token = std::getenv(value);
            if (!token || !ztipc::parseToken(token, options.token)) return false;
            // The parent gives this variable only to this child process. Do not
            // leave the token in argv or propagate it to any later subprocess.
#ifdef _WIN32
            _putenv_s(value, "");
#else
            unsetenv(value);
#endif
            tokenSeen = true;
        }
        else if (key == "--identity")
        {
            if (!options.identity.empty() || !*value) return false;
            options.identity = value;
        }
        else return false;
    }
    return options.parentPort && tokenSeen && networkSeen && !options.identity.empty();
}

void closeNative(NativeSocket socket)
{
#ifdef _WIN32
    if (socket != InvalidSocket) closesocket(socket);
#else
    if (socket != InvalidSocket) close(socket);
#endif
}

// A persistent node identity must never be used by two running node instances.
class IdentityLock
{
#ifdef _WIN32
    HANDLE handle = INVALID_HANDLE_VALUE;
#else
    int handle = -1;
#endif
public:
    bool acquire(const std::string& directory)
    {
        std::error_code error;
        bool const created = std::filesystem::create_directories(directory, error);
        if (error || !std::filesystem::is_directory(directory, error) || error) return false;
        auto const path = std::filesystem::path(directory) / "companion.lock";
#ifdef _WIN32
        (void)created;
        handle = CreateFileW(path.wstring().c_str(), GENERIC_READ | GENERIC_WRITE, 0,
            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        return handle != INVALID_HANDLE_VALUE;
#else
        if (created && chmod(directory.c_str(), S_IRWXU) < 0) return false;
        int flags = O_CREAT | O_RDWR;
#ifdef O_NOFOLLOW
        flags |= O_NOFOLLOW;
#endif
#ifdef O_CLOEXEC
        flags |= O_CLOEXEC;
#endif
        handle = open(path.c_str(), flags, S_IRUSR | S_IWUSR);
        return handle >= 0 && flock(handle, LOCK_EX | LOCK_NB) == 0;
#endif
    }
    ~IdentityLock()
    {
#ifdef _WIN32
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
#else
        if (handle >= 0) close(handle);
#endif
    }
};

std::atomic<bool> fatalNodeError{false};
void onEvent(void* message)
{
    auto const* event = static_cast<const zts_event_msg_t*>(message);
    if (event && event->event_code == ZTS_EVENT_NODE_FATAL_ERROR) fatalNodeError.store(true);
}

zts_sockaddr_in socketAddress(const Address& address, uint16_t port)
{
    zts_sockaddr_in out{};
    out.sin_len = sizeof(out);
    out.sin_family = ZTS_AF_INET;
    out.sin_port = htons(port);
    // libzt's address and family layouts differ from the operating system's.
    // Only the four network-order address bytes are shared by the two APIs.
    std::memcpy(&out.sin_addr, address.data(), address.size());
    return out;
}

struct PendingDatagram
{
    zts_sockaddr_in destination{};
    ztipc::Bytes bytes;
};
struct VirtualSocket
{
    int fd = -1;
    uint16_t port = 0;
    uint32_t generation = 0;
    uint16_t requestedPort = 0;
    uint8_t flags = 0;
    std::deque<PendingDatagram> outgoing;
    size_t queuedBytes = 0;
    void close()
    {
        if (fd >= 0) zts_bsd_close(fd);
        fd = -1;
        outgoing.clear();
        queuedBytes = 0;
    }
};

class Companion
{
    Options options;
    IdentityLock identityLock;
    NativeSocket ipc = InvalidSocket;
    sockaddr_in parent{};
    std::array<VirtualSocket, ztipc::MaxSocket + 1> sockets{};
    Address localAddress{};
    ztipc::State state = ztipc::State::Starting;
    std::string diagnostic = "Starting ZeroTier";
    bool nodeStarted = false, joined = false, stopping = false;
    Clock::time_point lastParent = Clock::now(), nextStatus = Clock::now(), nextJoin = Clock::now();

    bool send(ztipc::Type type, uint16_t socket, uint32_t request, const ztipc::Bytes& payload)
    {
        ztipc::Message message;
        message.type = type; message.socket = socket; message.request = request; message.payload = payload;
        auto const packet = ztipc::encode(message, options.token);
        int const count = sendto(ipc, reinterpret_cast<const char*>(packet.data()), int(packet.size()), 0,
            reinterpret_cast<const sockaddr*>(&parent), sizeof(parent));
        return count == int(packet.size());
    }

    void sendStatus()
    {
        send(ztipc::Type::Status, 0, 0, ztipc::statusPayload(state, localAddress.data(), diagnostic));
    }

    void updateStatus()
    {
        if (!nodeStarted) return;
        uint64_t const nodeId = zts_node_get_id();
        char node[32];
        std::snprintf(node, sizeof(node), " (node %010llx)", static_cast<unsigned long long>(nodeId));
        localAddress = {};
        if (fatalNodeError.load())
        {
            state = ztipc::State::Error;
            diagnostic = "ZeroTier node failed; restart the game";
        }
        else if (!zts_node_is_online())
        {
            state = ztipc::State::Waiting;
            diagnostic = "Connecting to ZeroTier";
        }
        else
        {
            if (!joined && Clock::now() >= nextJoin)
            {
                joined = zts_net_join(options.network) == ZTS_ERR_OK;
                nextJoin = Clock::now() + std::chrono::seconds(1);
            }
            int const status = zts_net_get_status(options.network);
            if (status == ZTS_NETWORK_STATUS_ACCESS_DENIED)
            {
                state = ztipc::State::Denied;
                diagnostic = "Waiting for network authorization";
            }
            else if (status == ZTS_NETWORK_STATUS_NOT_FOUND)
            {
                state = ztipc::State::Error;
                diagnostic = "ZeroTier network does not exist";
            }
            else if (status == ZTS_NETWORK_STATUS_PORT_ERROR || status == ZTS_NETWORK_STATUS_CLIENT_TOO_OLD)
            {
                state = ztipc::State::Error;
                diagnostic = "ZeroTier network configuration is incompatible";
            }
            else
            {
                char address[ZTS_IP_MAX_STR_LEN]{};
                if (joined && zts_net_transport_is_ready(options.network)
                    && zts_addr_get_str(options.network, ZTS_AF_INET, address, sizeof(address)) == ZTS_ERR_OK
                    && zts_inet_pton(ZTS_AF_INET, address, localAddress.data()) == 1)
                {
                    state = ztipc::State::Ready;
                    diagnostic = "Connected to ZeroTier";
                }
                else
                {
                    state = ztipc::State::Waiting;
                    diagnostic = "Waiting for ZeroTier network IPv4 configuration";
                }
            }
        }
        diagnostic += node;
    }

    void openSocket(const ztipc::Message& message)
    {
        uint16_t error = 0, port = 0;
        if (!message.socket || !message.request || message.payload.size() != 3
            || (message.payload[2] & ~ztipc::Broadcast)) error = 22;
        else
        {
            auto& socket = sockets[message.socket];
            uint16_t const requestedPort = ztipc::get16(message.payload.data());
            bool const newGeneration = socket.generation && message.request != socket.generation;
            // Close is a one-way datagram. A newer Open also withdraws the old
            // socket, recovering from a lost Close without admitting reordered
            // Open/Send/Close packets from the previous session.
            if (newGeneration && int32_t(message.request - socket.generation) <= 0) error = 16;
            else if (socket.fd >= 0 && !newGeneration)
            {
                if (socket.requestedPort != requestedPort || socket.flags != message.payload[2]) error = 16;
                else port = socket.port;
            }
            else if (state != ztipc::State::Ready) error = 100;
            else
            {
                socket.close();
                int const fd = zts_bsd_socket(ZTS_AF_INET, ZTS_SOCK_DGRAM, ZTS_IPPROTO_UDP);
                if (fd < 0) error = 5;
                else
                {
                    int const enabled = 1;
                    auto address = socketAddress(localAddress, requestedPort);
                    zts_socklen_t length = sizeof(address);
                    bool const broadcast = (message.payload[2] & ztipc::Broadcast) != 0;
                    if (zts_bsd_fcntl(fd, ZTS_F_SETFL, ZTS_O_NONBLOCK) < 0
                        || (broadcast && zts_bsd_setsockopt(fd, ZTS_SOL_SOCKET, ZTS_SO_BROADCAST,
                            &enabled, sizeof(enabled)) < 0)
                        || zts_bsd_bind(fd, reinterpret_cast<const zts_sockaddr*>(&address), sizeof(address)) < 0
                        || zts_bsd_getsockname(fd, reinterpret_cast<zts_sockaddr*>(&address), &length) < 0)
                    {
                        error = uint16_t(zts_errno > 0 && zts_errno < 65536 ? zts_errno : 5);
                        zts_bsd_close(fd);
                    }
                    else
                    {
                        socket.fd = fd;
                        socket.port = port = ntohs(address.sin_port);
                        socket.generation = message.request;
                        socket.requestedPort = requestedPort;
                        socket.flags = message.payload[2];
                    }
                }
            }
        }
        ztipc::Bytes payload;
        ztipc::put16(payload, error); ztipc::put16(payload, port);
        send(ztipc::Type::Opened, message.socket, message.request, payload);
    }

    void receiveControl()
    {
        std::array<uint8_t, ztipc::MaxPacket + 1> buffer{};
        for (int budget = 128; budget; --budget)
        {
            sockaddr_in sender{};
#ifdef _WIN32
            int senderLength = sizeof(sender);
#else
            socklen_t senderLength = sizeof(sender);
#endif
            int const count = recvfrom(ipc, reinterpret_cast<char*>(buffer.data()), int(buffer.size()), 0,
                reinterpret_cast<sockaddr*>(&sender), &senderLength);
            if (count < 0) break;
            if (senderLength != sizeof(sender) || sender.sin_family != AF_INET
                || sender.sin_addr.s_addr != parent.sin_addr.s_addr || sender.sin_port != parent.sin_port) continue;
            ztipc::Message message;
            if (!ztipc::decode(buffer.data(), size_t(count), options.token, message)) continue;
            lastParent = Clock::now();
            if (message.type == ztipc::Type::Stop && !message.socket && message.payload.empty()) stopping = true;
            else if (message.type == ztipc::Type::Keepalive && !message.socket && message.payload.empty())
                sendStatus();
            else if (message.type == ztipc::Type::Open) openSocket(message);
            else if (message.socket)
            {
                auto& socket = sockets[message.socket];
                if (socket.fd < 0 || socket.generation != message.request) continue;
                if (message.type == ztipc::Type::Close && message.payload.empty()) socket.close();
                else if (message.type == ztipc::Type::Send && message.payload.size() >= 6
                    && state == ztipc::State::Ready && socket.outgoing.size() < 64
                    && socket.queuedBytes + message.payload.size() <= 256 * 1024)
                {
                    uint16_t const port = ztipc::get16(message.payload.data() + 4);
                    if (!port) continue;
                    Address address{};
                    std::copy_n(message.payload.data(), address.size(), address.data());
                    PendingDatagram pending;
                    pending.destination = socketAddress(address, port);
                    pending.bytes.assign(message.payload.begin() + 6, message.payload.end());
                    socket.queuedBytes += pending.bytes.size();
                    socket.outgoing.push_back(std::move(pending));
                }
            }
        }
    }

    void pumpSockets()
    {
        if (state != ztipc::State::Ready) return;
        std::array<uint8_t, 65536> buffer{};
        for (uint16_t id = 1; id <= ztipc::MaxSocket; ++id)
        {
            auto& socket = sockets[id];
            if (socket.fd < 0) continue;
            for (int budget = 32; budget && !socket.outgoing.empty(); --budget)
            {
                auto const& pending = socket.outgoing.front();
                auto const count = zts_bsd_sendto(socket.fd, pending.bytes.data(), pending.bytes.size(), ZTS_MSG_DONTWAIT,
                    reinterpret_cast<const zts_sockaddr*>(&pending.destination), sizeof(pending.destination));
                if (count < 0 && (zts_errno == ZTS_EAGAIN || zts_errno == ZTS_EWOULDBLOCK)) break;
                socket.queuedBytes -= pending.bytes.size();
                socket.outgoing.pop_front();
            }
            for (int budget = 64; budget; --budget)
            {
                zts_sockaddr_in sender{};
                zts_socklen_t length = sizeof(sender);
                auto const count = zts_bsd_recvfrom(socket.fd, buffer.data(), buffer.size(), ZTS_MSG_DONTWAIT,
                    reinterpret_cast<zts_sockaddr*>(&sender), &length);
                if (count < 0) break;
                if (size_t(count) > ztipc::MaxPayload - 6 || length != sizeof(sender) || sender.sin_family != ZTS_AF_INET) continue;
                Address address{};
                std::memcpy(address.data(), &sender.sin_addr, address.size());
                send(ztipc::Type::Received, id, socket.generation,
                    ztipc::datagramPayload(address.data(), ntohs(sender.sin_port), buffer.data(), size_t(count)));
            }
        }
    }

public:
    explicit Companion(Options value) : options(std::move(value)) {}
    ~Companion()
    {
        for (auto& socket : sockets) socket.close();
        if (nodeStarted) zts_node_free();
        closeNative(ipc);
    }
    int run()
    {
        ipc = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (ipc == InvalidSocket) return 2;
        sockaddr_in local{};
        local.sin_family = AF_INET; local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(ipc, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) < 0) return 2;
#ifdef _WIN32
        u_long enabled = 1;
        if (ioctlsocket(ipc, FIONBIO, &enabled)) return 2;
#else
        if (fcntl(ipc, F_SETFL, O_NONBLOCK) < 0) return 2;
        fcntl(ipc, F_SETFD, FD_CLOEXEC);
#endif
        parent.sin_family = AF_INET; parent.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        parent.sin_port = htons(options.parentPort);
        if (!identityLock.acquire(options.identity))
        {
            state = ztipc::State::Error;
            diagnostic = "ZeroTier identity is unavailable or already in use";
        }
        else if (zts_init_from_storage(options.identity.c_str()) != ZTS_ERR_OK
            || zts_init_set_event_handler(onEvent) != ZTS_ERR_OK
            || zts_node_start() != ZTS_ERR_OK)
        {
            state = ztipc::State::Error;
            diagnostic = "Cannot start the bundled ZeroTier node";
        }
        else nodeStarted = true;
        sendStatus();
        while (!stopping && Clock::now() - lastParent < std::chrono::seconds(15))
        {
            receiveControl();
            if (Clock::now() >= nextStatus)
            {
                updateStatus(); sendStatus();
                nextStatus = Clock::now() + std::chrono::seconds(1);
            }
            pumpSockets();
            // Poll native and virtual sockets without blocking SDK callbacks.
            // libzt descriptors cannot be passed to the OS select/poll APIs.
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return 0;
    }
};
}

int main(int argc, char** argv)
{
    if (argc == 2 && (!std::strcmp(argv[1], "--help") || !std::strcmp(argv[1], "--version")))
    {
        std::puts("netblood-zerotier: independent ZeroTier UDP companion, IPC version 1");
        std::puts("--ipc-port PORT --token-env VARIABLE --network 16HEX --identity DIRECTORY");
        return 0;
    }
    Options options;
    if (!parseOptions(argc, argv, options))
    {
        std::fputs("Invalid companion arguments; use --help.\n", stderr);
        return 2;
    }
#ifdef _WIN32
    WSADATA winsock{};
    if (WSAStartup(MAKEWORD(2, 2), &winsock)) return 2;
#endif
    int result = 2;
    try { result = Companion(std::move(options)).run(); }
    catch (const std::exception&) { std::fputs("ZeroTier companion failed.\n", stderr); }
#ifdef _WIN32
    WSACleanup();
#endif
    return result;
}
