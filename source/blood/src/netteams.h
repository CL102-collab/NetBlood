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

// Use a private PRNG so a vote never advances the deterministic game RNG.
// Inactive slots retain -1; every active slot receives a valid, balanced team.
template<size_t Capacity>
inline std::array<int, Capacity> shuffleTeams(const std::array<Member, Capacity>& roster, uint32_t seed)
{
    std::array<int, Capacity> teams;
    teams.fill(-1);
    std::array<size_t, Capacity> slots{};
    size_t count = 0;
    for (size_t slot = 0; slot < roster.size(); ++slot)
        if (roster[slot].active)
            slots[count++] = slot;

    // A full-period counter followed by an invertible 32-bit mixer also
    // handles seed zero without falling into a zero-state PRNG cycle.
    auto random = [&seed]() -> uint32_t {
        uint32_t value = (seed += UINT32_C(0x9e3779b9));
        value = (value ^ (value >> 16)) * UINT32_C(0x85ebca6b);
        value = (value ^ (value >> 13)) * UINT32_C(0xc2b2ae35);
        return value ^ (value >> 16);
    };
    auto bounded = [&random](uint32_t bound) -> uint32_t {
        const uint32_t threshold = uint32_t(0 - bound) % bound;
        uint32_t value;
        do value = random(); while (value < threshold);
        return value % bound;
    };
    for (size_t remaining = count; remaining > 1; --remaining)
    {
        const size_t selected = bounded(static_cast<uint32_t>(remaining));
        const size_t temporary = slots[remaining - 1];
        slots[remaining - 1] = slots[selected];
        slots[selected] = temporary;
    }
    const size_t zeroCount = count / 2 + (count % 2 ? bounded(2) : 0);
    bool unchanged = true, reversed = true;
    for (size_t index = 0; index < count; ++index)
    {
        const size_t slot = slots[index];
        teams[slot] = index < zeroCount ? 0 : 1;
        unchanged = unchanged && roster[slot].team == unsigned(teams[slot]);
        reversed = reversed && roster[slot].team == unsigned(1 - teams[slot]);
    }
    // A bounded correction prevents a successful vote doing nothing. With
    // three or more players, swapping colors alone is not a teammate shuffle:
    // exchange one player from each new team to change the actual groups.
    // With two players, changing colors is the only balanced change possible.
    if (count >= 2 && (unchanged || (count >= 3 && reversed)))
    {
        teams[slots[0]] = 1;
        teams[slots[zeroCount]] = 0;
    }
    return teams;
}

// The server supplies a monotonically increasing join order, preserved across
// deaths and map changes. A disconnected or departing player is inactive.
// Return the newest player on the larger team, or -1 if no move is needed.
template<size_t Capacity>
inline int selectBalancePlayer(const std::array<Member, Capacity>& roster)
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
