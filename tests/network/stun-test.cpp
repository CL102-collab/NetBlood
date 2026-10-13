// SPDX-License-Identifier: GPL-2.0-or-later
#include "netstun.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace
{
unsigned checks = 0;
void check(bool condition, const char* expression, int line)
{
    ++checks;
    if (!condition)
    {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, expression);
        std::exit(1);
    }
}
#define CHECK(x) check(bool(x), #x, __LINE__)
using Bytes = std::vector<uint8_t>;
const netstun::Transaction transaction{{0xb7, 0xe7, 0xa7, 0x01, 0xbc, 0x34,
                                       0xd6, 0x86, 0xfa, 0x87, 0xdf, 0xae}};
// Published success responses from RFC 5769 sections 2.2 and 2.3. These
// independently exercise XOR address decoding and the optional fingerprint.
const Bytes rfcIPv4{
    0x01,0x01,0x00,0x3c, 0x21,0x12,0xa4,0x42,
    0xb7,0xe7,0xa7,0x01, 0xbc,0x34,0xd6,0x86, 0xfa,0x87,0xdf,0xae,
    0x80,0x22,0x00,0x0b, 0x74,0x65,0x73,0x74, 0x20,0x76,0x65,0x63, 0x74,0x6f,0x72,0x20,
    0x00,0x20,0x00,0x08, 0x00,0x01,0xa1,0x47, 0xe1,0x12,0xa6,0x43,
    0x00,0x08,0x00,0x14, 0x2b,0x91,0xf5,0x99, 0xfd,0x9e,0x90,0xc3,
    0x8c,0x74,0x89,0xf9, 0x2a,0xf9,0xba,0x53, 0xf0,0x6b,0xe7,0xd7,
    0x80,0x28,0x00,0x04, 0xc0,0x7d,0x4c,0x96};
const Bytes rfcIPv6{
    0x01,0x01,0x00,0x48, 0x21,0x12,0xa4,0x42,
    0xb7,0xe7,0xa7,0x01, 0xbc,0x34,0xd6,0x86, 0xfa,0x87,0xdf,0xae,
    0x80,0x22,0x00,0x0b, 0x74,0x65,0x73,0x74, 0x20,0x76,0x65,0x63, 0x74,0x6f,0x72,0x20,
    0x00,0x20,0x00,0x14, 0x00,0x02,0xa1,0x47, 0x01,0x13,0xa9,0xfa,
    0xa5,0xd3,0xf1,0x79, 0xbc,0x25,0xf4,0xb5, 0xbe,0xd2,0xb9,0xd9,
    0x00,0x08,0x00,0x14, 0xa3,0x82,0x95,0x4e, 0x4b,0xe6,0x7b,0xf1,
    0x17,0x84,0xc9,0x7c, 0x82,0x92,0xc2,0x75, 0xbf,0xe3,0xed,0x41,
    0x80,0x28,0x00,0x04, 0xc8,0xfb,0x0b,0x4c};
const std::array<uint8_t,16> ipv4{{0,0,0,0,0,0,0,0,0,0,255,255,192,0,2,1}};
const std::array<uint8_t,16> ipv6{{0x20,0x01,0x0d,0xb8,0x12,0x34,0x56,0x78,
                                0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77}};
