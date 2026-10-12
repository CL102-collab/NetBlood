// SPDX-License-Identifier: GPL-2.0-only
#include "netcountry.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

namespace expected
{
#include "netcountry-data.inc"
}

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)

static uint16_t code(const char *s)
{
    return (uint16_t(s[0]) << 8) | uint16_t(s[1]);
}

// Explicit address intervals exercise the implementation's prefix masking
// independently, especially /10, /12 and /15 range endpoints.
static const uint32_t reserved[][2] = {
    {0x00000000u, 0x00ffffffu}, {0x0a000000u, 0x0affffffu},
    {0x64400000u, 0x647fffffu}, {0x7f000000u, 0x7fffffffu},
    {0xa9fe0000u, 0xa9feffffu}, {0xac100000u, 0xac1fffffu},
    {0xc0000000u, 0xc00000ffu}, {0xc0000200u, 0xc00002ffu},
    {0xc0586300u, 0xc05863ffu}, {0xc0a80000u, 0xc0a8ffffu},
    {0xc6120000u, 0xc613ffffu}, {0xc6336400u, 0xc63364ffu},
    {0xcb007100u, 0xcb0071ffu}, {0xe0000000u, 0xffffffffu},
};

static bool isReserved(uint32_t address)
{
    for (const auto& range : reserved)
        if (address >= range[0] && address <= range[1])
            return true;
    return false;
}

static uint16_t lookup6(uint64_t high, uint64_t low = 0)
{
    uint8_t bytes[16];
    for (unsigned i = 0; i < 8; ++i)
    {
        bytes[i] = uint8_t(high >> (56 - i * 8));
        bytes[i + 8] = uint8_t(low >> (56 - i * 8));
    }
    return netCountryLookupIPv6(bytes);
}

static bool isReserved6(uint64_t high)
{
    return high < 0x2000000000000000ULL || high > 0x3fffffffffffffffULL ||
        (high >= 0x2001000000000000ULL && high <= 0x200101ffffffffffULL) ||
        (high >= 0x20010db800000000ULL && high <= 0x20010db8ffffffffULL) ||
        (high >= 0x3fff000000000000ULL && high <= 0x3fff0fffffffffffULL) ||
        (high >= 0x2002000000000000ULL && high <= 0x2002ffffffffffffULL);
}

static void ipv6()
{
    CHECK(netCountryLookupIPv6(nullptr) == 0);
    CHECK(lookup6(0, 0) == 0);
    CHECK(lookup6(0, 1) == 0);
    CHECK(lookup6(0, 0x0000ffff08080808ULL) == code("US"));
    CHECK(lookup6(0, 0x0000ffff01010101ULL) == code("AU"));
    CHECK(lookup6(0, 0x0000ffffc0000201ULL) == 0);
    CHECK(lookup6(0, 0x0000000008080808ULL) == 0); // Not an IPv4-mapped address.
    for (const auto& range : reserved)
    {
        CHECK(lookup6(0, 0xffff00000000ULL | range[0]) == 0);
        CHECK(lookup6(0, 0xffff00000000ULL | range[1]) == 0);
    }
    CHECK(lookup6(0x2001486048600000ULL, 0x8888) == code("US"));
    CHECK(lookup6(0x200107fa00000001ULL) == code("HK"));
    CHECK(lookup6(0x200107fa00000002ULL) == code("KR"));
    CHECK(lookup6(0x200107fa00000003ULL) == code("JP"));
    for (uint64_t high : {0xfc00000000000000ULL, 0xfdffffffffffffffULL,
                         0xfe80000000000000ULL, 0xfebfffffffffffffULL,
                         0xff00000000000000ULL, 0xffffffffffffffffULL,
                         0x2001000200000000ULL, 0x2001001000000000ULL,
                         0x2001002000000000ULL, 0x20010db800000000ULL,
                         0x3fff000000000000ULL, 0x3fff0fffffffffffULL})
        CHECK(lookup6(high, 0xffffffffffffffffULL) == 0);

    size_t const count = sizeof(expected::kCountryIPv6Starts) / sizeof(expected::kCountryIPv6Starts[0]);
    uint64_t previousEnd = 0;
    for (size_t i = 0; i < count; ++i)
    {
        unsigned const length = expected::kCountryIPv6PrefixLengths[i];
        CHECK(length >= 1 && length <= 64);
        uint64_t const start = expected::kCountryIPv6Starts[i];
        uint64_t const end = start | ((uint64_t(1) << (64 - length)) - 1);
        CHECK(start > previousEnd);
        uint16_t const country = expected::kCountryCodes[expected::kCountryIPv6Indices[i]];
        for (uint64_t high : {start, end, start + (end - start) / 2})
        {
            CHECK(lookup6(high, 0) == (isReserved6(high) ? 0 : country));
            CHECK(lookup6(high, 0xffffffffffffffffULL) == (isReserved6(high) ? 0 : country));
        }
        if (start - previousEnd > 1)
        {
            CHECK(lookup6(previousEnd + 1) == 0);
            CHECK(lookup6(start - 1, 0xffffffffffffffffULL) == 0);
            CHECK(lookup6(previousEnd + (start - previousEnd) / 2) == 0);
        }
        previousEnd = end;
    }
    CHECK(lookup6(previousEnd + 1) == 0);
}

