// SPDX-License-Identifier: GPL-2.0-or-later
#include "netcinematic.h"

#include <cstdio>
#include <cstdlib>
#include <limits>

static unsigned checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)

static netcinematic::State scene()
{
    netcinematic::State state;
    state.id = 42;
    state.active = true;
    state.participants = 0xa5;
    state.done = 0x81;
    state.descriptor.video = "movie/cs1.smk";
    state.descriptor.audio = "movie/cs1822m.wav";
    state.descriptor.audioResource = 4321;
    state.descriptor.kind = netcinematic::Kind::Intro;
    return state;
}

static void roundTrip(const netcinematic::State& state)
{
    netwire::Writer writer;
    CHECK(netcinematic::writeState(writer, state));
    netwire::Reader reader(writer.data.data(), writer.data.size());
    netcinematic::State decoded;
    CHECK(netcinematic::readState(reader, decoded));
    CHECK(decoded.id == state.id && decoded.active == state.active);
    CHECK(decoded.participants == state.participants && decoded.done == state.done);
    if (state.active)
    {
        CHECK(decoded.descriptor.video == state.descriptor.video);
        CHECK(decoded.descriptor.audio == state.descriptor.audio);
        CHECK(decoded.descriptor.audioResource == state.descriptor.audioResource);
        CHECK(decoded.descriptor.kind == state.descriptor.kind);
    }
    netwire::Writer canonical;
    CHECK(netcinematic::writeState(canonical, decoded));
    CHECK(canonical.data == writer.data);
}

static void rejectedState(const std::vector<uint8_t>& packet)
{
    netcinematic::State unchanged = scene();
    netwire::Reader reader(packet.data(), packet.size());
    CHECK(!netcinematic::readState(reader, unchanged));
    CHECK(unchanged.id == 42 && unchanged.active && unchanged.participants == 0xa5 &&
        unchanged.done == 0x81 && unchanged.descriptor.video == "movie/cs1.smk");
}

static void paths()
{
    const char* allowed[] = {
        "", "cs1.smk", "cs1.ogv", "movie/cs1.smk", "mod/movie/Ending Scene.ogv"
    };
    for (const auto* path : allowed)
        CHECK(netcinematic::validPath(path));
    const char* rejected[] = {
        ".", "..", "/cs1.smk", "movie/", "movie//cs1.smk", "../cs1.smk",
        "movie/../cs1.smk", "./movie/cs1.smk", "movie/./cs1.smk", "movie/..",
        "C:/movie/cs1.smk", "\\cs1.smk", "movie\\cs1.smk", "cs1\n.smk", "cs1\177.smk",
        ".. /cs1.smk", "movie/.. /cs1.smk", "movie./cs1.smk", "cs1.smk ", "cs1.smk."
    };
    for (const auto* path : rejected)
        CHECK(!netcinematic::validPath(path));
    CHECK(netcinematic::validPath("ending..smk"));
    CHECK(netcinematic::validPath(std::string(netcinematic::MaxPath, 'a')));
    CHECK(!netcinematic::validPath(std::string(netcinematic::MaxPath + 1, 'a')));
    CHECK(!netcinematic::validPath(std::string("cs1\0.smk", 8)));
    for (unsigned c = 0; c < 32; ++c)
        CHECK(!netcinematic::validPath(std::string("a") + char(c) + ".ogv"));
}

