// SPDX-License-Identifier: GPL-2.0-or-later
#include "netvote.h"
#include <cstdio>
#include <cstdlib>
#include <limits>

static unsigned checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)

static netvote::Roster roster(unsigned count)
{
    netvote::Roster result{};
    for (unsigned i = 0; i < count; ++i)
        result[i] = 100 + i;
    return result;
}

static void ballots()
{
    // Exhaust all yes/no/abstention combinations for one through eight players.
    // The caller's automatically recorded yes is fixed; other ballots vary.
    unsigned permutations = 1;
    for (unsigned players = 1; players <= netvote::MaxPlayers; ++players)
    {
        auto members = roster(players);
        for (unsigned pattern = 0; pattern < permutations; ++pattern)
        {
            netvote::Vote vote;
            CHECK(vote.start(100, members, 0, members[0]));
            CHECK(vote.eligibleCount() == players);
            CHECK(vote.requiredCount() == players / 2 + 1);
            CHECK(vote.hasVoted(0, members[0]));
            CHECK(!vote.cast(101, 0, members[0], true));
            CHECK(!vote.cast(101, 0, members[0], false));
            unsigned yes = 1, no = 0, combination = pattern;
            for (unsigned slot = 1; slot < players; ++slot)
            {
                unsigned ballot = combination % 3;
                combination /= 3;
                if (!ballot)
                {
                    CHECK(!vote.hasVoted(slot, members[slot]));
                    continue;
                }
                bool approval = ballot == 1;
                CHECK(vote.cast(101, slot, members[slot], approval));
                CHECK(vote.hasVoted(slot, members[slot]));
                CHECK(!vote.cast(102, slot, members[slot], approval));
                CHECK(!vote.cast(102, slot, members[slot], !approval));
                yes += approval;
                no += !approval;
            }
            CHECK(vote.yesCount() == yes);
            CHECK(vote.noCount() == no);
            // Even unanimous votes always remain open for the full 30 seconds.
            CHECK(vote.poll(30099) == netvote::Result::NoResult);
            CHECK(vote.active());
            CHECK(vote.remaining(30099) == 1);
            bool passes = (yes + no) * 100 >= players * 51 && yes * 100 >= players * 51;
            CHECK(vote.poll(30100) == (passes ? netvote::Result::Passed : netvote::Result::Failed));
            CHECK(!vote.active());
            CHECK(vote.remaining(30100) == 0);
            CHECK(vote.yesCount() == yes && vote.noCount() == no);
            CHECK(vote.eligibleCount() == players);
            CHECK(vote.poll(30101) == netvote::Result::NoResult);
            CHECK(!vote.cast(30101, 0, members[0], true));
        }
        permutations *= 3;
    }
}