int main()
{
    ipv6();
    CHECK(netCountryLookupIPv4(0x08080808u) == code("US"));
    CHECK(netCountryLookupIPv4(0x01010101u) == code("AU"));
    CHECK(netCountryLookupIPv4(0xc100068bu) == code("NL"));
    CHECK(netCountryLookupIPv4(0xc8a00203u) == code("BR"));
    CHECK(netCountryLookupIPv4(0xca0c1b21u) == code("JP"));
    CHECK(netCountryLookupIPv4(0xc4d80201u) == code("MU"));

    for (const auto& range : reserved)
    {
        CHECK(netCountryLookupIPv4(range[0]) == 0);
        CHECK(netCountryLookupIPv4(range[1]) == 0);
        CHECK(netCountryLookupIPv4(range[0] + (range[1] - range[0]) / 2) == 0);
    }

    // Every run's first/last/middle address catches binary-search equality,
    // insertion-point and unknown-gap errors across the complete database.
    size_t const count = sizeof(expected::kCountryStarts) / sizeof(expected::kCountryStarts[0]);
    size_t const countries = sizeof(expected::kCountryCodes) / sizeof(expected::kCountryCodes[0]);
    CHECK(expected::kCountryStarts[0] == 0);
    for (size_t i = 0; i < count; ++i)
    {
        uint32_t const start = expected::kCountryStarts[i];
        uint32_t const end = i + 1 < count ? expected::kCountryStarts[i + 1] - 1 : 0xffffffffu;
        CHECK(start <= end);
        CHECK(expected::kCountryIndices[i] < countries);
        uint16_t const country = expected::kCountryCodes[expected::kCountryIndices[i]];
        for (uint32_t address : {start, end, start + (end - start) / 2})
            CHECK(netCountryLookupIPv4(address) == (isReserved(address) ? 0 : country));
    }

    const auto *unknown = netCountryFlag(uint16_t(0));
    CHECK(netCountryFlag("") == unknown);
    CHECK(netCountryFlag(nullptr) == unknown);
    CHECK(netCountryFlag("U") == unknown);
    CHECK(netCountryFlag("USA") == unknown);
    CHECK(netCountryFlag("us") == unknown);
    CHECK(netCountryFlag("ZZ") == unknown);
    CHECK(netCountryFlag(uint16_t(0xffff)) == unknown);
    for (size_t i = 1; i < countries; ++i)
    {
        uint16_t const packed = expected::kCountryCodes[i];
        CHECK(packed > expected::kCountryCodes[i - 1]);
        char text[] = {char(packed >> 8), char(packed & 255), 0};
        const auto *flag = netCountryFlag(packed);
        CHECK(flag != unknown);
        CHECK(flag == netCountryFlag(text));
        // Reading every bitmap byte also catches atlas-stride mistakes with ASan.
        unsigned brightness = 0;
        for (unsigned p = 0; p < kNetCountryFlagWidth * kNetCountryFlagHeight * 3; ++p)
            brightness += flag[p];
        CHECK(brightness > 0);
    }
    std::printf("Country lookup and flags: %u checks passed.\n", checks);
}
