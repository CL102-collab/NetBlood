// SPDX-License-Identifier: GPL-2.0-or-later
#include "netintermission.h"
#include <cstdio>
#include <cstdlib>

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)

static netintermission::State fixture(unsigned count = 3)
{
    netintermission::State state;
    state.active = true;
    state.remainingMs = netintermission::DurationMs;
    for (unsigned slot = 0; slot < count; ++slot)
    {
        netscoreboard::Row row;
        row.slot = slot;
        row.team = slot % 3 < 2 ? slot % 3 : netscoreboard::NoTeam;
        row.generation = slot + 1;
        row.pingMs = slot * 101;
        row.elapsedMinutes = slot * 12;
        row.score = static_cast<int32_t>(slot) - 3;
        row.livesRemaining = slot ? 4 : -1;
        row.downed = slot % 2;
        row.giveUpTicks = row.downed ? 90 : 0;
        row.name = slot ? "Player" + std::to_string(slot) : "Caleb";
        row.country = slot % 2 ? "HU" : "US";
        state.scoreboard.rows.push_back(row);
    }
    return state;
}

static std::vector<uint8_t> encode(const netintermission::State& state)
{
    netwire::Writer writer;
    CHECK(netintermission::writeState(writer, state));
    return writer.data;
}

static bool decode(const std::vector<uint8_t>& bytes, netintermission::State& state)
{
    netwire::Reader reader(bytes.data(), bytes.size());
    return netintermission::readState(reader, state);
}

static void reject(const std::vector<uint8_t>& bytes)
{
    auto state = fixture();
    auto const before = encode(state);
    CHECK(!decode(bytes, state));
    CHECK(encode(state) == before); // Failed packets cannot replace final results.
}

static void invalid(const netintermission::State& state)
{
    CHECK(!netintermission::validState(state));
    netwire::Writer writer;
    writer.u8(37);
    CHECK(!netintermission::writeState(writer, state));
    CHECK(writer.data == std::vector<uint8_t>({37}));
}

static void roundTrips()
{
    netintermission::State inactive;
    CHECK(encode(inactive) == std::vector<uint8_t>({0, 0, 0, 0, 0, 0}));
    auto restored = fixture();
    CHECK(decode(encode(inactive), restored));
    CHECK(!restored.active && !restored.remainingMs && restored.scoreboard.rows.empty());
    for (unsigned count = 0; count <= netscoreboard::MaxPlayers; ++count)
    for (uint32_t remaining : {0u, 1u, 999u, 1000u, 14999u, netintermission::DurationMs})
    {
        auto state = fixture(count);
        state.remainingMs = remaining;
        auto bytes = encode(state);
        CHECK(decode(bytes, restored));
        CHECK(encode(restored) == bytes);
        for (size_t length = 0; length < bytes.size(); ++length)
            reject(std::vector<uint8_t>(bytes.begin(), bytes.begin() + length));
        bytes.push_back(0);
        reject(bytes);
    }

    // Preserve final individual/team results, even after the live roster changes.
    auto original = fixture(netscoreboard::MaxPlayers);
    original.scoreboard.rows[0].score = INT32_MIN;
    original.scoreboard.rows[1].score = INT32_MAX;
    original.scoreboard.rows[2].generation = UINT32_MAX;
    original.scoreboard.rows[3].pingMs = netscoreboard::MaxPingMs;
    original.scoreboard.rows[4].elapsedMinutes = UINT32_MAX;
    auto const bytes = encode(original);
    CHECK(decode(bytes, restored));
    for (size_t n = 0; n < original.scoreboard.rows.size(); ++n)
    {
        auto const& expected = original.scoreboard.rows[n];
        auto const& actual = restored.scoreboard.rows[n];
        CHECK(actual.slot == expected.slot && actual.generation == expected.generation);
        CHECK(actual.team == expected.team && actual.score == expected.score);
        CHECK(actual.name == expected.name && actual.country == expected.country);
        CHECK(actual.pingMs == expected.pingMs && actual.elapsedMinutes == expected.elapsedMinutes);
        CHECK(actual.livesRemaining == expected.livesRemaining && actual.downed == expected.downed &&
              actual.giveUpTicks == expected.giveUpTicks);
    }
    original.scoreboard.rows.clear();
    CHECK(encode(restored) == bytes);
}