static void packets()
{
    auto state = scene();
    roundTrip(state);
    for (unsigned kind = 0; kind < 3; ++kind)
    {
        state.descriptor.kind = static_cast<netcinematic::Kind>(kind);
        state.descriptor.audioResource = -1;
        state.descriptor.audio.clear();
        state.descriptor.video = "intro.ogv";
        roundTrip(state);
    }
    state.descriptor.video.assign(netcinematic::MaxPath, 'v');
    state.descriptor.audio.assign(netcinematic::MaxPath, 'a');
    state.descriptor.audioResource = std::numeric_limits<int32_t>::max();
    roundTrip(state);
    netwire::Writer longest;
    CHECK(netcinematic::writeState(longest, state));
    CHECK(longest.data.size() == 16 + 2 * netcinematic::MaxPath);
    for (size_t length = 0; length < longest.data.size(); ++length)
        rejectedState(std::vector<uint8_t>(longest.data.begin(), longest.data.begin() + length));
    longest.data.push_back(0);
    rejectedState(longest.data);

    state = scene();
    netwire::Writer valid;
    CHECK(netcinematic::writeState(valid, state));
    auto malformed = valid.data;
    malformed[0] = 0; // Scene id is little endian 42.
    rejectedState(malformed);
    malformed = valid.data;
    malformed[4] = 2; // Boolean fields have a canonical representation.
    rejectedState(malformed);
    malformed = valid.data;
    malformed[6] = 0xff; // Done must be a subset of the cohort.
    rejectedState(malformed);
    malformed = valid.data;
    malformed[7] = 3; // Unknown scene kind.
    rejectedState(malformed);
    malformed = valid.data;
    malformed[8] = 0xfe;
    malformed[9] = malformed[10] = malformed[11] = 0xff; // Resource -2.
    rejectedState(malformed);
    malformed = valid.data;
    malformed[12] = 0xff;
    malformed[13] = 0xff; // Oversize path length.
    rejectedState(malformed);
    malformed = valid.data;
    malformed[14] = '/'; // Absolute path.
    rejectedState(malformed);
    malformed = valid.data;
    malformed[14] = '\0';
    rejectedState(malformed);

    const auto invalidWrite = [](const netcinematic::State& invalid) {
        netwire::Writer writer;
        writer.u8(0xab);
        CHECK(!netcinematic::writeState(writer, invalid));
        CHECK(writer.data.size() == 1 && writer.data[0] == 0xab);
    };
    state.id = 0;
    invalidWrite(state);
    state = scene();
    state.descriptor.video.clear();
    invalidWrite(state);
    state = scene();
    state.descriptor.video.assign(netcinematic::MaxPath + 1, 'v');
    invalidWrite(state);
    state = scene();
    state.active = false;
    invalidWrite(state); // Released barriers carry no participant masks.
    state.participants = state.done = 0;
    roundTrip(state);
    netwire::Writer released;
    CHECK(netcinematic::writeState(released, state));
    CHECK(released.data.size() == 7);
    released.data.push_back(0);
    rejectedState(released.data);

    state = scene();
    for (unsigned participants = 0; participants <= 255; ++participants)
        for (unsigned done = 0; done <= 255; ++done)
        {
            state.participants = uint8_t(participants);
            state.done = uint8_t(done);
            netwire::Writer writer;
            const bool expected = (done & ~participants) == 0;
            CHECK(netcinematic::writeState(writer, state) == expected);
            if (expected)
                roundTrip(state);
            else
                CHECK(writer.data.empty());
        }

    netcinematic::Done done;
    done.slotGeneration = 0x12345678;
    done.id = 42;
    netwire::Writer ack;
    CHECK(netcinematic::writeDone(ack, done));
    CHECK(ack.data.size() == 8);
    netcinematic::Done decoded;
    netwire::Reader ackReader(ack.data.data(), ack.data.size());
    CHECK(netcinematic::readDone(ackReader, decoded));
    CHECK(decoded.slotGeneration == done.slotGeneration && decoded.id == done.id);
    ack.data.push_back(0);
    for (size_t size = 0; size <= 9; ++size)
    {
        netwire::Reader reader(ack.data.data(), size);
        decoded.id = 17;
        CHECK(netcinematic::readDone(reader, decoded) == (size == 8));
        CHECK(decoded.id == (size == 8 ? 42u : 17u));
    }
    done.id = 0;
    const auto old = ack.data;
    CHECK(!netcinematic::writeDone(ack, done));
    CHECK(ack.data == old);
    ack.data.resize(8);
    ack.data[4] = 0;
    netwire::Reader zeroId(ack.data.data(), ack.data.size());
    CHECK(!netcinematic::readDone(zeroId, decoded));
}

