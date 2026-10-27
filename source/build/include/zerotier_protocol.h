/*
 * Generic datagram companion protocol. MIT License.
 * Copyright (c) 2026 NetBlood contributors
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#ifndef ZEROTIER_COMPANION_PROTOCOL_H
#define ZEROTIER_COMPANION_PROTOCOL_H
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace ztipc
{
using Bytes                    = std::vector<uint8_t>;
using Token                    = std::array<uint8_t, 16>;
constexpr size_t   HeaderBytes = 32;
constexpr size_t   MaxPayload  = 16384;
constexpr size_t   MaxPacket   = HeaderBytes + MaxPayload;
constexpr uint16_t MaxSocket   = 64;
// All integers are little endian. IPv4 addresses are four network-order bytes.
enum class Type : uint8_t
{
    Keepalive = 1,
    Status    = 2,
    Open      = 3,
    Opened    = 4,
    Send      = 5,
    Received  = 6,
    Close     = 7,
    Stop      = 8
};
enum class State : uint8_t
{
    Starting = 0,
    Waiting  = 1,
    Ready    = 2,
    Denied   = 3,
    Error    = 4
};
constexpr uint8_t Broadcast = 1;
struct Message
{
    Type     type    = Type::Keepalive;
    uint16_t socket  = 0;
    uint32_t request = 0;
    Bytes    payload;
};
inline void put16(Bytes &b, uint16_t n)
{
    b.push_back(uint8_t(n));
    b.push_back(uint8_t(n >> 8));
}
inline void put32(Bytes &b, uint32_t n)
{
    put16(b, uint16_t(n));
    put16(b, uint16_t(n >> 16));
}
inline uint16_t get16(const uint8_t *p)
{ return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }
inline uint32_t get32(const uint8_t *p)
{ return uint32_t(get16(p)) | (uint32_t(get16(p + 2)) << 16); }
inline bool parseToken(const std::string &text, Token &out)
{
    if (text.size() != 32)
        return false;
    Token result {};
    for (size_t i = 0; i < 16; i++)
    {
        unsigned n = 0;
        for (size_t j = 0; j < 2; j++)
        {
            char     c = text[2 * i + j];
            unsigned v = c >= '0' && c <= '9'     ? unsigned(c - '0')
                         : c >= 'a' && c <= 'f'   ? unsigned(c - 'a' + 10)
                           : c >= 'A' && c <= 'F' ? unsigned(c - 'A' + 10)
                                                  : 16;
            if (v > 15)
                return false;
            n = n * 16 + v;
        }
        result[i] = uint8_t(n);
    }
    out = result;
    return true;
}
inline std::string tokenText(const Token &token)
{
    static const char hex[] = "0123456789abcdef";
    std::string       text;
    for (auto b : token)
    {
        text += hex[b >> 4];
        text += hex[b & 15];
    }
    return text;
}
inline Bytes encode(const Message &message, const Token &token)
{
    if (message.payload.size() > MaxPayload || message.socket > MaxSocket)
        return {};
    Bytes out { 'Z', 'T', 'I', 'P', 1, uint8_t(message.type) };
    put16(out, message.socket);
    out.insert(out.end(), token.begin(), token.end());
    put32(out, message.request);
    put16(out, uint16_t(message.payload.size()));
    put16(out, 0);
    out.insert(out.end(), message.payload.begin(), message.payload.end());
    return out;
}
inline bool decode(const uint8_t *bytes, size_t size, const Token &token, Message &result)
{
    if (!bytes || size < HeaderBytes || size > MaxPacket || std::memcmp(bytes, "ZTIP", 4) || bytes[4] != 1 || bytes[5] < uint8_t(Type::Keepalive)
        || bytes[5] > uint8_t(Type::Stop) || get16(bytes + 6) > MaxSocket || get16(bytes + 30) != 0 || size != HeaderBytes + get16(bytes + 28))
        return false;
    uint8_t difference = 0;
    for (size_t i = 0; i < token.size(); i++) difference |= uint8_t(token[i] ^ bytes[8 + i]);
    if (difference)
        return false;
    Message next;
    next.type    = Type(bytes[5]);
    next.socket  = get16(bytes + 6);
    next.request = get32(bytes + 24);
    next.payload.assign(bytes + HeaderBytes, bytes + size);
    result = std::move(next);
    return true;
}
// Open payload: uint16 port, uint8 flags (Broadcast only). Open is idempotent
// for a logical socket ID. A strictly newer request generation replaces an
// older socket, so a lost Close does not prevent reuse. Retain the last socket
// generation after Close and reject older Open/Send/Close requests, including
// delayed retries. Compare generations with signed modular 32-bit subtraction.
// Opened: uint16 error (0=success), uint16 bound port.
inline Bytes openPayload(uint16_t port, bool broadcast)
{
    Bytes b;
    put16(b, port);
    b.push_back(broadcast ? Broadcast : 0);
    return b;
}
// Send/Received payload: IPv4[4], uint16 port, then unmodified datagram bytes.
// Send/Received/Close header.request is the Open request generation; stale
// packets for a closed/reused logical socket must be rejected in both directions.
inline Bytes datagramPayload(const uint8_t *address, uint16_t port, const uint8_t *data, size_t size)
{
    if (!address || !port || size > MaxPayload - 6 || (size && !data))
        return {};
    Bytes out(address, address + 4);
    put16(out, port);
    if (size)
        out.insert(out.end(), data, data + size);
    return out;
}
// Status: uint8 State, IPv4[4] (zero until assigned), <=160 printable ASCII
// diagnostic bytes. Header socket/request are zero. Both directions send
// keepalive at least once per second; helper exits after 15s without a valid
// authenticated parent packet. The parent accepts one loopback helper endpoint.
inline Bytes statusPayload(State state, const uint8_t *address, const std::string &text)
{
    Bytes out { uint8_t(state) };
    if (address)
        out.insert(out.end(), address, address + 4);
    else
        out.insert(out.end(), 4, 0);
    for (char c : text)
    {
        if (out.size() >= 165)
            break;
        out.push_back(uint8_t(c >= ' ' && c <= '~' ? c : '?'));
    }
    return out;
}
}  // namespace ztipc
#endif