static void membershipAndLifetime()
{
    netvote::Vote vote;
    auto members = roster(3);
    CHECK(!vote.active());
    CHECK(vote.remaining(0) == 0);
    CHECK(vote.poll(0) == netvote::Result::NoResult);
    CHECK(!vote.eligible(0, 0));
    CHECK(!vote.start(0, {}, 0, 100));
    CHECK(!vote.start(0, members, netvote::MaxPlayers, 100));
    CHECK(!vote.start(0, members, 0, 0));
    CHECK(!vote.start(0, members, 0, 101));
    CHECK(!vote.cast(0, 0, 100, true));
    CHECK(vote.start(0, members, 0, 100));
    CHECK(vote.remaining(0) == netvote::DurationMs);
    CHECK(!vote.start(1, members, 1, 101));
    CHECK(vote.yesCount() == 1 && vote.noCount() == 0);
    CHECK(vote.eligible(1, 101));
    CHECK(!vote.eligible(1, 999)); // Rejoin into the same slot gets a new token.
    CHECK(!vote.cast(100, 1, 999, true));
    CHECK(!vote.hasVoted(1, 999));
    CHECK(!vote.cast(100, 3, 103, true)); // A late join was not in the electorate.
    CHECK(!vote.cast(100, netvote::MaxPlayers, 108, true));
    CHECK(!vote.hasVoted(netvote::MaxPlayers, 108));
    CHECK(!vote.cast(100, 1, 0, true));
    members[1] = 999;
    CHECK(vote.eligible(1, 101)); // Caller mutation cannot change the frozen roster.
    CHECK(vote.cast(29999, 1, 101, true));
    CHECK(!vote.cast(30000, 2, 102, true));
    CHECK(vote.yesCount() == 2);
    CHECK(vote.poll(30000) == netvote::Result::Passed);
    CHECK(vote.start(30001, members, 1, 999));
    CHECK(vote.yesCount() == 1 && vote.noCount() == 0);
    CHECK(vote.hasVoted(1, 999));
    CHECK(!vote.hasVoted(0, 100));
    CHECK(vote.cast(30002, 2, 102, false));
    vote.cancel(true);
    CHECK(!vote.active() && vote.remaining(30003) == 0);
    CHECK(vote.eligibleCount() == 3 && vote.yesCount() == 1 && vote.noCount() == 1);
    CHECK(vote.requiredCount() == 2);
    CHECK(vote.eligible(1, 999) && vote.hasVoted(1, 999));
    CHECK(vote.eligible(2, 102) && vote.hasVoted(2, 102));
    CHECK(vote.eligible(0, 100) && !vote.hasVoted(0, 100));
    CHECK(!vote.cast(30003, 0, 100, true));
    CHECK(vote.poll(60001) == netvote::Result::NoResult);
    vote.cancel();
    CHECK(!vote.active() && vote.remaining(30002) == 0);
    CHECK(vote.eligibleCount() == 0 && vote.yesCount() == 0 && vote.noCount() == 0);
    CHECK(!vote.eligible(1, 999));
    CHECK(vote.poll(60001) == netvote::Result::NoResult);
    CHECK(vote.start(60002, members, 0, 100));
    vote.cancel();
    vote.cancel();
    CHECK(vote.poll(90002) == netvote::Result::NoResult);
}

static void cooldownAndClockWrap()
{
    netvote::Cooldown cooldown;
    CHECK(cooldown.ready(0) && cooldown.remaining(0) == 0);
    cooldown.accept(0);
    CHECK(!cooldown.ready(0) && cooldown.remaining(0) == 120000);
    CHECK(!cooldown.ready(119999) && cooldown.remaining(119999) == 1);
    CHECK(cooldown.ready(120000) && cooldown.remaining(120000) == 0);
    cooldown.accept(120000);
    CHECK(!cooldown.ready(239999) && cooldown.ready(240000));

    uint32_t start = std::numeric_limits<uint32_t>::max() - 10000;
    cooldown.accept(start);
    CHECK(!cooldown.ready(uint32_t(start + 119999)));
    CHECK(cooldown.remaining(uint32_t(start + 119999)) == 1);
    CHECK(cooldown.ready(uint32_t(start + 120000)));
    CHECK(cooldown.remaining(uint32_t(start + 120000)) == 0);

    netvote::Vote vote;
    auto members = roster(2);
    CHECK(vote.start(start, members, 0, 100));
    CHECK(vote.cast(uint32_t(start + 29999), 1, 101, true));
    CHECK(vote.remaining(uint32_t(start + 29999)) == 1);
    CHECK(vote.poll(uint32_t(start + 29999)) == netvote::Result::NoResult);
    CHECK(vote.poll(uint32_t(start + 30000)) == netvote::Result::Passed);
    CHECK(vote.poll(uint32_t(start + 30001)) == netvote::Result::NoResult);

    // Rejected requests never consume cooldown; map changes only cancel the
    // active vote and do not affect the externally owned per-player timer.
    netvote::Cooldown untouched;
    CHECK(vote.start(0, members, 0, 100));
    CHECK(!vote.start(1, members, 1, 101));
    CHECK(untouched.ready(1));
    cooldown.accept(0);
    vote.cancel();
    CHECK(!cooldown.ready(30000));
    CHECK(cooldown.remaining(30000) == 90000);
}

int main()
{
    ballots();
    membershipAndLifetime();
    cooldownAndClockWrap();
    std::printf("PASS: %u vote checks; 1-8 players, ballots, quorum, timing, roster generations, cancellation and cooldown\n", checks);
}
