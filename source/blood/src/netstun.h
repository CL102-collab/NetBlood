// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef BLOOD_NETSTUN_H
#define BLOOD_NETSTUN_H

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Unauthenticated RFC 5389/8489 Binding discovery, independent of sockets.
// The caller must match the response source to the server it queried, generate
// unpredictable transaction IDs, and expire outstanding transactions. STUN
// discovers a mapping; a successful response does not prove inbound reachability.
namespace netstun
{
using Transaction = std::array<uint8_t, 12>;
struct Address
{
    std::array<uint8_t, 16> host{}; // IPv4 is represented as IPv4-mapped IPv6.
    uint16_t port = 0;              // Host byte order.
};

namespace detail
{
inline uint16_t read16(const uint8_t* p)
{ return uint16_t(uint16_t(p[0]) << 8 | p[1]); }
inline uint32_t read32(const uint8_t* p)
{ return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

inline uint32_t fingerprint(const uint8_t* bytes, size_t size)
{
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < size; ++i)
    {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return (crc ^ 0xffffffffu) ^ 0x5354554eu;
}

inline bool decodeAddress(const uint8_t* bytes, size_t size, bool xored,
                          const Transaction& transaction, Address& address,
                          uint8_t& family, std::string& error)
{
    if (size < 4 || bytes[0] != 0)
    {
        error = "Malformed STUN mapped address.";
        return false;
    }
    family = bytes[1];
    if ((family != 1 && family != 2) || size != (family == 1 ? 8u : 20u))
    {
        error = "Invalid STUN mapped address family or length.";
        return false;
    }
    address = {};
    address.port = read16(bytes + 2) ^ (xored ? 0x2112 : 0);
    if (!address.port)
    {
        error = "STUN returned a zero mapped port.";
        return false;
    }
    const uint8_t cookie[] = {0x21, 0x12, 0xa4, 0x42};
    const unsigned length = family == 1 ? 4 : 16;
    const unsigned offset = family == 1 ? 12 : 0;
    if (family == 1) address.host[10] = address.host[11] = 0xff;
    for (unsigned i = 0; i < length; ++i)
        address.host[offset + i] = bytes[4 + i] ^
            (xored ? (i < 4 ? cookie[i] : transaction[i - 4]) : 0);
    return true;
}
} // namespace detail

// A cookie-bearing STUN datagram can be consumed before an ENet decoder sees it.
// This recognizes even a truncated/invalid message; bindingResponse validates it.
inline bool isStun(const uint8_t* bytes, size_t size)
{
    return bytes && size >= 8 && (bytes[0] & 0xc0) == 0 &&
        detail::read32(bytes + 4) == 0x2112a442u;
}

inline std::vector<uint8_t> bindingRequest(const Transaction& transaction)
{
    std::vector<uint8_t> request{0x00, 0x01, 0x00, 0x00, 0x21, 0x12, 0xa4, 0x42};
    request.insert(request.end(), transaction.begin(), transaction.end());
    return request;
}

// Decodes only Binding success responses. Failure never modifies address.
// Optional fingerprints are checked. MESSAGE-INTEGRITY attributes are accepted
// but cannot be authenticated without credentials; this codec provides no trust
// beyond transaction/source correlation. Unsupported mandatory attributes fail.
inline bool bindingResponse(const uint8_t* bytes, size_t size,
                            const Transaction& transaction, Address& address,
                            std::string& error)
{
    error = "Malformed STUN Binding response header.";
    if (!isStun(bytes, size) || size < 20 || detail::read16(bytes) != 0x0101)
        return false;
    const size_t payloadLength = detail::read16(bytes + 2);
    if ((payloadLength & 3) || size != 20 + payloadLength)
    {
        error = "STUN response length does not match the datagram.";
        return false;
    }
    for (size_t i = 0; i < transaction.size(); ++i)
        if (bytes[8 + i] != transaction[i])
        {
            error = "STUN response transaction does not match the request.";
            return false;
        }

    Address mapped, xored;
    uint8_t mappedFamily = 0, xoredFamily = 0;
    std::bitset<65536> attributes;
    for (size_t offset = 20; offset < size;)
    {
        if (size - offset < 4)
        {
            error = "Truncated STUN attribute header.";
            return false;
        }
        const uint16_t type = detail::read16(bytes + offset);
        const size_t length = detail::read16(bytes + offset + 2);
        const size_t padded = (length + 3) & ~size_t(3);
        if (padded > size - offset - 4)
        {
            error = "Truncated STUN attribute value or padding.";
            return false;
        }
        if (attributes.test(type))
        {
            error = "Duplicate STUN response attribute.";
            return false;
        }
        attributes.set(type);
        const uint8_t* value = bytes + offset + 4;
        if (type == 0x0001 || type == 0x0020)
        {
            if (!detail::decodeAddress(value, length, type == 0x0020, transaction,
                                      type == 0x0020 ? xored : mapped,
                                      type == 0x0020 ? xoredFamily : mappedFamily, error))
                return false;
        }
        else if (type == 0x0008 || type == 0x001c)
        {
            if ((type == 0x0008 && length != 20) ||
                (type == 0x001c && (length < 16 || length > 32 || (length & 3))))
            {
                error = "Invalid STUN message integrity length.";
                return false;
            }
        }
        else if (type == 0x8028)
        {
            if (length != 4 || offset + 8 != size ||
                detail::read32(value) != detail::fingerprint(bytes, offset))
            {
                error = "Invalid STUN fingerprint.";
                return false;
            }
        }
        else if (type < 0x8000)
        {
            error = "Unsupported mandatory STUN response attribute.";
            return false;
        }
        offset += 4 + padded;
    }
    if (!mappedFamily && !xoredFamily)
    {
        error = "STUN response has no mapped address.";
        return false;
    }
    if (mappedFamily && xoredFamily && mappedFamily != xoredFamily)
    {
        error = "STUN mapped address families conflict.";
        return false;
    }
    address = xoredFamily ? xored : mapped;
    error.clear();
    return true;
}
} // namespace netstun
#endif