void length(Bytes& bytes)
{
    bytes[2] = uint8_t((bytes.size() - 20) >> 8);
    bytes[3] = uint8_t(bytes.size() - 20);
}
Bytes header()
{
    auto bytes = netstun::bindingRequest(transaction);
    bytes[0] = 1;
    return bytes;
}
void attribute(Bytes& bytes, uint16_t type, const Bytes& value)
{
    bytes.push_back(uint8_t(type >> 8)); bytes.push_back(uint8_t(type));
    bytes.push_back(uint8_t(value.size() >> 8)); bytes.push_back(uint8_t(value.size()));
    bytes.insert(bytes.end(), value.begin(), value.end());
    while (bytes.size() & 3) bytes.push_back(0xa5); // Padding need not be zero.
    length(bytes);
}
Bytes minimal(bool ipv6Address = false, bool xored = true)
{
    auto bytes = header();
    if (xored)
    {
        if (ipv6Address)
            attribute(bytes, 0x20, {0,2,0xa1,0x47,0x01,0x13,0xa9,0xfa,0xa5,0xd3,
                                   0xf1,0x79,0xbc,0x25,0xf4,0xb5,0xbe,0xd2,0xb9,0xd9});
        else attribute(bytes, 0x20, {0,1,0xa1,0x47,0xe1,0x12,0xa6,0x43});
    }
    else
    {
        Bytes value{0,uint8_t(ipv6Address ? 2 : 1),0x80,0x55};
        const auto& host = ipv6Address ? ipv6 : ipv4;
        value.insert(value.end(), host.begin() + (ipv6Address ? 0 : 12), host.end());
        attribute(bytes, 1, value);
    }
    return bytes;
}
bool accepts(const Bytes& bytes, const std::array<uint8_t,16>& host = ipv4, uint16_t port = 32853)
{
    netstun::Address decoded;
    std::string error = "Previous error";
    bool okay = netstun::bindingResponse(bytes.data(), bytes.size(), transaction, decoded, error);
    if (!okay) std::fprintf(stderr, "Unexpected decode error: %s\n", error.c_str());
    CHECK(okay && error.empty());
    return decoded.host == host && decoded.port == port;
}
void rejects(const uint8_t* bytes, size_t size, const netstun::Transaction& tx = transaction)
{
    netstun::Address decoded;
    decoded.host.fill(0x5a); decoded.port = 12345;
    auto before = decoded;
    std::string error;
    CHECK(!netstun::bindingResponse(bytes, size, tx, decoded, error));
    CHECK(!error.empty());
    CHECK(decoded.host == before.host && decoded.port == before.port);
}
void rejects(const Bytes& bytes) { rejects(bytes.data(), bytes.size()); }

