// SPDX-License-Identifier: GPL-2.0-or-later
// Production scoreboard wire validation; no engine state or display required.
#include "netscoreboard.h"
#include <cstdio>
#include <cstdlib>
#include <limits>

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)

static netscoreboard::State fixture(unsigned count)
{
    netscoreboard::State state;
    for (unsigned slot = 0; slot < count; ++slot)
    {
        netscoreboard::Row row;
        row.slot = slot;
        row.generation = slot + 1;
        row.team = slot % 3 < 2 ? slot % 3 : netscoreboard::NoTeam;
        row.score = static_cast<int>(slot) - 3;
        row.livesRemaining = static_cast<int>(slot) - 1;
        row.downed = row.livesRemaining > 0 && slot % 2;
        row.giveUpTicks = row.downed ? slot * 10 : 0;
        row.pingMs = slot * 99;
        row.elapsedMinutes = slot * 60;
        row.name = slot ? "FifteenChars123" : "Player One";
        row.country = slot % 2 ? "HU" : "";
        state.rows.push_back(row);
    }
    return state;
}

static std::vector<uint8_t> encode(netscoreboard::State const& state)
{
    netwire::Writer writer;
    CHECK(netscoreboard::writeState(writer, state));
    return writer.data;
}

static bool decode(std::vector<uint8_t> const& data, netscoreboard::State& state)
{
    netwire::Reader reader(data.data(), data.size());
    return netscoreboard::readState(reader, state);
}

static void reject(std::vector<uint8_t> const& data)
{
    auto state = fixture(3);
    auto before = encode(state);
    CHECK(!decode(data, state));
    CHECK(encode(state) == before); // Failure must not install a partial roster.
}

static void invalid(netscoreboard::State const& state)
{
    CHECK(!netscoreboard::validState(state));
    netwire::Writer writer;
    writer.u8(37);
    CHECK(!netscoreboard::writeState(writer, state));
    CHECK(writer.data == std::vector<uint8_t>({37}));
}

static void roundTrips()
{
    for (unsigned count = 0; count <= netscoreboard::MaxPlayers; ++count)
    {
        auto input = fixture(count);
        auto bytes = encode(input);
        netscoreboard::State output;
        CHECK(decode(bytes, output));
        CHECK(encode(output) == bytes);
        for (size_t length = 0; length < bytes.size(); ++length)
            reject(std::vector<uint8_t>(bytes.begin(), bytes.begin() + length));
        bytes.push_back(0);
        reject(bytes);
    }
    netscoreboard::State empty;
    CHECK(encode(empty) == std::vector<uint8_t>({0}));
    auto one = fixture(1);
    one.rows[0].score = -1;
    one.rows[0].name = "X";
    one.rows[0].country = "US";
    const std::vector<uint8_t> golden = {
        1, 0, 0, 1,0,0,0, 0,0,0,0, 0,0,0,0, 255,255,255,255, 255,255,255,255,
        0,0, 1,0,'X', 2,0,'U','S'};
    CHECK(encode(one) == golden);
    for (int32_t score : {INT32_MIN, -1, 0, INT32_MAX})
    for (int32_t lives : {-1, 0, 1, 4, 255})
    {
        one.rows[0].score = score;
        one.rows[0].livesRemaining = lives;
        one.rows[0].pingMs = netscoreboard::MaxPingMs;
        one.rows[0].generation = UINT32_MAX;
        one.rows[0].elapsedMinutes = UINT32_MAX;
        one.rows[0].name.assign(netscoreboard::MaxName, 'x');
        netscoreboard::State restored;
        CHECK(decode(encode(one), restored));
        CHECK(restored.rows[0].score == score && restored.rows[0].elapsedMinutes == UINT32_MAX);
        CHECK(restored.rows[0].livesRemaining == lives);
        CHECK(!restored.rows[0].downed && !restored.rows[0].giveUpTicks);
    }
    for (int32_t lives : {1, 4, 255})
    for (unsigned ticks = 0; ticks <= 90; ++ticks)
    {
        one.rows[0].livesRemaining = lives;
        one.rows[0].downed = true;
        one.rows[0].giveUpTicks = ticks;
        netscoreboard::State restored;
        CHECK(decode(encode(one), restored));
        CHECK(restored.rows[0].livesRemaining == lives && restored.rows[0].downed &&
              restored.rows[0].giveUpTicks == ticks);
    }
}

