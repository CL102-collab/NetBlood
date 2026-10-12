// SPDX-License-Identifier: GPL-2.0-only
#include "netcountry.h"

#include <algorithm>
#include <cstddef>

namespace
{
#include "netcountry-data.inc"

bool privateOrReserved(uint32_t address)
{
    // LAN, shared address space, loopback, link-local, documentation,
    // benchmarking, protocol assignments, multicast and future-use space.
    return (address >> 24) == 0 || (address >> 24) == 10 ||
        (address >> 22) == (0x64400000u >> 22) || (address >> 24) == 127 ||
        (address >> 16) == 0xa9fe || (address >> 20) == 0xac1 ||
        (address >> 8) == 0xc00000 || (address >> 8) == 0xc00002 ||
        (address >> 8) == 0xc05863 || (address >> 16) == 0xc0a8 ||
        (address >> 17) == (0xc6120000u >> 17) || (address >> 8) == 0xc63364 ||
        (address >> 8) == 0xcb0071 || address >= 0xe0000000u;
}
}

uint16_t netCountryLookupIPv4(uint32_t hostOrderAddress)
{
    if (privateOrReserved(hostOrderAddress))
        return 0;
    auto const *end = kCountryStarts + sizeof(kCountryStarts) / sizeof(kCountryStarts[0]);
    auto const *next = std::upper_bound(kCountryStarts, end, hostOrderAddress);
    if (next == kCountryStarts)
        return 0;
    return kCountryCodes[kCountryIndices[next - kCountryStarts - 1]];
}

uint16_t netCountryLookupIPv6(const uint8_t address[16])
{
    if (!address)
        return 0;
    bool mapped = address[10] == 0xff && address[11] == 0xff;
    for (unsigned i = 0; i < 10 && mapped; ++i)
        mapped = address[i] == 0;
    if (mapped)
        return netCountryLookupIPv4((uint32_t(address[12]) << 24) |
            (uint32_t(address[13]) << 16) | (uint32_t(address[14]) << 8) | address[15]);

    // Only assigned global unicast space. This also excludes unspecified,
    // loopback, IPv4-compatible, translated, ULA, link-local and multicast.
    if ((address[0] & 0xe0) != 0x20)
        return 0;
    uint64_t high = 0;
    for (unsigned i = 0; i < 8; ++i)
        high = (high << 8) | address[i];
    // IETF special-purpose 2001::/23 (including benchmarking and ORCHID),
    // documentation 2001:db8::/32 and 3fff::/20, and deprecated 6to4 space.
    if ((high >> 41) == (0x2001000000000000ULL >> 41) ||
        (high >> 32) == 0x20010db8ULL || (high >> 44) == 0x3fff0ULL ||
        (high >> 48) == 0x2002ULL)
        return 0;

    // Upstream delegation prefixes are <=64 bits; the generator validates
    // this, so the interface-ID half cannot change an allocation's country.
    auto const *end = kCountryIPv6Starts + sizeof(kCountryIPv6Starts) / sizeof(kCountryIPv6Starts[0]);
    auto const *next = std::upper_bound(kCountryIPv6Starts, end, high);
    if (next == kCountryIPv6Starts)
        return 0;
    size_t const index = next - kCountryIPv6Starts - 1;
    unsigned const suffix = 64 - kCountryIPv6PrefixLengths[index];
    if ((high >> suffix) != (kCountryIPv6Starts[index] >> suffix))
        return 0;
    return kCountryCodes[kCountryIPv6Indices[index]];
}

const uint8_t *netCountryFlag(uint16_t country)
{
    auto const *end = kCountryCodes + sizeof(kCountryCodes) / sizeof(kCountryCodes[0]);
    auto const *entry = std::lower_bound(kCountryCodes, end, country);
    size_t const index = entry != end && *entry == country ? entry - kCountryCodes : 0;
    return kCountryFlags[index];
}

const uint8_t *netCountryFlag(const char *country)
{
    if (!country || !country[0] || !country[1] || country[2] ||
        country[0] < 'A' || country[0] > 'Z' || country[1] < 'A' || country[1] > 'Z')
        return netCountryFlag(uint16_t(0));
    return netCountryFlag(uint16_t((uint16_t(country[0]) << 8) | uint16_t(country[1])));
}
