// Offline network-country hints and scoreboard flag artwork.
// SPDX-License-Identifier: GPL-2.0-only
#ifndef BLOOD_NETCOUNTRY_H
#define BLOOD_NETCOUNTRY_H

#include <cstdint>

// Numeric host order, e.g. 0x08080808 for 8.8.8.8. The result is an ISO 3166
// alpha-2 code packed as (first ASCII letter << 8) | second ASCII letter.
// Private, reserved and unknown addresses return zero. Registration-country
// data is an estimate of the network location, not a player's nationality.
uint16_t netCountryLookupIPv4(uint32_t hostOrderAddress);

// Network-order IPv6 bytes, as used by ENetAddress.host.s6_addr. IPv4-mapped
// addresses (::ffff:a.b.c.d) use the IPv4 table. nullptr returns unknown.
uint16_t netCountryLookupIPv6(const uint8_t networkOrderAddress[16]);

enum { kNetCountryFlagWidth = 16, kNetCountryFlagHeight = 12 };

// Row-major RGB pixels; static lifetime. Invalid/unknown codes use a neutral
// question-mark flag. The string overload accepts uppercase ISO2 or "".
const uint8_t *netCountryFlag(uint16_t country);
const uint8_t *netCountryFlag(const char *country);

#endif
