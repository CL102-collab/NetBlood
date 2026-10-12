// SPDX-License-Identifier: GPL-2.0-or-later
// Cooperative cinematics are presentation barriers, independent of game state.
#pragma once

#include "netprotocol.h"

namespace netcinematic
{
constexpr size_t MaxPath = 259;
constexpr uint32_t SkipHoldMillis = 3000;

enum class Kind : uint8_t { Intro = 0, Map = 1, Outro = 2 };

struct Descriptor
{
    std::string video, audio;
    int32_t audioResource = -1;
    Kind kind = Kind::Intro;
};

struct State
{
    uint32_t id = 0;
    bool active = false;
    uint8_t participants = 0, done = 0;
    Descriptor descriptor;
};

struct Done
{
    uint32_t slotGeneration = 0, id = 0;
};

// Absolute DOS/CD paths must be normalized by the sender. Relative mod paths
// remain usable, but the receiver must never traverse out of its search paths.
inline bool validPath(const std::string& path)
{
    if (path.size() > MaxPath)
        return false;
    if (path.empty())
        return true;
    size_t begin = 0;
    for (size_t i = 0; i <= path.size(); ++i)
    {
        if (i == path.size() || path[i] == '/')
        {
            const size_t length = i - begin;
            // Windows strips trailing dots/spaces, including from a ".. "
            // component. Reject ambiguous names consistently on all platforms.
            if (!length || path[i - 1] == '.' || path[i - 1] == ' ')
                return false;
            begin = i + 1;
        }
        else
        {
            const auto c = static_cast<unsigned char>(path[i]);
            if (c < 32 || c == 127 || c == '\\' || c == ':')
                return false;
        }
    }
    return true;
}

inline bool validDescriptor(const Descriptor& descriptor)
{
    return !descriptor.video.empty() && validPath(descriptor.video) &&
        validPath(descriptor.audio) && descriptor.audioResource >= -1 &&
        static_cast<unsigned>(descriptor.kind) <= static_cast<unsigned>(Kind::Outro);
}

inline bool validState(const State& state)
{
    if (!state.id || (state.done & ~state.participants))
        return false;
    return state.active ? validDescriptor(state.descriptor) :
        state.participants == 0 && state.done == 0;
}

inline bool writeState(netwire::Writer& writer, const State& state)
{
    if (!validState(state))
        return false;
    writer.u32(state.id);
    writer.u8(state.active ? 1 : 0);
    writer.u8(state.participants);
    writer.u8(state.done);
    if (state.active)
    {
        writer.u8(static_cast<uint8_t>(state.descriptor.kind));
        writer.u32(static_cast<uint32_t>(state.descriptor.audioResource));
        writer.text(state.descriptor.video, MaxPath);
        writer.text(state.descriptor.audio, MaxPath);
    }
    return true;
}

inline bool readState(netwire::Reader& reader, State& state)
{
    State candidate;
    candidate.id = reader.u32();
    const uint8_t active = reader.u8();
    candidate.active = active != 0;
    candidate.participants = reader.u8();
    candidate.done = reader.u8();
    if (active > 1)
        return false;
    if (candidate.active)
    {
        candidate.descriptor.kind = static_cast<Kind>(reader.u8());
        candidate.descriptor.audioResource = static_cast<int32_t>(reader.u32());
        candidate.descriptor.video = reader.text(MaxPath);
        candidate.descriptor.audio = reader.text(MaxPath);
    }
    if (!reader.end() || !validState(candidate))
        return false;
    state = candidate;
    return true;
}

inline bool writeDone(netwire::Writer& writer, const Done& done)
{
    if (!done.id)
        return false;
    writer.u32(done.slotGeneration);
    writer.u32(done.id);
    return true;
}

inline bool readDone(netwire::Reader& reader, Done& done)
{
    Done candidate;
    candidate.slotGeneration = reader.u32();
    candidate.id = reader.u32();
    if (!reader.end() || !candidate.id)
        return false;
    done = candidate;
    return true;
}

class Barrier
{
public:
    bool start(uint32_t id, uint8_t participants)
    {
        if (!id)
            return false;
        id_ = id;
        participants_ = participants;
        done_ = 0;
        everHadParticipants_ = participants != 0;
        return true;
    }

    void reset()
    {
        id_ = 0;
        participants_ = done_ = 0;
        everHadParticipants_ = false;
    }

    // Disconnects must remove a slot before a new connection reuses it. This
    // clears that slot's acknowledgement even if it rejoins the same scene.
    void updateParticipants(uint8_t participants)
    {
        if (!id_)
            return;
        participants_ = participants;
        done_ &= participants;
        everHadParticipants_ = everHadParticipants_ || participants != 0;
    }

    bool markDone(uint32_t id, unsigned slot)
    {
        if (!id_ || id != id_ || slot >= 8)
            return false;
        const uint8_t bit = static_cast<uint8_t>(1u << slot);
        if (!(participants_ & bit) || (done_ & bit))
            return false;
        done_ |= bit;
        return true;
    }

    bool complete() const
    {
        return id_ && everHadParticipants_ && done_ == participants_;
    }

    uint32_t id() const { return id_; }
    uint8_t participants() const { return participants_; }
    uint8_t done() const { return done_; }

private:
    uint32_t id_ = 0;
    uint8_t participants_ = 0, done_ = 0;
    bool everHadParticipants_ = false;
};

class HoldToSkip
{
public:
    bool update(uint32_t nowMillis, bool held, bool focused = true)
    {
        if (!held || !focused)
        {
            reset();
            return false;
        }
        if (!holding_)
        {
            holding_ = true;
            startMillis_ = nowMillis;
        }
        return nowMillis - startMillis_ >= SkipHoldMillis;
    }

    float progress(uint32_t nowMillis) const
    {
        if (!holding_)
            return 0.f;
        const uint32_t elapsed = nowMillis - startMillis_;
        return elapsed >= SkipHoldMillis ? 1.f : float(elapsed) / SkipHoldMillis;
    }

    void reset() { holding_ = false; startMillis_ = 0; }

private:
    uint32_t startMillis_ = 0;
    bool holding_ = false;
};
} // namespace netcinematic