static void barriers()
{
    netcinematic::Barrier barrier;
    CHECK(!barrier.complete());
    CHECK(!barrier.markDone(0, 0));
    barrier.updateParticipants(0xff);
    CHECK(barrier.participants() == 0);
    CHECK(!barrier.start(0, 0xff));
    CHECK(barrier.id() == 0);
    CHECK(barrier.start(42, 0));
    CHECK(!barrier.complete()); // Empty dedicated server waits for a first join.
    barrier.updateParticipants(0);
    CHECK(!barrier.complete());
    barrier.updateParticipants(1);
    CHECK(!barrier.complete());
    barrier.updateParticipants(0);
    CHECK(barrier.complete()); // Everyone disconnecting must release the map.

    for (unsigned mask = 1; mask <= 255; ++mask)
    {
        CHECK(barrier.start(42, uint8_t(mask)));
        CHECK(!barrier.complete());
        CHECK(!barrier.start(0, 0));
        CHECK(barrier.id() == 42 && barrier.participants() == mask);
        unsigned done = 0;
        for (unsigned slot = 0; slot < 8; ++slot)
        {
            CHECK(!barrier.markDone(41, slot));
            CHECK(!barrier.markDone(43, slot));
            CHECK(barrier.markDone(42, slot) == bool(mask & (1u << slot)));
            if (mask & (1u << slot))
                done |= 1u << slot;
            CHECK(barrier.done() == done);
            CHECK(barrier.complete() == (done == mask));
            CHECK(!barrier.markDone(42, slot)); // Duplicated UDP packets are harmless.
        }
        CHECK(barrier.complete());
        CHECK(!barrier.markDone(42, 8));
        CHECK(!barrier.markDone(42, std::numeric_limits<unsigned>::max()));
        for (unsigned keep = 0; keep <= 255; ++keep)
        {
            CHECK(barrier.start(43, uint8_t(mask)));
            for (unsigned slot = 0; slot < 8; ++slot)
                barrier.markDone(43, slot);
            barrier.updateParticipants(uint8_t(keep));
            CHECK(barrier.done() == (mask & keep));
            CHECK(barrier.participants() == keep);
            CHECK(barrier.complete() == ((keep & ~mask) == 0));
        }
    }
    CHECK(barrier.start(51, 3));
    CHECK(barrier.markDone(51, 1));
    barrier.updateParticipants(1); // Slot 1 disconnects.
    CHECK(barrier.done() == 0);
    barrier.updateParticipants(3); // A new client reuses slot 1.
    CHECK(barrier.done() == 0 && !barrier.complete());
    CHECK(barrier.markDone(51, 0));
    CHECK(!barrier.complete());
    CHECK(barrier.markDone(51, 1));
    CHECK(barrier.complete());
    CHECK(barrier.start(52, 3)); // Back-to-back outro/intro do not share acknowledgements.
    CHECK(!barrier.complete() && barrier.done() == 0);
    CHECK(!barrier.markDone(51, 0));
    barrier.reset();
    CHECK(!barrier.complete() && !barrier.id() && !barrier.participants() && !barrier.done());
}

static void holds()
{
    netcinematic::HoldToSkip hold;
    CHECK(hold.progress(0) == 0.f);
    CHECK(!hold.update(100, true));
    CHECK(hold.progress(100) == 0.f);
    CHECK(!hold.update(1600, true));
    CHECK(hold.progress(1600) == .5f);
    CHECK(!hold.update(3099, true));
    CHECK(hold.update(3100, true));
    CHECK(hold.progress(3100) == 1.f);
    CHECK(hold.update(9100, true));
    CHECK(hold.progress(9100) == 1.f);
    CHECK(!hold.update(9101, false));
    CHECK(hold.progress(9101) == 0.f);
    CHECK(!hold.update(10000, true));
    CHECK(!hold.update(12999, false));
    CHECK(!hold.update(13000, true));
    CHECK(!hold.update(15999, true));
    CHECK(hold.update(16000, true));
    CHECK(!hold.update(17000, true, false));
    CHECK(hold.progress(17000) == 0.f);
    CHECK(!hold.update(20000, true, true));
    CHECK(!hold.update(22999, true, true));
    CHECK(hold.update(23000, true, true));
    hold.reset();
    const uint32_t beforeWrap = std::numeric_limits<uint32_t>::max() - 999;
    CHECK(!hold.update(beforeWrap, true));
    CHECK(!hold.update(0, true));
    CHECK(hold.progress(500) == .5f);
    CHECK(!hold.update(1999, true));
    CHECK(hold.update(2000, true));
    hold.reset();
    CHECK(!hold.update(0, false));
    CHECK(!hold.update(50000, true)); // Idle time does not count toward a hold.
    CHECK(!hold.update(52999, true));
    CHECK(hold.update(53000, true));
}

int main()
{
    paths();
    packets();
    barriers();
    holds();
    std::printf("Cinematic protocol, barrier and skip-hold tests passed (%u checks).\n", checks);
}