static void malformed()
{
    auto state = fixture();
    state.active = false; invalid(state);
    state.remainingMs = 0; invalid(state); // Inactive packets cannot retain rows.
    state.scoreboard.rows.clear(); state.remainingMs = 1; invalid(state);
    for (uint32_t remaining : {netintermission::DurationMs + 1, 65535u, UINT32_MAX})
    {
        state = fixture(); state.remainingMs = remaining; invalid(state);
        auto bytes = encode(fixture());
        for (unsigned n = 0; n < 4; ++n) bytes[1 + n] = static_cast<uint8_t>(remaining >> (n * 8));
        reject(bytes);
    }
    auto const good = encode(fixture());
    for (unsigned active = 2; active <= 255; ++active)
    {
        auto bytes = good;
        bytes[0] = active;
        reject(bytes);
    }
    auto bytes = good; bytes[0] = 0; reject(bytes);
    for (unsigned n = 1; n <= 4; ++n) bytes[n] = 0;
    reject(bytes); // Zero remaining time still cannot make inactive rows valid.

    state = fixture(netscoreboard::MaxPlayers + 1); invalid(state);
    state = fixture(); state.scoreboard.rows[1].slot = 0; invalid(state);
    state = fixture(); state.scoreboard.rows[0].generation = 0; invalid(state);
    state = fixture(); state.scoreboard.rows[0].team = 2; invalid(state);
    state = fixture(); state.scoreboard.rows[0].pingMs = netscoreboard::MaxPingMs + 1; invalid(state);
    state = fixture(); state.scoreboard.rows[0].name = "Invalid\nName"; invalid(state);
    state = fixture(); state.scoreboard.rows[0].country = "us"; invalid(state);
    state = fixture(); state.scoreboard.rows[0].livesRemaining = -2; invalid(state);

    bytes = good; bytes[5] = netscoreboard::MaxPlayers + 1; reject(bytes);
    bytes = good; bytes[6] = netscoreboard::MaxPlayers; reject(bytes);
    bytes = good; bytes[7] = 2; reject(bytes);
    bytes = good; bytes[8] = 0; reject(bytes); // First row generation is one.
    bytes = good; bytes[12] = 0xff; bytes[13] = 0xff; reject(bytes); // Oversized ping.
    bytes = good; bytes[28] = 2; reject(bytes); // Nonboolean downed field.
}

static void countdown()
{
    auto state = fixture();
    CHECK(netintermission::secondsRemaining(state, 0) == 15);
    CHECK(netintermission::secondsRemaining(state, 999) == 15);
    CHECK(netintermission::secondsRemaining(state, 1000) == 14);
    CHECK(netintermission::secondsRemaining(state, 14000) == 1);
    CHECK(netintermission::secondsRemaining(state, 14999) == 1);
    CHECK(netintermission::secondsRemaining(state, 15000) == 0);
    CHECK(netintermission::secondsRemaining(state, UINT32_MAX) == 0);
    for (uint32_t remaining = 0; remaining <= netintermission::DurationMs; ++remaining)
    {
        state.remainingMs = remaining;
        CHECK(netintermission::secondsRemaining(state, 0) == (remaining + 999) / 1000);
        CHECK(netintermission::secondsRemaining(state, remaining) == 0);
        if (remaining) CHECK(netintermission::secondsRemaining(state, remaining - 1) == 1);
    }
    state.remainingMs = UINT32_MAX;
    CHECK(netintermission::secondsRemaining(state, 0) == 15); // Defensive display clamp.
    state.active = false;
    CHECK(netintermission::secondsRemaining(state, 0) == 0);
}

int main()
{
    roundTrips();
    malformed();
    countdown();
    std::printf("PASS: %u intermission codec checks (frozen results, teams, bounded packets, atomic rejection and countdown)\n", checks);
}
