// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

// Independent of game ticks: pausing and changing maps must not reset the
// per-player cooldown. The caller supplies a monotonic millisecond clock.
namespace netvote
{
constexpr size_t MaxPlayers = 8;
constexpr uint32_t DurationMs = 30000;
constexpr uint32_t CooldownMs = 120000;
using Roster = std::array<uint32_t, MaxPlayers>;

enum class Result { NoResult, Passed, Failed };

struct Cooldown
{
    bool used = false;
    uint32_t started = 0;

    bool ready(uint32_t now) const
    {
        return !used || uint32_t(now - started) >= CooldownMs;
    }

    uint32_t remaining(uint32_t now) const
    {
        return ready(now) ? 0 : CooldownMs - uint32_t(now - started);
    }

    // Call only after the server accepts a vote request.
    void accept(uint32_t now)
    {
        used = true;
        started = now;
    }
};

class Vote
{
public:
    bool start(uint32_t now, const Roster& roster, size_t callerSlot, uint32_t callerGeneration)
    {
        if (active_ || callerSlot >= MaxPlayers || !callerGeneration ||
            roster[callerSlot] != callerGeneration)
            return false;

        roster_ = roster;
        ballots_.fill(0);
        eligibleCount_ = 0;
        for (uint32_t generation : roster_)
            eligibleCount_ += generation != 0;
        yesCount_ = 1;
        noCount_ = 0;
        ballots_[callerSlot] = 1; // Calling a vote also votes yes.
        started_ = now;
        active_ = true;
        return true;
    }

    bool cast(uint32_t now, size_t slot, uint32_t generation, bool yes)
    {
        if (!active_ || uint32_t(now - started_) >= DurationMs ||
            !eligible(slot, generation) || ballots_[slot])
            return false;
        ballots_[slot] = yes ? 1 : -1;
        if (yes)
            ++yesCount_;
        else
            ++noCount_;
        return true;
    }

    Result poll(uint32_t now)
    {
        if (!active_ || uint32_t(now - started_) < DurationMs)
            return Result::NoResult;
        active_ = false;
        // Keep both the participation and approval requirements explicit.
        bool quorum = (yesCount_ + noCount_) * 100 >= eligibleCount_ * 51;
        bool approval = yesCount_ * 100 >= eligibleCount_ * 51;
        return quorum && approval ? Result::Passed : Result::Failed;
    }

    // Preserve the final tally when preparing a cancellation notification.
    void cancel(bool preserveTally = false)
    {
        active_ = false;
        if (preserveTally)
            return;
        roster_.fill(0);
        ballots_.fill(0);
        eligibleCount_ = yesCount_ = noCount_ = 0;
    }

    bool active() const { return active_; }

    // Generation tokens prevent reconnects and reused slots from inheriting a
    // ballot. Zero denotes an absent player in the roster frozen at start.
    bool eligible(size_t slot, uint32_t generation) const
    {
        return slot < MaxPlayers && generation && roster_[slot] == generation;
    }

    bool hasVoted(size_t slot, uint32_t generation) const
    {
        return eligible(slot, generation) && ballots_[slot] != 0;
    }

    unsigned eligibleCount() const { return eligibleCount_; }
    unsigned yesCount() const { return yesCount_; }
    unsigned noCount() const { return noCount_; }
    unsigned requiredCount() const { return (eligibleCount_ * 51 + 99) / 100; }

    uint32_t remaining(uint32_t now) const
    {
        if (!active_ || uint32_t(now - started_) >= DurationMs)
            return 0;
        return DurationMs - uint32_t(now - started_);
    }

private:
    Roster roster_{};
    std::array<int8_t, MaxPlayers> ballots_{};
    uint32_t started_ = 0;
    unsigned eligibleCount_ = 0, yesCount_ = 0, noCount_ = 0;
    bool active_ = false;
};
}
