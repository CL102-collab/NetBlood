// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <limits>

namespace rnet { namespace rulemodel {
// The original player layout stores these counters in bytes. Observe every
// simulation tick so wraparound does not reset a modern match's score.
struct Score
{
    int32_t total = 0;
    uint8_t frags = 0, penalties = 0;
};
inline void updateScore(Score& score, uint8_t frags, uint8_t penalties)
{
    int64_t const total = int64_t(score.total) + uint8_t(frags-score.frags) - uint8_t(penalties-score.penalties);
    score.total = total > std::numeric_limits<int32_t>::max() ? std::numeric_limits<int32_t>::max()
        : total < std::numeric_limits<int32_t>::min() ? std::numeric_limits<int32_t>::min() : int32_t(total);
    score.frags = frags; score.penalties = penalties;
}
enum LifeState : uint8_t { Alive, Down, Dead, Out };
struct Life
{
    int remaining = 4;
    LifeState state = Alive;
    unsigned hold = 0;
    bool useWasDown = false;
    uint8_t standingHeight = 36;
};
inline bool knockDown(Life& life, uint8_t standingHeight)
{
    if (life.state != Alive || life.remaining <= 0) return false;
    life.state = Down;
    life.hold = 0;
    life.useWasDown = true;
    life.standingHeight = standingHeight;
    return true;
}
// Entering a downed state with Use held must not accidentally spend a life.
// Releasing Use also cancels an incomplete hold. A full hold spends one life.
inline bool giveUp(Life& life, bool use, unsigned requiredTicks)
{
    if (life.state != Down) return false;
    if (!use) { life.hold = 0; life.useWasDown = false; return false; }
    if (life.useWasDown || !requiredTicks) return false;
    if (++life.hold < requiredTicks) return false;
    if (life.remaining > 0) --life.remaining;
    life.state = life.remaining ? Dead : Out;
    life.hold = 0;
    life.useWasDown = true;
    return true;
}
inline bool revive(Life& life)
{
    if (life.state != Down || life.remaining <= 0) return false;
    life.state = Alive;
    life.hold = 0;
    life.useWasDown = false;
    return true;
}
inline bool canRespawn(const Life& life)
{
    return life.state == Alive || (life.state == Dead && life.remaining > 0 && !life.useWasDown);
}
} }
