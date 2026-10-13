// SPDX-License-Identifier: GPL-2.0-or-later
#include "netteams.h"
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <set>

static unsigned checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)

static unsigned teamCount(const netteams::Roster& roster, unsigned team)
{
    unsigned count = 0;
    for (const auto& member : roster)
        count += member.active && member.team == team;
    return count;
}

static unsigned difference(const netteams::Roster& roster)
{
    unsigned a = teamCount(roster, 0), b = teamCount(roster, 1);
    return a > b ? a - b : b - a;
}

static void verifySelection(const netteams::Roster& roster)
{
    const auto before = roster;
    int selected = netteams::selectBalancePlayer(roster);
    CHECK((selected < 0) == (difference(roster) < 2));
    if (selected >= 0)
    {
        CHECK(static_cast<size_t>(selected) < roster.size());
        const auto& member = roster[selected];
        CHECK(member.active);
        CHECK(member.team < 2);
        CHECK(teamCount(roster, member.team) >= teamCount(roster, 1 - member.team) + 2);
        for (size_t slot = 0; slot < roster.size(); ++slot)
        {
            const auto& other = roster[slot];
            if (!other.active || other.team != member.team)
                continue;
            CHECK(member.joinOrder >= other.joinOrder);
            if (member.joinOrder == other.joinOrder)
                CHECK(static_cast<size_t>(selected) <= slot);
        }
    }
    for (size_t slot = 0; slot < roster.size(); ++slot)
    {
        CHECK(roster[slot].active == before[slot].active);
        CHECK(roster[slot].team == before[slot].team);
        CHECK(roster[slot].joinOrder == before[slot].joinOrder);
    }
}

static void exhaustiveRosters()
{
    // All absent/team-zero/team-one assignments cover every population from
    // zero through eight, including holes after disconnects. Exercise forward,
    // reversed, tied and non-slot-ordered 64-bit join chronology for each one.
    unsigned permutations = 1;
    for (size_t slot = 0; slot < netteams::MaxPlayers; ++slot)
        permutations *= 3;
    for (unsigned pattern = 0; pattern < permutations; ++pattern)
    {
        for (unsigned chronology = 0; chronology < 4; ++chronology)
        {
            netteams::Roster roster{};
            unsigned assignment = pattern;
            for (size_t slot = 0; slot < roster.size(); ++slot)
            {
                unsigned state = assignment % 3;
                assignment /= 3;
                auto& member = roster[slot];
                member.active = state != 0;
                member.team = state ? state - 1 : 0;
                member.joinOrder = chronology == 0 ? slot + 1 :
                    chronology == 1 ? roster.size() - slot :
                    chronology == 2 ? 123 :
                    (uint64_t(1) << 40) + (slot * 5) % roster.size();
            }
            verifySelection(roster);
            unsigned originalDifference = difference(roster), moves = 0;
            for (;;)
            {
                int selected = netteams::selectBalancePlayer(roster);
                if (selected < 0)
                    break;
                unsigned previous = difference(roster);
                roster[selected].team = 1 - roster[selected].team;
                CHECK(difference(roster) + 2 == previous);
                CHECK(++moves <= netteams::MaxPlayers / 2);
                verifySelection(roster);
            }
            CHECK(difference(roster) <= 1);
            CHECK(moves == originalDifference / 2);
        }
    }
}

static void connectionChronology()
{
    netteams::Roster roster{};
    uint64_t order = 0;
    auto join = [&](size_t slot, unsigned team) {
        roster[slot].active = true;
        roster[slot].team = team;
        roster[slot].joinOrder = ++order;
    };

    CHECK(netteams::selectBalancePlayer(roster) == -1);
    join(7, 0);
    CHECK(netteams::selectBalancePlayer(roster) == -1); // One player is valid.
    join(2, 0);
    CHECK(netteams::selectBalancePlayer(roster) == 2); // Newest, not last slot.
    roster[2].team = 1;
    join(5, 0);
    join(0, 0);
    join(3, 1); // Newest overall belongs to the smaller team.
    CHECK(netteams::selectBalancePlayer(roster) == -1); // 3 versus 2.
    roster[2].active = false; // A departure creates a 3-versus-1 imbalance.
    CHECK(netteams::selectBalancePlayer(roster) == 0);
    roster[0].team = 1;
    CHECK(netteams::selectBalancePlayer(roster) == -1);

    // Reusing a vacated low slot must get a new join order; the previous
    // occupant's age or a team's newest slot number must not drive selection.
    join(2, 0);
    join(1, 0);
    CHECK(netteams::selectBalancePlayer(roster) == 1);
    roster[1].active = false; // Pending departures are excluded immediately.
    CHECK(netteams::selectBalancePlayer(roster) == -1);
    roster[0].active = false;
    CHECK(netteams::selectBalancePlayer(roster) == 2);

    // Lifetime order is 64-bit: selection must not truncate it to a 32-bit
    // game tick or signed integer when a server has a long connection history.
    roster = {};
    join(0, 1);
    join(1, 1);
    roster[0].joinOrder = std::numeric_limits<uint32_t>::max();
    roster[1].joinOrder = uint64_t(1) << 32;
    CHECK(netteams::selectBalancePlayer(roster) == 1);
    roster[0].joinOrder = std::numeric_limits<uint64_t>::max();
    CHECK(netteams::selectBalancePlayer(roster) == 0);
    roster[1].joinOrder = roster[0].joinOrder;
    CHECK(netteams::selectBalancePlayer(roster) == 0); // Stable tie break.

    roster[2].active = true;
    roster[2].team = 2;
    roster[2].joinOrder = std::numeric_limits<uint64_t>::max();
    CHECK(netteams::selectBalancePlayer(roster) == 0); // Invalid teams ignored.
    roster[0].active = false;
    CHECK(netteams::selectBalancePlayer(roster) == -1);
}

