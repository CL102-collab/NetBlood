// SPDX-License-Identifier: GPL-2.0-or-later
#include "rnet_social_model.h"
#include <cassert>
#include <iostream>

int main()
{
    using namespace rnet::social;
    std::array<std::string, 16> names{};
    for (unsigned slot = 0; slot < names.size(); ++slot)
    {
        names[slot] = playerName(" ^12 \t\r\n", slot, names);
        assert(names[slot].size() == 6 && names[slot].substr(0, 4) == "Duke");
        for (unsigned other = 0; other < slot; ++other) assert(names[other] != names[slot]);
    }
    names[1] = "duke00";
    assert(playerName("", 0, names) == "Duke01");
    names[1] = "^12DUKE00";
    assert(playerName("", 0, names) == "Duke01");
    assert(playerName("", 0, std::array<std::string,16>{}) == "Duke00");
    assert(playerName("", 15, std::array<std::string,16>{}) == "Duke00");
    std::array<std::string,16> reserved{};
    reserved[2] = "Duke00"; reserved[5] = "DUKE01";
    assert(playerName("",15,reserved) == "Duke02");
    reserved[2].clear();
    assert(playerName("",15,reserved) == "Duke00");
    assert(playerName("  Duke Nukem  ", 4, names) == "Duke Nukem");
    assert(playerName("^12Alice\n", 4, names) == "Alice");
    assert(playerName(std::string(1000, 'A'), 4, names).size() == 31);
    assert(cleanText(std::string("X\0Y",3),120) == "XY");
    assert(cleanText("^1^23^4",31).empty());
    assert(cleanText(std::string(1000,'x'),120).size() == 120);

    std::array<uint32_t, 16> roster{};
    for (unsigned i = 0; i < roster.size(); ++i) roster[i] = i + 1;
    netvote::BasicVote<16> vote;
    assert(vote.start(100,roster,15,16));
    assert(vote.eligibleCount() == 16 && vote.requiredCount() == 9);
    assert(!vote.cast(200,15,16,true)); // Caller has already voted.
    assert(!vote.cast(200,8,99,true)); // Slot reuse cannot inherit a ballot.
    for (unsigned i = 0; i < 7; ++i) assert(vote.cast(200,i,roster[i],true));
    assert(vote.poll(30099) == netvote::Result::NoResult);
    assert(vote.poll(30100) == netvote::Result::Failed); // 8/16 is below 51%.
    assert(vote.start(UINT32_MAX-100,roster,15,16));
    for (unsigned i = 0; i < 8; ++i) assert(vote.cast(500,i,roster[i],true));
    assert(vote.poll(29899) == netvote::Result::Passed); // Monotonic-clock wrap.
    assert(vote.requiredCount() == 9);

    std::array<netteams::Member,16> members{};
    for (unsigned i = 0; i < members.size(); ++i)
    {
        members[i].active = true;
        members[i].team = 0;
        members[i].joinOrder = i + 1;
    }
    assert(netteams::selectBalancePlayer(members) == 15);
    for (unsigned i = 0; i < 8; ++i)
    {
        int selected = netteams::selectBalancePlayer(members);
        assert(selected == int(15-i));
        members[selected].team = 1;
    }
    assert(netteams::selectBalancePlayer(members) == -1);
    for (unsigned seed = 0; seed < 1000; ++seed)
    {
        auto result = netteams::shuffleTeams(members,seed);
        unsigned count[2] = {};
        bool changed = false, inverted = true;
        for (unsigned i = 0; i < result.size(); ++i)
        {
            assert(result[i] >= 0 && result[i] <= 1);
            ++count[result[i]];
            changed |= unsigned(result[i]) != members[i].team;
            inverted &= unsigned(result[i]) != members[i].team;
        }
        assert(count[0] == 8 && count[1] == 8);
        assert(changed && !inverted);
    }
    members[15].active = false;
    auto result = netteams::shuffleTeams(members,123);
    assert(result[15] == -1);
    std::cout << "Rednukem social: sanitized unique Duke names, 16-player votes and balanced teams passed\n";
}
