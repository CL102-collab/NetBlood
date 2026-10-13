// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef NETCODE_DISABLE
#include "netstun_client.h"
#include "netstun.h"

#include <algorithm>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace netstun
{
namespace
{
bool randomBytes(uint8_t* bytes, size_t size)
{
#ifdef _WIN32
    HCRYPTPROV provider = 0;
    if (!CryptAcquireContext(&provider, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT))
        return false;
    const bool success = CryptGenRandom(provider, DWORD(size), bytes) != 0;
    CryptReleaseContext(provider, 0);
    return success;
#else
    const int descriptor = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) return false;
    size_t received = 0;
    while (received < size)
    {
        const ssize_t result = read(descriptor, bytes + received, size - received);
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0) break;
        received += size_t(result);
    }
    close(descriptor);
    return received == size;
#endif
}

uint32_t elapsed(uint32_t now, uint32_t then) { return now - then; }

bool sameAddress(const ENetAddress& a, const ENetAddress& b)
{
    return a.port == b.port && a.sin6_scope_id == b.sin6_scope_id &&
        std::memcmp(&a.host, &b.host, sizeof(a.host)) == 0;
}

bool validAddress(const ENetAddress& address)
{
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&address.host);
    if (!address.port || bytes[0] == 0xff) return false;
    bool any = false, mapped = true;
    for (size_t i = 0; i < 16; ++i) any |= bytes[i] != 0;
    if (!any) return false;
    for (size_t i = 0; i < 10; ++i) mapped &= bytes[i] == 0;
    mapped &= bytes[10] == 0xff && bytes[11] == 0xff;
    // Reject IPv4 unspecified, multicast and broadcast/reserved endpoints.
    // Private/loopback remain useful for LAN-hosted STUN and local testing.
    return !mapped || (bytes[12] != 0 && bytes[12] < 224);
}
}

struct Client::Impl
{
    ENetHost* host = nullptr;
    std::vector<ENetAddress> endpoints;
    Transaction transaction{};
    ENetAddress observed{};
    uint32_t now = 0, attemptStarted = 0, idleStarted = 0, lastSend = 0, lastSuccess = 0;
    size_t endpointIndex = 0, attempts = 0;
    bool waiting = false, sent = false, haveAddress = false;
    std::string diagnostic;

    void begin()
    {
        if (!randomBytes(transaction.data(), transaction.size()))
        {
            waiting = false;
            idleStarted = now;
            diagnostic = "Cannot generate a secure STUN transaction ID.";
            return;
        }
        waiting = true;
        sent = false;
        attemptStarted = now;
        ++attempts;
    }

    void send()
    {
        const auto bytes = bindingRequest(transaction);
        ENetAddress destination = endpoints[endpointIndex];
        ENetBuffer buffer{};
        buffer.data = const_cast<uint8_t*>(bytes.data());
        buffer.dataLength = bytes.size();
        enet_socket_send(host->socket, &destination, &buffer, 1);
        sent = true;
        lastSend = now;
    }

    void update()
    {
        if (!host || endpoints.empty()) return;
        if (haveAddress && elapsed(now, lastSuccess) >= 60000) haveAddress = false;
        if (!waiting && elapsed(now, idleStarted) >= 30000)
        {
            attempts = 0;
            begin();
        }
        if (waiting && elapsed(now, attemptStarted) >= 10000)
        {
            waiting = false;
            endpointIndex = (endpointIndex + 1) % endpoints.size();
            if (attempts < endpoints.size())
            {
                diagnostic = "STUN server timed out; trying the next discovery server.";
                begin();
            }
            else
            {
                idleStarted = now;
                diagnostic = "All STUN discovery servers timed out; retrying in 30 seconds. Direct connections are unaffected.";
            }
        }
        if (waiting && (!sent || elapsed(now, lastSend) >= 1000)) send();
    }
};

Client::Client() : m(new Impl) {}
Client::~Client() { stop(); }

bool Client::start(ENetHost* host, uint32_t now)
{
    stop();
    m.reset(new Impl);
    m->now = m->idleStarted = now;
    if (!host || host->socket == ENET_SOCKET_NULL)
    {
        m->diagnostic = "STUN discovery requires an active ENet UDP socket.";
        return false;
    }
    m->host = host;
    return true;
}

bool Client::configure(const std::vector<ENetAddress>& endpoints, uint32_t now, std::string& error)
{
    error.clear();
    if (!m->host)
        error = "STUN discovery requires an active ENet UDP socket.";
    else if (endpoints.size() > 8 || std::any_of(endpoints.begin(), endpoints.end(),
             [](const ENetAddress& address) { return !validAddress(address); }))
        error = "STUN supports at most eight discovery servers, each with a valid unicast endpoint and nonzero port.";
    if (!error.empty()) return false;

    m->endpoints = endpoints;
    m->now = m->idleStarted = now;
    m->haveAddress = m->waiting = m->sent = false;
    m->endpointIndex = m->attempts = 0;
    m->diagnostic.clear();
    if (!endpoints.empty())
    {
        m->begin();
        if (!m->waiting) { error = m->diagnostic; return false; }
        m->update();
    }
    return true;
}

void Client::pump(uint32_t now) { m->now = now; m->update(); }

int Client::intercept(ENetHost* host)
{
    if (!host || host != m->host) return 0;
    const uint8_t* bytes = host->receivedData;
    const size_t size = host->receivedDataLength;
    if (!isStun(bytes, size)) return 0;
    if (m->waiting && sameAddress(host->receivedAddress, m->endpoints[m->endpointIndex]) &&
        size >= 20 && std::memcmp(bytes + 8, m->transaction.data(), m->transaction.size()) == 0)
    {
        Address decoded;
        std::string error;
        if (bindingResponse(bytes, size, m->transaction, decoded, error))
        {
            ENetAddress address{};
            std::memcpy(&address.host, decoded.host.data(), decoded.host.size());
            address.port = decoded.port;
            if (validAddress(address))
            {
                m->observed = address;
                m->haveAddress = true;
                m->waiting = false;
                m->lastSuccess = m->idleStarted = m->now;
                m->diagnostic.clear();
            }
            else m->diagnostic = "STUN returned an invalid unicast address.";
        }
        else m->diagnostic = error;
    }
    return 1;
}

void Client::stop()
{
    if (!m) return;
    m->host = nullptr;
    m->endpoints.clear();
    m->haveAddress = m->waiting = m->sent = false;
    m->diagnostic.clear();
}

bool Client::active() const { return m->host && !m->endpoints.empty(); }
bool Client::discoveredAddress(ENetAddress& address) const
{
    if (!m->host || !m->haveAddress) return false;
    address = m->observed;
    return true;
}
std::string Client::status() const
{
    if (!active()) return "disabled";
    return m->haveAddress ? "STUN mapping discovered (reachability not verified)" :
        "discovering UDP mapping with STUN";
}
std::string Client::error() const { return m->diagnostic; }
}
#endif