static void verifyShuffle(const netteams::Roster& roster, uint32_t seed)
{
    const auto before = roster;
    const auto teams = netteams::shuffleTeams(roster, seed);
    CHECK(teams == netteams::shuffleTeams(roster, seed));
    unsigned counts[2] = {}, active = 0;
    bool changed = false, sameGroups = true, validOldTeams = true;
    for (size_t slot = 0; slot < roster.size(); ++slot)
    {
        CHECK(roster[slot].active == before[slot].active);
        CHECK(roster[slot].team == before[slot].team);
        CHECK(roster[slot].joinOrder == before[slot].joinOrder);
        if (!roster[slot].active)
        {
            CHECK(teams[slot] == -1);
            continue;
        }
        ++active;
        CHECK(teams[slot] == 0 || teams[slot] == 1);
        ++counts[teams[slot]];
        changed = changed || unsigned(teams[slot]) != roster[slot].team;
        validOldTeams = validOldTeams && roster[slot].team < 2;
        for (size_t other = 0; other < slot; ++other)
            if (roster[other].active &&
                ((roster[slot].team == roster[other].team) != (teams[slot] == teams[other])))
                sameGroups = false;
    }
    CHECK(counts[0] <= counts[1] + 1 && counts[1] <= counts[0] + 1);
    CHECK(counts[0] + counts[1] == active);
    if (active >= 2)
        CHECK(changed);
    if (active >= 3 && validOldTeams)
        CHECK(!sameGroups);
}

static void exhaustiveShuffles()
{
    // Every absent/team-zero/team-one roster includes empty games, all eight
    // population sizes, unbalanced teams and disconnected holes in the roster.
    unsigned permutations = 1;
    for (size_t slot = 0; slot < netteams::MaxPlayers; ++slot)
        permutations *= 3;
    const uint32_t seeds[] = {0, 1, UINT32_C(0x12345678), UINT32_MAX};
    for (unsigned pattern = 0; pattern < permutations; ++pattern)
    {
        netteams::Roster roster{};
        unsigned assignment = pattern;
        for (size_t slot = 0; slot < roster.size(); ++slot)
        {
            const unsigned state = assignment % 3;
            assignment /= 3;
            roster[slot].active = state != 0;
            roster[slot].team = state ? state - 1 : 0;
            roster[slot].joinOrder = (uint64_t(1) << 40) + slot;
        }
        for (uint32_t seed : seeds)
            verifyShuffle(roster, seed);
    }

    // Old team values cannot leak into an assignment, even if an active slot
    // has invalid or uninitialized team metadata when the vote completes.
    netteams::Roster roster{};
    for (size_t slot = 0; slot < roster.size(); ++slot)
    {
        roster[slot].active = slot % 3 != 1;
        roster[slot].team = slot % 2 ? UINT32_MAX : 2;
    }
    for (uint32_t seed : seeds)
        verifyShuffle(roster, seed);

    // A departure immediately before applying a passed vote is not assigned
    // to either team; a reused slot participates independently of join order.
    roster[2].active = false;
    verifyShuffle(roster, 31);
    roster[2].active = true;
    roster[2].joinOrder = std::numeric_limits<uint64_t>::max();
    verifyShuffle(roster, 31);
}

static void shuffleVariety()
{
    for (size_t count = 1; count <= netteams::MaxPlayers; ++count)
    {
        netteams::Roster roster{};
        for (size_t index = 0; index < count; ++index)
        {
            // Permuted slots ensure variety is not tied to a compact roster.
            const size_t slot = (index * 3) % roster.size();
            roster[slot].active = true;
            roster[slot].team = index % 2;
            roster[slot].joinOrder = count - index;
        }
        std::set<std::array<int, netteams::MaxPlayers>> outcomes;
        bool extraZero = false, extraOne = false;
        for (uint32_t seed = 0; seed < 256; ++seed)
        {
            verifyShuffle(roster, seed);
            const auto teams = netteams::shuffleTeams(roster, seed);
            outcomes.insert(teams);
            unsigned zero = 0, one = 0;
            for (int team : teams)
            {
                zero += team == 0;
                one += team == 1;
            }
            extraZero = extraZero || zero > one;
            extraOne = extraOne || one > zero;
        }
        // Two players have just one changed, balanced assignment. Larger
        // rosters must produce multiple outcomes instead of a fixed rotation.
        CHECK(outcomes.size() >= (count == 2 ? 1 : count == 1 ? 2 : count));
        if (count % 2)
            CHECK(extraZero && extraOne);
    }
}

int main()
{
    exhaustiveRosters();
    connectionChronology();
    exhaustiveShuffles();
    shuffleVariety();
    std::printf("Team balancing and shuffling passed: %u checks\n", checks);
    return 0;
}