void referenceVectors()
{
    const Bytes golden{0,1,0,0,0x21,0x12,0xa4,0x42,0xb7,0xe7,0xa7,1,
                       0xbc,0x34,0xd6,0x86,0xfa,0x87,0xdf,0xae};
    CHECK(netstun::bindingRequest(transaction) == golden);
    CHECK(accepts(rfcIPv4));
    CHECK(accepts(rfcIPv6, ipv6));
    for (bool family : {false, true})
        for (bool xored : {false, true}) CHECK(accepts(minimal(family, xored), family ? ipv6 : ipv4));
    CHECK(netstun::isStun(rfcIPv4.data(), rfcIPv4.size()));
    CHECK(netstun::isStun(rfcIPv4.data(), 8));
    CHECK(!netstun::isStun(rfcIPv4.data(), 7));
    CHECK(!netstun::isStun(nullptr, 100));
    const uint8_t enet[]{0x80,1,0,0,0x21,0x12,0xa4,0x42};
    CHECK(!netstun::isStun(enet, sizeof(enet)));
}
void malformed()
{
    rejects(nullptr, 0); rejects(nullptr, 32);
    for (const auto& response : {rfcIPv4, rfcIPv6, minimal(), minimal(true)})
        for (size_t size = 0; size < response.size(); ++size) rejects(response.data(), size);
    for (size_t offset = 0; offset < 20; ++offset)
    {
        auto bytes = minimal(); bytes[offset] ^= 0x40; rejects(bytes);
    }
    auto bytes = minimal(); bytes.push_back(0); rejects(bytes);
    auto tx = transaction; tx[11] ^= 1; rejects(rfcIPv4.data(), rfcIPv4.size(), tx);
    rejects(netstun::bindingRequest(transaction));
    bytes = minimal(); bytes[0] = 0x01; bytes[1] = 0x11; rejects(bytes); // Error response.
    bytes = minimal(); bytes[1] = 0x02; rejects(bytes); // Wrong method.
    bytes = minimal(); bytes[3] |= 1; rejects(bytes); // Nonaligned framing.
    rejects(header()); // No address.
    bytes = minimal(); bytes[22] = bytes[23] = 0xff; rejects(bytes);
    bytes = minimal(); bytes[24] = 1; rejects(bytes); // Reserved address byte.
    for (uint8_t family : {uint8_t(0), uint8_t(2), uint8_t(3), uint8_t(255)})
    { bytes = minimal(); bytes[25] = family; rejects(bytes); }
    bytes = minimal(true); bytes[25] = 1; rejects(bytes);
    bytes = minimal(); bytes[26] = 0x21; bytes[27] = 0x12; rejects(bytes); // XOR zero port.
    bytes = minimal(false, false); bytes[26] = bytes[27] = 0; rejects(bytes);
    bytes = minimal(); attribute(bytes, 0x20, {0,1,0xa1,0x47,0xe1,0x12,0xa6,0x43}); rejects(bytes);
    bytes = minimal(false, false); attribute(bytes, 1, {0,1,0x80,0x55,192,0,2,1}); rejects(bytes);
    bytes = minimal(true); attribute(bytes, 1, {0,1,0x80,0x55,192,0,2,1}); rejects(bytes);
    bytes = minimal();
    auto v6 = minimal(true, false); bytes.insert(bytes.end(), v6.begin() + 20, v6.end()); length(bytes); rejects(bytes);
    bytes = minimal(); attribute(bytes, 0x1234, {}); rejects(bytes); // Unknown required attribute.
    bytes = minimal(); attribute(bytes, 0xaabb, {}); CHECK(accepts(bytes)); // Unknown optional attribute.
    attribute(bytes, 0xaabb, {}); rejects(bytes);
    for (size_t size : {0u, 1u, 19u, 21u})
    { bytes = minimal(); attribute(bytes, 8, Bytes(size)); rejects(bytes); }
    for (size_t size : {0u, 15u, 17u, 33u})
    { bytes = minimal(); attribute(bytes, 0x1c, Bytes(size)); rejects(bytes); }
    for (size_t size : {16u, 20u, 24u, 28u, 32u})
    { bytes = minimal(); attribute(bytes, 0x1c, Bytes(size)); CHECK(accepts(bytes)); }
    bytes = minimal(); attribute(bytes, 0x8022, {1}); CHECK(accepts(bytes));
    bytes[bytes.size() - 5] = 5; rejects(bytes); // Attribute claims absent padding/value.
    bytes = rfcIPv4; bytes.back() ^= 1; rejects(bytes); // Wrong fingerprint.
    bytes = rfcIPv4; bytes[bytes.size() - 5] = 3; rejects(bytes); // Wrong fingerprint length.
    bytes = rfcIPv4; attribute(bytes, 0x8022, {}); rejects(bytes); // Fingerprint must be last.
    bytes = minimal(); attribute(bytes, 1, {0,1,0x80,0x56,192,0,2,9});
    CHECK(accepts(bytes)); // XOR wins over an altered legacy mapped address.
    auto alternative = header(); attribute(alternative, 1, {0,1,0x80,0x56,192,0,2,9});
    auto xorAddress = minimal(); alternative.insert(alternative.end(), xorAddress.begin() + 20, xorAddress.end());
    length(alternative); CHECK(accepts(alternative)); // Precedence is order independent.
}
void boundedMutations()
{
    // Each modified datagram must either decode safely or preserve the output.
    uint32_t random = 0x416813a5;
    for (unsigned trial = 0; trial < 10000; ++trial)
    {
        random ^= random << 13; random ^= random >> 17; random ^= random << 5;
        auto bytes = trial & 1 ? minimal(true) : rfcIPv4;
        bytes[random % bytes.size()] ^= uint8_t(1 + (random >> 24));
        if (trial % 7 == 0) bytes.resize(random % bytes.size());
        netstun::Address result; result.host.fill(0xcd); result.port = 400;
        const auto before = result;
        std::string error;
        const bool okay = netstun::bindingResponse(bytes.data(), bytes.size(), transaction, result, error);
        CHECK(okay ? error.empty() && result.port != 0 :
              !error.empty() && result.host == before.host && result.port == before.port);
    }
    // Maximum legal payload length and large TLV lengths exercise size bounds.
    auto bytes = minimal(); attribute(bytes, 0xaaaa, Bytes(65516)); CHECK(accepts(bytes));
    bytes[34] = bytes[35] = 0xff; rejects(bytes);
}
} // namespace
int main()
{
    referenceVectors(); malformed(); boundedMutations();
    std::printf("PASS: %u STUN request/response codec checks\n", checks);
}
