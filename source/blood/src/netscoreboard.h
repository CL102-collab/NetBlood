// SPDX-License-Identifier: GPL-2.0-or-later
// Authoritative presentation metadata; never part of deterministic game state.
#pragma once

#include "netprotocol.h"

namespace netscoreboard
{
constexpr size_t MaxPlayers = 8;
constexpr size_t MaxName = 15;
constexpr uint8_t NoTeam = 255;
constexpr uint32_t MaxPingMs = 60000;

struct Row
{
    uint8_t slot = 0, team = NoTeam;
    uint32_t generation = 0, pingMs = 0, elapsedMinutes = 0;
    int32_t score = 0;
    std::string name, country; // ISO 3166-1 alpha-2; empty means unknown.
};

struct State
{
    std::vector<Row> rows;
};

inline uint32_t joinedMinutes(uint64_t nowMs, uint64_t joinedMs)
{
    if (nowMs <= joinedMs) return 0;
    uint64_t const minutes = (nowMs - joinedMs) / 60000;
    return minutes > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(minutes);
}

inline bool validState(const State& state)
{
    if (state.rows.size() > MaxPlayers) return false;
    int previous = -1;
    for (auto const& row : state.rows)
    {
        if (row.slot >= MaxPlayers || row.slot <= previous || !row.generation ||
            (row.team != NoTeam && row.team > 1) || row.pingMs > MaxPingMs || row.name.size() > MaxName)
            return false;
        previous = row.slot;
        for (unsigned char c : row.name) if (c < 32 || c == 127) return false;
        if (!row.country.empty() && (row.country.size() != 2 ||
            row.country[0] < 'A' || row.country[0] > 'Z' || row.country[1] < 'A' || row.country[1] > 'Z'))
            return false;
    }
    return true;
}

inline bool writeState(netwire::Writer& writer, const State& state)
{
    if (!validState(state)) return false;
    writer.u8(static_cast<uint8_t>(state.rows.size()));
    for (auto const& row : state.rows)
    {
        writer.u8(row.slot);
        writer.u8(row.team);
        writer.u32(row.generation);
        writer.u32(row.pingMs);
        writer.u32(row.elapsedMinutes);
        writer.u32(static_cast<uint32_t>(row.score));
        writer.text(row.name, MaxName);
        writer.text(row.country, 2);
    }
    return true;
}

inline bool readState(netwire::Reader& reader, State& state)
{
    State candidate;
    unsigned const count = reader.u8();
    if (count > MaxPlayers) return false;
    candidate.rows.reserve(count);
    for (unsigned n = 0; n < count; ++n)
    {
        Row row;
        row.slot = reader.u8();
        row.team = reader.u8();
        row.generation = reader.u32();
        row.pingMs = reader.u32();
        row.elapsedMinutes = reader.u32();
        row.score = static_cast<int32_t>(reader.u32());
        row.name = reader.text(MaxName);
        row.country = reader.text(2);
        candidate.rows.push_back(row);
    }
    if (!reader.end() || !validState(candidate)) return false;
    state = candidate;
    return true;
}
}
