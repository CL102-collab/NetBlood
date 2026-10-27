// Server-time coordinates for the remote world displayed by a client.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cstdint>

struct NetLagCompensationTime
{
    uint32_t tick;
    uint16_t fraction; // Fraction of the interval from tick to tick + 1.
    NetLagCompensationTime(uint32_t tick = 0, uint16_t fraction = 0) : tick(tick), fraction(fraction) {}
};

namespace netcommandtime
{
constexpr int64_t TickUnits = 65536;
constexpr int64_t MaxAge = 30 * TickUnits; // The authoritative simulation is 30 Hz.

inline NetLagCompensationTime advance(NetLagCompensationTime time, uint32_t ticks)
{
    time.tick += ticks;
    return time;
}

inline NetLagCompensationTime interpolate(uint32_t previous, uint32_t current, unsigned fraction)
{
    uint32_t const distance = current - previous;
    if (distance > 30 || fraction >= 65536) return {current, 0};
    uint64_t const offset = uint64_t(distance) * fraction;
    return {previous + uint32_t(offset >> 16), uint16_t(offset)};
}

// Validate against transport-observed latency when the command is first
// admitted, before queueing. Absolute snapshot time includes both the snapshot
// trip to the client and the command's return trip, hence the full RTT here.
// Allow one snapshot interval plus 200ms for input/render scheduling and jitter;
// an implausible timestamp falls back to the measured latency/interpolation age.
inline NetLagCompensationTime validate(NetLagCompensationTime requested, uint32_t receivedTick,
                                      uint32_t roundTripMs, unsigned snapshotRate)
{
    unsigned const rate = std::max(1u, std::min(30u, snapshotRate));
    int64_t const interval = ((30 + rate - 1) / rate) * TickUnits;
    int64_t const latency = int64_t(std::min(1000u, roundTripMs)) * 30 * TickUnits / 1000;
    int64_t const maximum = std::min(MaxAge, latency + interval + 6 * TickUnits);
    int64_t age = int64_t(int32_t(receivedTick - requested.tick)) * TickUnits - requested.fraction;
    if (age < 0 || age > maximum) age = std::min(MaxAge, latency + interval / 2);
    // The history owns map/life boundaries; modular subtraction also works
    // across server-tick rollover. A time before map start has no saved sample.
    uint32_t const whole = uint32_t(age / TickUnits);
    uint16_t const remainder = uint16_t(age % TickUnits);
    return {receivedTick - whole - unsigned(remainder != 0), uint16_t(remainder ? TickUnits - remainder : 0)};
}
} // namespace netcommandtime
