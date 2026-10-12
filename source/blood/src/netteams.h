// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace netteams
{
constexpr size_t MaxPlayers = 8;

struct Member
{
    bool active = false;
    unsigned team = 0;
    uint64_t joinOrder = 0;
};

using Roster = std::array<Member, MaxPlayers>;

// The server supplies a monotonically increasing join order, preserved across
// deaths and map changes. A disconnected or departing player is inactive.
// Return the newest player on the larger team, or -1 if no move is needed.
inline int selectBalancePlayer(const Roster& roster)
{
    unsigned count[2] = {};
    for (const auto& member : roster)
        if (member.active && member.team < 2)
            ++count[member.team];

    unsigned larger;
    if (count[0] >= count[1] + 2)
        larger = 0;
    else if (count[1] >= count[0] + 2)
        larger = 1;
    else
        return -1;

    int selected = -1;
    for (size_t slot = 0; slot < roster.size(); ++slot)
    {
        const auto& member = roster[slot];
        if (member.active && member.team == larger &&
            (selected < 0 || member.joinOrder > roster[selected].joinOrder))
            selected = static_cast<int>(slot);
    }
    // Equal join orders use the lowest slot, making the result deterministic.
    return selected;
}
}
