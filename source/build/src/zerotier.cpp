#include "zerotier.h"

#include "zerotier_browser.h"

#ifdef NETCODE_DISABLE
namespace zerotier
{
namespace
{
std::string         unavailable = "ZeroTier is unavailable in this build.";
std::vector<Server> empty;
}  // namespace
bool start(const Config &, std::string &error)
{
    error = unavailable;
    return false;
}
void stop()
{
}
void pump()
{
}
bool ready()
{ return false; }
State state()
{ return State::Error; }
const std::string &status()
{ return unavailable; }
std::string localAddress()
{ return {}; }
void advertise(const Advertisement *)
{
}
void browse(bool)
{
}
const std::vector<Server> &servers()
{ return empty; }
ENetHost *createHost(ENetAddress *, size_t, size_t, enet_uint32, enet_uint32)
{ return nullptr; }
}  // namespace zerotier
#else
#include "zerotier_protocol.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <thread>
#include <utility>
#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#endif

namespace zerotier
{
namespace
{
using Clock = std::chrono::steady_clock;
using Bytes = ztipc::Bytes;
uint64_t millis()
{ return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count()); }
std::string         lastStatus = "ZeroTier is not connected.";
State               lastState  = State::Stopped;
std::vector<Server> emptyServers;
uint32_t            requestSequence = 1;
std::string         ipv4Text(const uint8_t *p)
{
    char text[16];
    std::snprintf(text, sizeof(text), "%u.%u.%u.%u", p[0], p[1], p[2], p[3]);
    return text;
}
void addressFromIPv4(ENetAddress &address, const uint8_t *ip, uint16_t port)
{
    std::memset(&address, 0, sizeof(address));
    address.host.s6_addr[10] = address.host.s6_addr[11] = 0xff;
    std::memcpy(address.host.s6_addr + 12, ip, 4);
    address.port = port;
}
bool addressToIPv4(const ENetAddress &address, uint8_t *out)
{
    const uint8_t *p = address.host.s6_addr;
    for (int i = 0; i < 10; i++)
        if (p[i])
            return false;
    if (p[10] != 0xff || p[11] != 0xff || !address.port)
        return false;
    std::memcpy(out, p + 12, 4);
    return true;
}
bool sameAddress(const ENetAddress &a, const ENetAddress &b)
{ return a.port == b.port && !std::memcmp(a.host.s6_addr, b.host.s6_addr, 16); }
bool validIPv4(const uint8_t *p)
{ return p[0] && p[0] != 127 && p[0] < 224; }
bool makeToken(ztipc::Token &token)
{
#if defined(_WIN32)
    HMODULE library = LoadLibraryA("advapi32.dll");
    if (!library)
        return false;
    using RandomFunction    = BOOLEAN(WINAPI *)(PVOID, ULONG);
    FARPROC        function = GetProcAddress(library, "SystemFunction036");
    RandomFunction random   = nullptr;
    static_assert(sizeof(random) == sizeof(function), "Windows function pointer sizes differ");
    std::memcpy(&random, &function, sizeof(random));
    bool result = random && random(token.data(), ULONG(token.size()));
    FreeLibrary(library);
    return result;
#else
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0)
        return false;
    size_t done = 0;
    while (done < token.size())
    {
        ssize_t size = read(fd, token.data() + done, token.size() - done);
        if (size < 0 && errno == EINTR)
            continue;
        if (size <= 0)
        {
            close(fd);
            return false;
        }
        done += size_t(size);
    }
    close(fd);
    return true;
#endif
}
std::string environmentPath(const char *name)
{
    const char *value = std::getenv(name);
    return value ? value : "";
}
std::string executableDirectory()
{
    char buffer[4096] = {};
#if defined(_WIN32)
    DWORD size = GetModuleFileNameA(nullptr, buffer, sizeof(buffer));
    if (!size || size >= sizeof(buffer))
        return ".";
#elif defined(__APPLE__)
    uint32_t size = sizeof(buffer);
    if (_NSGetExecutablePath(buffer, &size))
        return ".";
#else
    ssize_t size = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (size < 0)
        return ".";
    buffer[size] = 0;
#endif
    std::string path  = buffer;
    size_t      split = path.find_last_of("/\\");
    return split == std::string::npos ? "." : path.substr(0, split);
}
std::string defaultIdentity(Game game)
{
    const char *name = game == Game::NBlood ? "netblood" : "rednukem";
#if defined(_WIN32)
    std::string root = environmentPath("APPDATA");
    if (root.empty())
        root = environmentPath("USERPROFILE");
    return root.empty() ? std::string() : root + "/" + name + "/zerotier";
#elif defined(__APPLE__)
    std::string root = environmentPath("HOME");
    return root.empty() ? std::string() : root + "/Library/Application Support/" + name + "/zerotier";
#else
    std::string root = environmentPath("XDG_DATA_HOME");
    if (root.empty())
    {
        root = environmentPath("HOME");
        if (!root.empty())
            root += "/.local/share";
    }
    return root.empty() ? std::string() : root + "/" + name + "/zerotier";
#endif
}
struct Process
{
#if defined(_WIN32)
    HANDLE             handle = nullptr;
    static std::string quote(const std::string &value)
    {
        std::string out     = "\"";
        size_t      slashes = 0;
        for (char c : value)
        {
            if (c == '\\')
            {
                ++slashes;
                continue;
            }
            if (c == '\"')
                out.append(slashes * 2 + 1, '\\');
            else
                out.append(slashes, '\\');
            slashes = 0;
            out += c;
        }
        out.append(slashes * 2, '\\');
        out += '\"';
        return out;
    }
#else
    pid_t pid = -1;
#endif
    bool launch(const std::string &executable, const std::vector<std::string> &arguments, const std::string &token)
    {
#if defined(_WIN32)
        std::string line = quote(executable);
        for (const auto &value : arguments)
        {
            line += ' ';
            line += quote(value);
        }
        std::vector<char> command(line.begin(), line.end());
        command.push_back(0);
        std::vector<char> environment;
        LPCH              inherited = GetEnvironmentStringsA();
        if (!inherited)
            return false;
        for (const char *entry = inherited; *entry; entry += std::strlen(entry) + 1)
            if (std::strncmp(entry, "NETBLOOD_ZT_IPC_TOKEN=", 22))
                environment.insert(environment.end(), entry, entry + std::strlen(entry) + 1);
        FreeEnvironmentStringsA(inherited);
        std::string credential = "NETBLOOD_ZT_IPC_TOKEN=" + token;
        environment.insert(environment.end(), credential.begin(), credential.end());
        environment.push_back(0);
        environment.push_back(0);
        STARTUPINFOA startup        = {};
        startup.cb                  = sizeof(startup);
        PROCESS_INFORMATION process = {};
        if (!CreateProcessA(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, environment.data(), nullptr, &startup, &process))
            return false;
        handle = process.hProcess;
        CloseHandle(process.hThread);
        return true;
#else
        if (access(executable.c_str(), X_OK))
            return false;
        std::vector<char *> argv;
        argv.push_back(const_cast<char *>(executable.c_str()));
        for (const auto &value : arguments) argv.push_back(const_cast<char *>(value.c_str()));
        argv.push_back(nullptr);
        std::vector<std::string> environment;
        for (char **entry = ::environ; *entry; ++entry)
            if (std::strncmp(*entry, "NETBLOOD_ZT_IPC_TOKEN=", 22))
                environment.emplace_back(*entry);
        environment.push_back("NETBLOOD_ZT_IPC_TOKEN=" + token);
        std::vector<char *> envp;
        for (auto &entry : environment) envp.push_back(&entry[0]);
        envp.push_back(nullptr);
        pid = fork();
        if (pid == 0)
        {
            for (int fd = 3; fd < 1024; fd++) close(fd);
            execve(executable.c_str(), argv.data(), envp.data());
            _exit(127);
        }
        return pid > 0;
#endif
    }
    bool running()
    {
#if defined(_WIN32)
        return handle && WaitForSingleObject(handle, 0) == WAIT_TIMEOUT;
#else
        if (pid <= 0)
            return false;
        int   status = 0;
        pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == pid || (result < 0 && errno == ECHILD))
        {
            pid = -1;
            return false;
        }
        return result == 0;
#endif
    }
    void finish()
    {
        // The authenticated Stop has already been sent. Bound shutdown protects
        // game exit even if the companion/SDK is stuck in a network operation.
        uint64_t until = millis() + 1500;
        while (running() && millis() < until) std::this_thread::sleep_for(std::chrono::milliseconds(5));
#if defined(_WIN32)
        if (handle)
        {
            if (running())
                TerminateProcess(handle, 0);
            CloseHandle(handle);
            handle = nullptr;
        }
#else
        if (pid > 0)
        {
            kill(pid, SIGTERM);
            until = millis() + 100;
            while (running() && millis() < until) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            if (pid > 0)
            {
                kill(pid, SIGKILL);
                int code = 0;
                while (waitpid(pid, &code, 0) < 0 && errno == EINTR) { }
                pid = -1;
            }
        }
#endif
    }
    ~Process()
    { finish(); }
};
struct Datagram
{
    ENetAddress address {};
    Bytes       data;
};
struct VirtualSocket
{
    enum class State
    {
        Opening,
        Open,
        Failed,
        Closed
    };
    uint16_t             id = 0, port = 0;
    uint32_t             request   = 0;
    bool                 broadcast = false;
    State                state     = State::Opening;
    uint64_t             created = 0, lastAttempt = 0;
    std::deque<Datagram> incoming;
};
struct Service
{
    Config       config;
    ENetSocket   ipc = ENET_SOCKET_NULL;
    ENetAddress  endpoint {};
    ztipc::Token token {};
    bool         endpointKnown = false, networkReady = false, pumping = false, browsing = false, announcing = false;
    uint8_t      address[4] = {};
    uint64_t     started = 0, lastStatusAt = 0, lastHeartbeat = 0, lastQuery = 0, lastResponseWindow = 0;
    unsigned     responseCount = 0;
    uint32_t     nextNonce     = 0;
    Process      process;
    std::array<std::shared_ptr<VirtualSocket>, ztipc::MaxSocket + 1> sockets {};
    std::shared_ptr<VirtualSocket>                                   discovery;
    Advertisement                                                    advertisement;
    ztbrowse::Browser                                                browser;
    size_t                                                           queuedBytes = 0;
    explicit Service(const Config &c) : config(c), browser(c.game)
    {
    }
    ~Service()
    {
        if (ipc != ENET_SOCKET_NULL)
        {
            ztipc::Message message;
            message.type = ztipc::Type::Stop;
            if (endpointKnown)
                for (int i = 0; i < 3; i++) send(message);
            enet_socket_destroy(ipc);
            ipc = ENET_SOCKET_NULL;
        }
        for (auto &socket : sockets)
            if (socket)
                socket->state = VirtualSocket::State::Closed;
        process.finish();
        enet_deinitialize();
    }
    bool send(const ztipc::Message &message)
    {
        if (!endpointKnown || ipc == ENET_SOCKET_NULL)
            return false;
        auto bytes = ztipc::encode(message, token);
        if (bytes.empty())
            return false;
        ENetBuffer buffer;
        buffer.data       = bytes.data();
        buffer.dataLength = bytes.size();
        return enet_socket_send(ipc, &endpoint, &buffer, 1) == int(bytes.size());
    }
    std::shared_ptr<VirtualSocket> openSocket(uint16_t port, bool broadcast)
    {
        for (uint16_t id = 1; id <= ztipc::MaxSocket; id++)
            if (!sockets[id])
            {
                auto socket       = std::make_shared<VirtualSocket>();
                socket->id        = id;
                socket->port      = port;
                socket->broadcast = broadcast;
                socket->created   = millis();
                socket->request   = ++requestSequence;
                if (!socket->request)
                    socket->request = ++requestSequence;
                sockets[id] = socket;
                attemptOpen(*socket);
                return socket;
            }
        return {};
    }
    void attemptOpen(VirtualSocket &socket)
    {
        ztipc::Message message;
        message.type    = ztipc::Type::Open;
        message.socket  = socket.id;
        message.request = socket.request;
        message.payload = ztipc::openPayload(socket.port, socket.broadcast);
        send(message);
        socket.lastAttempt = millis();
    }
    void closeSocket(const std::shared_ptr<VirtualSocket> &socket)
    {
        if (!socket || socket->state == VirtualSocket::State::Closed)
            return;
        ztipc::Message message;
        message.type    = ztipc::Type::Close;
        message.socket  = socket->id;
        message.request = socket->request;
        send(message);
        socket->state = VirtualSocket::State::Closed;
        for (const auto &packet : socket->incoming) queuedBytes -= packet.data.size();
        socket->incoming.clear();
        if (sockets[socket->id] == socket)
            sockets[socket->id].reset();
    }
    bool datagram(const std::shared_ptr<VirtualSocket> &socket, const uint8_t *target, uint16_t port, const Bytes &data)
    {
        if (!networkReady || !socket || socket->state != VirtualSocket::State::Open)
            return false;
        ztipc::Message message;
        message.type    = ztipc::Type::Send;
        message.socket  = socket->id;
        message.request = socket->request;
        message.payload = ztipc::datagramPayload(target, port, data.data(), data.size());
        return !message.payload.empty() && send(message);
    }
    void receive(const ztipc::Message &message)
    {
        if (message.type == ztipc::Type::Status)
        {
            const Bytes &p = message.payload;
            if (message.socket || message.request || p.size() < 5 || p.size() > 165 || p[0] > uint8_t(ztipc::State::Error))
                return;
            for (size_t i = 5; i < p.size(); i++)
                if (p[i] < 32 || p[i] > 126)
                    return;
            lastStatusAt = millis();
            std::memcpy(address, p.data() + 1, 4);
            switch (ztipc::State(p[0]))
            {
                case ztipc::State::Starting: lastState = State::Starting; break;
                case ztipc::State::Waiting: lastState = State::Waiting; break;
                case ztipc::State::Ready: lastState = State::Ready; break;
                case ztipc::State::Denied: lastState = State::Denied; break;
                case ztipc::State::Error: lastState = State::Error; break;
            }
            networkReady = lastState == State::Ready && validIPv4(address);
            if (lastState == State::Ready && !networkReady)
                lastState = State::Waiting;
            lastStatus.assign(reinterpret_cast<const char *>(p.data() + 5), p.size() - 5);
            if (lastStatus.empty())
                lastStatus = networkReady ? "ZeroTier is connected." : "Connecting to ZeroTier...";
            return;
        }
        if (!message.socket || message.socket > ztipc::MaxSocket)
            return;
        auto socket = sockets[message.socket];
        if (!socket || socket->request != message.request)
            return;
        if (message.type == ztipc::Type::Opened)
        {
            if (message.payload.size() != 4 || socket->state != VirtualSocket::State::Opening)
                return;
            uint16_t error = ztipc::get16(message.payload.data()), port = ztipc::get16(message.payload.data() + 2);
            socket->state = error || !port ? VirtualSocket::State::Failed : VirtualSocket::State::Open;
            if (socket->state == VirtualSocket::State::Open)
                socket->port = port;
            else
            {
                lastState  = State::Error;
                lastStatus = "ZeroTier could not open the game connection.";
            }
            return;
        }
        if (message.type != ztipc::Type::Received || message.payload.size() < 6 || socket->state != VirtualSocket::State::Open)
            return;
        const Bytes &p = message.payload;
        if (!validIPv4(p.data()) || !ztipc::get16(p.data() + 4) || socket->incoming.size() >= 256 || queuedBytes + p.size() - 6 > 4 * 1024 * 1024)
            return;
        Datagram packet;
        addressFromIPv4(packet.address, p.data(), ztipc::get16(p.data() + 4));
        packet.data.assign(p.begin() + 6, p.end());
        queuedBytes += packet.data.size();
        socket->incoming.push_back(std::move(packet));
    }
    void discoveryPump(uint64_t now)
    {
        if (!networkReady)
            return;
        if (!browsing && !announcing)
        {
            if (discovery)
            {
                closeSocket(discovery);
                discovery.reset();
            }
            return;
        }
        if (!discovery)
            discovery = openSocket(DiscoveryPort, true);
        if (!discovery || discovery->state != VirtualSocket::State::Open)
            return;
        if (browsing && (!lastQuery || now - lastQuery >= ztbrowse::QueryMillis))
        {
            if (!++nextNonce)
                ++nextNonce;
            Bytes                query        = ztbrowse::query(uint8_t(config.game), nextNonce);
            static const uint8_t broadcast[4] = { 255, 255, 255, 255 };
            if (datagram(discovery, broadcast, DiscoveryPort, query))
            {
                browser.sent(nextNonce, now);
                lastQuery = now;
            }
        }
        for (unsigned budget = 0; budget < 128 && !discovery->incoming.empty(); budget++)
        {
            Datagram packet = std::move(discovery->incoming.front());
            discovery->incoming.pop_front();
            queuedBytes -= packet.data.size();
            if (packet.address.port != DiscoveryPort)
                continue;
            uint8_t sender[4];
            if (!addressToIPv4(packet.address, sender))
                continue;
            ztbrowse::Query query;
            if (announcing && ztbrowse::readQuery(packet.data.data(), packet.data.size(), query) && query.game == uint8_t(config.game))
            {
                if (now - lastResponseWindow >= 1000)
                {
                    lastResponseWindow = now;
                    responseCount      = 0;
                }
                if (responseCount >= 32)
                    continue;
                Bytes reply = ztbrowse::reply(query, advertisement);
                if (!reply.empty() && datagram(discovery, sender, DiscoveryPort, reply))
                    ++responseCount;
            }
            else if (browsing)
                browser.receive(ipv4Text(sender), packet.data.data(), packet.data.size(), now);
        }
    }
    void pump()
    {
        if (pumping)
            return;
        pumping      = true;
        uint64_t now = millis();
        for (unsigned budget = 0; budget < 256; budget++)
        {
            uint8_t    bytes[ztipc::MaxPacket + 1];
            ENetBuffer buffer;
            buffer.data       = bytes;
            buffer.dataLength = sizeof(bytes);
            ENetAddress sender {};
            int         size = enet_socket_receive(ipc, &sender, &buffer, 1);
            if (size <= 0)
                break;
            uint8_t ip[4];
            if (!addressToIPv4(sender, ip) || ip[0] != 127 || ip[1] || ip[2] || ip[3] != 1)
                continue;
            if (endpointKnown && !sameAddress(endpoint, sender))
                continue;
            ztipc::Message message;
            if (!ztipc::decode(bytes, size, token, message))
                continue;
            if (!endpointKnown)
            {
                if (message.type != ztipc::Type::Status)
                    continue;
                endpoint      = sender;
                endpointKnown = true;
            }
            receive(message);
        }
        if (!process.running())
        {
            networkReady = false;
            lastState    = State::Error;
            lastStatus   = "The bundled ZeroTier companion could not start.";
        }
        else if ((lastStatusAt && now - lastStatusAt > 5000) || (!lastStatusAt && now - started > 10000))
        {
            networkReady = false;
            lastState    = State::Error;
            lastStatus   = "The ZeroTier companion stopped responding.";
        }
        if (endpointKnown && (!lastHeartbeat || now - lastHeartbeat >= 1000))
        {
            ztipc::Message message;
            message.type = ztipc::Type::Keepalive;
            send(message);
            lastHeartbeat = now;
        }
        for (auto &socket : sockets)
            if (socket && socket->state == VirtualSocket::State::Opening)
            {
                if (now - socket->created > 2000)
                {
                    socket->state = VirtualSocket::State::Failed;
                    lastState     = State::Error;
                    lastStatus    = "ZeroTier connection timed out.";
                }
                else if (now - socket->lastAttempt >= 200)
                    attemptOpen(*socket);
            }
        discoveryPump(now);
        browser.expire(now);
        pumping = false;
    }
};
std::shared_ptr<Service> service;
struct Transport
{
    std::weak_ptr<Service>         service;
    std::shared_ptr<VirtualSocket> socket;
};
int ENET_CALLBACK transportSend(void *context, ENetAddress *destination, ENetBuffer *buffers, size_t count)
{
    auto &transport = *static_cast<Transport *>(context);
    auto  active    = transport.service.lock();
    if (!active || !destination)
        return -1;
    uint8_t address[4];
    if (!addressToIPv4(*destination, address))
        return -1;
    Bytes packet;
    for (size_t i = 0; i < count; i++)
    {
        if (buffers[i].dataLength > ztipc::MaxPayload - 6 - packet.size())
            return -1;
        const uint8_t *bytes = static_cast<const uint8_t *>(buffers[i].data);
        if (buffers[i].dataLength)
            packet.insert(packet.end(), bytes, bytes + buffers[i].dataLength);
    }
    return active->datagram(transport.socket, address, destination->port, packet) ? int(packet.size()) : 0;
}
int ENET_CALLBACK transportReceive(void *context, ENetAddress *sender, ENetBuffer *buffers, size_t count)
{
    auto &transport = *static_cast<Transport *>(context);
    auto  active    = transport.service.lock();
    if (!active)
        return -1;
    active->pump();
    auto &incoming = transport.socket->incoming;
    if (incoming.empty())
        return 0;
    Datagram packet = std::move(incoming.front());
    incoming.pop_front();
    active->queuedBytes -= packet.data.size();
    size_t capacity = 0;
    for (size_t i = 0; i < count; i++) capacity += buffers[i].dataLength;
    if (capacity < packet.data.size())
        return -2;
    size_t offset = 0;
    for (size_t i = 0; i < count && offset < packet.data.size(); i++)
    {
        size_t copied = (std::min)(buffers[i].dataLength, packet.data.size() - offset);
        if (copied)
            std::memcpy(buffers[i].data, packet.data.data() + offset, copied);
        offset += copied;
    }
    if (sender)
        *sender = packet.address;
    return int(packet.data.size());
}
int ENET_CALLBACK transportWait(void *context, enet_uint32 *condition, enet_uint64 timeout)
{
    auto &transport = *static_cast<Transport *>(context);
    auto  active    = transport.service.lock();
    if (!active || !condition)
        return -1;
    enet_uint32 requested = *condition;
    *condition            = ENET_SOCKET_WAIT_NONE;
    uint64_t until        = millis() + std::min<enet_uint64>(timeout, 1000);
    do
    {
        active->pump();
        if ((requested & ENET_SOCKET_WAIT_RECEIVE) && !transport.socket->incoming.empty())
            *condition |= ENET_SOCKET_WAIT_RECEIVE;
        if (requested & ENET_SOCKET_WAIT_SEND)
            *condition |= ENET_SOCKET_WAIT_SEND;
        if (*condition || millis() >= until)
            return 0;
        enet_uint32 native = ENET_SOCKET_WAIT_RECEIVE;
        if (enet_socket_wait(active->ipc, &native, std::min<uint64_t>(10, until - millis())) < 0)
            return -1;
    } while (true);
}
void ENET_CALLBACK transportDestroy(void *context)
{
    std::unique_ptr<Transport> transport(static_cast<Transport *>(context));
    if (auto active = transport->service.lock())
        active->closeSocket(transport->socket);
}
}  // namespace
bool start(const Config &requested, std::string &error)
{
    Config config = requested;
    if (config.networkId.empty())
        config.networkId = ZEROTIER_NETWORK_ID;
    bool valid   = config.networkId.size() == 16;
    bool nonzero = false;
    for (char &ch : config.networkId)
    {
        if (ch >= 'A' && ch <= 'F')
            ch += char('a' - 'A');
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
            valid = false;
        nonzero |= ch != '0';
    }
    if (!valid || !nonzero)
    {
        error = config.networkId.empty() ? "The shared ZeroTier network has not been configured." : "The ZeroTier network ID must contain 16 hexadecimal digits.";
        if (!service)
        {
            lastStatus = error;
            lastState  = State::Error;
        }
        return false;
    }
    if (config.identityPath.empty())
        config.identityPath = defaultIdentity(config.game);
    if (config.helperPath.empty())
        config.helperPath = executableDirectory()
                            + "/netblood-zerotier"
#if defined(_WIN32)
                              ".exe"
#endif
        ;
    if (config.identityPath.empty())
    {
        error = "A per-user folder is required for the ZeroTier identity.";
        if (!service)
        {
            lastStatus = error;
            lastState  = State::Error;
        }
        return false;
    }
    if (service && service->config.networkId == config.networkId && service->config.game == config.game && service->config.identityPath == config.identityPath
        && service->config.helperPath == config.helperPath)
    {
        error.clear();
        return lastState != State::Error;
    }
    stop();
    if (enet_initialize())
    {
        error      = "Cannot initialize the ZeroTier connection.";
        lastState  = State::Error;
        lastStatus = error;
        return false;
    }
    auto active = std::make_shared<Service>(config);
    if (!makeToken(active->token))
    {
        error      = "Cannot create a secure ZeroTier companion session.";
        lastState  = State::Error;
        lastStatus = error;
        return false;
    }
    active->ipc                      = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
    static const uint8_t loopback[4] = { 127, 0, 0, 1 };
    ENetAddress          local {};
    addressFromIPv4(local, loopback, 0);
    if (active->ipc != ENET_SOCKET_NULL)
        enet_socket_set_option(active->ipc, ENET_SOCKOPT_IPV6_V6ONLY, 0);
    if (active->ipc == ENET_SOCKET_NULL || enet_socket_bind(active->ipc, &local) < 0 || enet_socket_set_option(active->ipc, ENET_SOCKOPT_NONBLOCK, 1) < 0
        || enet_socket_get_address(active->ipc, &local) < 0)
    {
        error      = "Cannot open the local ZeroTier companion connection.";
        lastState  = State::Error;
        lastStatus = error;
        return false;
    }
    enet_socket_set_option(active->ipc, ENET_SOCKOPT_RCVBUF, 4 * 1024 * 1024);
    enet_socket_set_option(active->ipc, ENET_SOCKOPT_SNDBUF, 4 * 1024 * 1024);
    std::vector<std::string> arguments
    = { "--ipc-port", std::to_string(local.port), "--token-env", "NETBLOOD_ZT_IPC_TOKEN", "--network", config.networkId, "--identity", config.identityPath };
    if (!active->process.launch(config.helperPath, arguments, ztipc::tokenText(active->token)))
    {
        error      = "The bundled ZeroTier companion was not found or could not start.";
        lastState  = State::Error;
        lastStatus = error;
        return false;
    }
    active->started        = millis();
    active->nextNonce      = ztipc::get32(active->token.data());
    service                = std::move(active);
    lastState              = State::Starting;
    lastStatus             = "Connecting to ZeroTier...";
    static bool registered = false;
    if (!registered)
    {
        std::atexit(stop);
        registered = true;
    }
    error.clear();
    return true;
}
void stop()
{
    service.reset();
    lastState  = State::Stopped;
    lastStatus = "ZeroTier is not connected.";
}
void pump()
{
    if (service)
        service->pump();
}
bool ready()
{ return service && service->networkReady; }
State state()
{ return lastState; }
const std::string &status()
{ return lastStatus; }
std::string localAddress()
{ return service && service->networkReady ? ipv4Text(service->address) : std::string(); }
void advertise(const Advertisement *value)
{
    if (!service)
        return;
    service->announcing = value != nullptr;
    if (value)
    {
        service->advertisement = *value;
        auto &map              = service->advertisement.map;
        auto &mode             = service->advertisement.mode;
        if (map.size() > 64)
            map.resize(64);
        if (mode.size() > 16)
            mode.resize(16);
        for (char &ch : map)
            if (ch < 32 || ch > 126)
                ch = '?';
        for (char &ch : mode)
            if (ch < 32 || ch > 126)
                ch = '?';
    }
}
void browse(bool enabled)
{
    if (service)
    {
        service->browsing = enabled;
        if (enabled)
            service->lastQuery = 0;
    }
}
const std::vector<Server> &servers()
{ return service ? service->browser.servers() : emptyServers; }
ENetHost *createHost(ENetAddress *address, size_t peers, size_t channels, enet_uint32 incoming, enet_uint32 outgoing)
{
    if (!service || !ready())
        return nullptr;
    if (address && address->port != GamePort)
    {
        lastStatus = "ZeroTier games use the fixed game port.";
        return nullptr;
    }
    auto socket = service->openSocket(address ? GamePort : 0, false);
    if (!socket)
        return nullptr;
    uint64_t until = millis() + 300;
    while (socket->state == VirtualSocket::State::Opening && millis() < until)
    {
        service->pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (socket->state != VirtualSocket::State::Open)
    {
        service->closeSocket(socket);
        lastStatus = "ZeroTier could not open the game connection.";
        return nullptr;
    }
    auto *context           = new Transport;
    context->service        = service;
    context->socket         = socket;
    ENetTransport transport = { context, transportSend, transportReceive, transportWait, transportDestroy };
    ENetAddress   bound {};
    addressFromIPv4(bound, service->address, socket->port);
    ENetHost *host = enet_host_create_with_transport(&bound, peers, channels, incoming, outgoing, &transport);
    if (!host)
    {
        service->closeSocket(socket);
        delete context;
    }
    return host;
}
}  // namespace zerotier
#endif
