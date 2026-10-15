// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstdio>
#include <string>
#include "../../blood/src/netvote.h"
#include "../../blood/src/netteams.h"

namespace rnet { namespace social {
// Strip console colors and controls before measuring a name or displaying any
// untrusted string. Color-only and whitespace-only names are empty names.
inline std::string cleanText(const std::string& text, size_t limit)
{
    std::string out;
    for (size_t i = 0; i < text.size() && out.size() < limit; ++i)
    {
        unsigned char c = text[i];
        if (c == '^' && i + 1 < text.size() && text[i + 1] >= '0' && text[i + 1] <= '9')
        {
            ++i;
            if (i + 1 < text.size() && text[i + 1] >= '0' && text[i + 1] <= '9') ++i;
            continue;
        }
        if (c >= 32 && c < 127) out += char(c);
    }
    const size_t begin = out.find_first_not_of(' ');
    if (begin == std::string::npos) return {};
    return out.substr(begin, out.find_last_not_of(' ') - begin + 1);
}

inline bool sameName(std::string a, std::string b)
{
    auto fold = [](char& c) { if (c >= 'a' && c <= 'z') c -= 'a' - 'A'; };
    for (char& c : a) fold(c);
    for (char& c : b) fold(c);
    return a == b;
}

template<size_t Capacity>
std::string playerName(const std::string& requested, size_t slot,
                       const std::array<std::string, Capacity>& roster)
{
    std::string result = cleanText(requested, 31);
    if (!result.empty()) return result;
    for (unsigned attempt = 0; attempt < 100; ++attempt)
    {
        char candidate[8];
        std::snprintf(candidate, sizeof(candidate), "Duke%02u", attempt);
        bool taken = false;
        for (size_t i = 0; i < Capacity; ++i)
            if (i != slot && sameName(cleanText(roster[i], 31), candidate)) taken = true;
        if (!taken) return candidate;
    }
    // The engine has only 16 slots, so this cannot exhaust the 100 names.
    return "Duke";
}
} }
