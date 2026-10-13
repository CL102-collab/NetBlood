// SPDX-License-Identifier: GPL-2.0-or-later
// Frozen end-of-match results; presentation state outside the game simulation.
#pragma once

#include "netscoreboard.h"

namespace netintermission
{
constexpr uint32_t DurationMs = 15000;

struct State
{
    bool active = false;
    uint32_t remainingMs = 0;
    netscoreboard::State scoreboard;
};

inline bool validState(const State& state)
{
    return state.remainingMs <= DurationMs && netscoreboard::validState(state.scoreboard) &&
        (state.active || (state.remainingMs == 0 && state.scoreboard.rows.empty()));
}

inline bool writeState(netwire::Writer& writer, const State& state)
{
    if (!validState(state)) return false;
    writer.u8(state.active ? 1 : 0);
    writer.u32(state.remainingMs);
    return netscoreboard::writeState(writer, state.scoreboard);
}

inline bool readState(netwire::Reader& reader, State& state)
{
    State candidate;
    uint8_t const active = reader.u8();
    if (active > 1) return false;
    candidate.active = active != 0;
    candidate.remainingMs = reader.u32();
    if (!netscoreboard::readState(reader, candidate.scoreboard) || !validState(candidate))
        return false;
    state = candidate;
    return true;
}

// Display a smooth local countdown between authoritative updates. Elapsed time
// is relative to receipt of this state; an expired timer never underflows.
inline uint32_t secondsRemaining(const State& state, uint32_t elapsedMs)
{
    if (!state.active) return 0;
    uint32_t const remaining = state.remainingMs < DurationMs ? state.remainingMs : DurationMs;
    return elapsedMs < remaining ? (remaining - elapsedMs + 999) / 1000 : 0;
}
}