static void malformed()
{
    invalid(fixture(netscoreboard::MaxPlayers + 1));
    auto state = fixture(2);
    state.rows[1].slot = 0; invalid(state);
    state = fixture(2); state.rows[0].slot = 1; state.rows[1].slot = 0; invalid(state);
    state = fixture(1); state.rows[0].slot = netscoreboard::MaxPlayers; invalid(state);
    state = fixture(1); state.rows[0].generation = 0; invalid(state);
    state = fixture(1); state.rows[0].pingMs = netscoreboard::MaxPingMs + 1; invalid(state);
    for (int32_t lives : {INT32_MIN, -2, 256, INT32_MAX})
    {
        state = fixture(1); state.rows[0].livesRemaining = lives; invalid(state);
    }
    for (int32_t lives : {-1, 0})
    {
        state = fixture(1); state.rows[0].livesRemaining = lives;
        state.rows[0].downed = true; invalid(state);
    }
    state = fixture(1); state.rows[0].livesRemaining = 4; state.rows[0].giveUpTicks = 1; invalid(state);
    for (unsigned ticks = 91; ticks <= 255; ++ticks)
    {
        state = fixture(1); state.rows[0].livesRemaining = 4;
        state.rows[0].downed = true; state.rows[0].giveUpTicks = ticks; invalid(state);
    }
    for (unsigned team = 2; team < netscoreboard::NoTeam; ++team)
    {
        state = fixture(1); state.rows[0].team = team; invalid(state);
    }
    state = fixture(1); state.rows[0].name.assign(netscoreboard::MaxName + 1, 'n'); invalid(state);
    for (unsigned c = 0; c < 128; ++c)
        if (c < 32 || c == 127)
        {
            state = fixture(1); state.rows[0].name = std::string("A") + static_cast<char>(c) + "B"; invalid(state);
        }
    for (const char* country : {"U", "USA", "us", "Us", "1A", "A-", "A\n", "\377\377"})
    {
        state = fixture(1); state.rows[0].country = country; invalid(state);
    }
    // Mutate actual wire payloads so reader validation is independently covered.
    state = fixture(1); state.rows[0].name = "X"; state.rows[0].country = "US";
    auto const good = encode(state);
    auto bad = good; bad[0] = 9; reject(bad);
    bad = good; bad[1] = 8; reject(bad);
    bad = good; bad[2] = 2; reject(bad);
    bad = good; bad[3] = 0; reject(bad);
    bad = good; bad[7] = 0xff; bad[8] = 0xff; reject(bad);
    for (uint32_t lives : {256u, 0x80000000u, UINT32_MAX - 1})
    {
        bad = good;
        for (unsigned byte = 0; byte < 4; ++byte) bad[19 + byte] = static_cast<uint8_t>(lives >> (byte * 8));
        reject(bad);
    }
    for (unsigned downed = 2; downed <= 255; ++downed)
    {
        bad = good; bad[23] = downed; reject(bad);
    }
    bad = good; bad[23] = 1; reject(bad); // Unlimited lives cannot have a downed state.
    bad = good; bad[23] = 1;
    for (unsigned byte = 0; byte < 4; ++byte) bad[19 + byte] = 0;
    reject(bad); // Exhausted players spectate, rather than becoming downed again.
    bad = good; bad[24] = 1; reject(bad); // Progress requires being downed.
    for (unsigned ticks = 91; ticks <= 255; ++ticks)
    {
        bad = good;
        for (unsigned byte = 0; byte < 4; ++byte) bad[19 + byte] = byte == 0 ? 4 : 0;
        bad[23] = 1; bad[24] = ticks; reject(bad);
    }
    bad = good; bad[25] = 16; reject(bad);
    bad = good; bad[27] = 0; reject(bad);
    bad = good; bad[27] = 31; reject(bad);
    bad = good; bad[28] = 3; reject(bad);
    bad = good; bad[30] = 'u'; reject(bad);
    bad = good; bad[31] = 0; reject(bad);
    state = fixture(2);
    bad = encode(state);
    size_t const secondOffset = 1 + 24 + 2 + state.rows[0].name.size() + 2 + state.rows[0].country.size();
    bad[secondOffset] = 0;
    reject(bad);
}

static void clocks()
{
    CHECK(netscoreboard::joinedMinutes(0, 0) == 0);
    CHECK(netscoreboard::joinedMinutes(4, 5) == 0);
    CHECK(netscoreboard::joinedMinutes(59999, 0) == 0);
    CHECK(netscoreboard::joinedMinutes(60000, 0) == 1);
    CHECK(netscoreboard::joinedMinutes(119999, 60000) == 0);
    CHECK(netscoreboard::joinedMinutes(120000, 60000) == 1);
    CHECK(netscoreboard::joinedMinutes(uint64_t(UINT32_MAX) + 60000, UINT32_MAX) == 1);
    CHECK(netscoreboard::joinedMinutes(UINT64_MAX, UINT64_MAX - 60000) == 1);
    CHECK(netscoreboard::joinedMinutes(UINT64_MAX, 0) == UINT32_MAX);
    CHECK(netscoreboard::joinedMinutes(uint64_t(UINT32_MAX) * 60000, 0) == UINT32_MAX);
}

int main()
{
    roundTrips();
    malformed();
    clocks();
    std::printf("PASS: %u scoreboard codec checks (bounds, atomic rejection, names, countries, scores, lives, revival and join clocks)\n", checks);
}
