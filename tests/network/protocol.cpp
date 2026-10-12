// Exercise the production wire and snapshot codecs without engine globals.
#include "netprotocol.h"
#include "netsnapshot.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {
unsigned checks;
void check(bool value, const char* expression, int line)
{
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL line %d: %s\n", line, expression); std::exit(1); }
}
#define CHECK(x) check(bool(x), #x, __LINE__)
uint32_t randomState = 0x584293b1u;
uint32_t randomValue()
{
    randomState ^= randomState << 13;
    randomState ^= randomState >> 17;
    randomState ^= randomState << 5;
    return randomState;
}
void field(std::vector<uint8_t>& bytes, size_t offset, uint32_t value)
{
    for (unsigned i = 0; i != 4; ++i) bytes[offset+i] = uint8_t(value >> (8*i));
}
bool decode(const std::vector<uint8_t>& data)
{
    netwire::Header header;
    netwire::Reader body(nullptr, 0);
    return netwire::decode(data.data(), data.size(), header, body);
}
void framing()
{
    netwire::Header header;
    header.type = netwire::Commands; header.session = 0x78563412;
    header.generation = 0x08070605; header.sequence = 0xfefdfcfb;
    const std::vector<uint8_t> payload = {0xa1, 0x00, 0xff};
    const auto encoded = netwire::encode(header, payload);
    const std::vector<uint8_t> golden = {
        'N','B','N','1', 1,0,0,0, 4,0,0,0, 0x12,0x34,0x56,0x78,
        5,6,7,8, 0xfb,0xfc,0xfd,0xfe, 3,0,0,0, 0xa1,0,0xff};
    CHECK(encoded == golden);
    netwire::Header decoded;
    netwire::Reader body(nullptr, 0);
    CHECK(netwire::decode(encoded.data(), encoded.size(), decoded, body));
    CHECK(decoded.type == header.type && decoded.session == header.session &&
          decoded.generation == header.generation && decoded.sequence == header.sequence);
    std::vector<uint8_t> copy(payload.size());
    CHECK(body.bytes(copy.data(), copy.size()) && body.end() && copy == payload);
    for (size_t length = 0; length < encoded.size(); ++length)
        CHECK(!decode(std::vector<uint8_t>(encoded.begin(), encoded.begin()+length)));
    auto bad = encoded; bad.push_back(0); CHECK(!decode(bad));
    for (auto offset : {0u, 4u, 8u, 24u}) {
        bad = encoded; field(bad, offset, 0xffffffffu); CHECK(!decode(bad));
    }
    bad = encoded; field(bad, 8, 0); CHECK(!decode(bad));
    header.type = 0; CHECK(netwire::encode(header, payload).empty());
    header.type = netwire::Hello;
    CHECK(netwire::encode(header, std::vector<uint8_t>(netwire::MaxPayload+1)).empty());
    CHECK(netwire::newer(1, 0) && netwire::newer(1, 0xffffffffu));
    CHECK(!netwire::newer(0xffffffffu, 1) && !netwire::newer(42, 42));
    CHECK(!netwire::newer(0x80000000u, 0));
    netwire::Writer text; text.text("sixteen characters", 7);
    netwire::Reader textReader(text.data.data(), text.data.size());
    CHECK(textReader.text(7) == "sixteen" && textReader.end());
    netwire::Reader tooShort(text.data.data(), 3); CHECK(tooShort.text(7).empty() && !tooShort.ok);
    netwire::Reader tooLong(text.data.data(), text.data.size()); CHECK(tooLong.text(6).empty() && !tooLong.ok);
    const uint8_t embeddedNull[] = {3,0,'a',0,'b'};
    netwire::Reader nullText(embeddedNull, sizeof(embeddedNull)); nullText.text(3); CHECK(!nullText.ok);
    netwire::Reader empty(nullptr, 0); CHECK(empty.u32() == 0 && !empty.ok);
    uint8_t sentinel = 19; CHECK(!empty.bytes(&sentinel, 1) && sentinel == 19);
    std::puts("PASS: wire framing, little-endian golden bytes, bounds, sequence wrap");
}
bool commandDecode(const netwire::Command& command)
{
    netwire::Writer writer; netwire::writeCommand(writer, command);
    netwire::Reader reader(writer.data.data(), writer.data.size());
    netwire::Command decoded;
    return netwire::readCommand(reader, decoded) && reader.end();
}
void commands()
{
    for (int32_t turn : {INT32_MIN, INT32_MAX, -134217728, -1, 0, 1, 134217727, 134217728}) {
        const int32_t normalized = netwire::normalizeTurn(turn);
        CHECK(normalized >= 0 && normalized < 134217728);
        for (int32_t angle : {0, 1, 134217727}) {
            const int32_t sum = angle+normalized; // Must stay in signed range.
            CHECK((sum & 0x07ffffff) == ((int64_t(angle)+turn) & 0x07ffffff));
        }
    }
    for (unsigned i = 1; i != 4096; ++i) {
        netwire::Command source;
        source.sequence = i; source.forward = int(randomValue()%4097)-2048;
        source.strafe = int(randomValue()%4097)-2048; source.turn = int32_t(randomValue());
        source.mlook = (int(randomValue()%255)-127) * (1<<14);
        source.sync = randomValue() & 63; source.buttons = randomValue() & 63;
        source.use = randomValue() & 15; source.keys = randomValue() & 0x8fff;
        source.weapon = randomValue()%netwire::WeaponCount;
        netwire::Writer writer; netwire::writeCommand(writer, source);
        CHECK(writer.data.size() == 22);
        netwire::Reader reader(writer.data.data(), writer.data.size());
        netwire::Command decoded;
        CHECK(netwire::readCommand(reader, decoded) && reader.end());
        netwire::Writer again; netwire::writeCommand(again, decoded); CHECK(again.data == writer.data);
        for (size_t length = 0; length != writer.data.size(); ++length) {
            netwire::Reader truncated(writer.data.data(), length);
            CHECK(!netwire::readCommand(truncated, decoded));
        }
    }
    netwire::Command source; source.sequence = 1;
    CHECK(commandDecode(source));
    auto bad = source; bad.sequence = 0; CHECK(!commandDecode(bad));
    bad = source; bad.forward = 2049; CHECK(!commandDecode(bad));
    bad = source; bad.strafe = -2049; CHECK(!commandDecode(bad));
    bad = source; bad.mlook = (127<<14)+1; CHECK(!commandDecode(bad));
    bad = source; bad.weapon = netwire::WeaponCount; CHECK(!commandDecode(bad));
    bad = source; bad.weapon = 15; CHECK(!commandDecode(bad));
    bad = source; bad.buttons = 64; CHECK(!commandDecode(bad));
    bad = source; bad.use = 16; CHECK(!commandDecode(bad));
    bad = source; bad.sync = 64; CHECK(!commandDecode(bad));
    for (uint16_t key : {0x1000, 0x2000, 0x4000}) {
        bad = source; bad.keys = key; CHECK(!commandDecode(bad));
    }
    netwire::CommandQueue queue;
    // Three physical copies and reordering must execute each input at most once.
    for (uint32_t sequence : {3u, 1u, 5u, 2u}) {
        source.sequence = sequence; CHECK(queue.push(source));
        CHECK(!queue.push(source)); CHECK(!queue.push(source));
    }
    for (uint32_t sequence : {1u, 2u, 3u, 5u}) {
        CHECK(queue.pop(source) && source.sequence == sequence && queue.acknowledged == sequence);
    }
    CHECK(!queue.pop(source));
    source.sequence = 4; CHECK(!queue.push(source)); // Gap acknowledged by consuming 5.
    source.sequence = 261; CHECK(!queue.push(source)); // Bounded future lead.
    source.sequence = 260; CHECK(queue.push(source));
    CHECK(queue.pop(source) && source.sequence == 260);
    queue = netwire::CommandQueue(); queue.acknowledged = 0xfffffffeu;
    source.sequence = 1; CHECK(queue.push(source));
    source.sequence = 0xffffffffu; CHECK(queue.push(source));
    CHECK(queue.pop(source) && source.sequence == 0xffffffffu);
    CHECK(queue.pop(source) && source.sequence == 1);
    CHECK(!queue.push(source));
    source.sequence = 1024; CHECK(!queue.push(source));
    const uint32_t acknowledged = queue.acknowledged;
    CHECK(queue.resync(source) && queue.acknowledged == acknowledged);
    CHECK(queue.pop(source) && source.sequence == 1024 && queue.acknowledged == 1024);
    CHECK(!queue.resync(source));
    source.sequence = 1025; CHECK(queue.push(source));
    CHECK(queue.pop(source) && source.sequence == 1025);
    std::puts("PASS: command validation, triplicate suppression, gaps, reorder, wrap and bounded lead");
}
void snapshots()
{
    std::vector<uint8_t> baseline(32768);
    for (auto& byte : baseline) byte = uint8_t(randomValue());
    auto state = baseline; state[17] ^= 0x55; state[32501] ^= 0xff;
    std::vector<uint8_t> wire, output;
    std::string error;
    bool delta = true;
    CHECK(netSnapshotPack(baseline, nullptr, wire, delta, error) && !delta && error.empty());
    const auto full = wire;
    CHECK(wire[0] == 'N' && wire[1] == 'S' && wire[2] == 'Z' && wire[3] == '1');
    CHECK(netSnapshotUnpack(full, nullptr, output, error) && output == baseline);
    CHECK(netSnapshotPack(state, &baseline, wire, delta, error) && delta && wire.size() < full.size()/20);
    const auto difference = wire;
    CHECK(netSnapshotUnpack(difference, &baseline, output, error) && output == state);
    const std::vector<uint8_t> sentinel = {19, 7, 0, 11};
    output = sentinel; CHECK(!netSnapshotUnpack(difference, nullptr, output, error) && output == sentinel);
    auto wrongBaseline = baseline; wrongBaseline[0] ^= 1;
    CHECK(!netSnapshotUnpack(difference, &wrongBaseline, output, error) && output == sentinel);
    wrongBaseline.pop_back(); CHECK(!netSnapshotUnpack(difference, &wrongBaseline, output, error));
    // The receiver can recover from a lost/wrong delta baseline using a full snapshot.
    CHECK(netSnapshotPack(state, nullptr, wire, delta, error) && !delta);
    CHECK(netSnapshotUnpack(wire, nullptr, output, error) && output == state);
    for (int resize : {-512, 512}) {
        auto resized = state; resized.resize(size_t(int(resized.size())+resize), 17);
        CHECK(netSnapshotPack(resized, &baseline, wire, delta, error));
        CHECK(netSnapshotUnpack(wire, &baseline, output, error) && output == resized);
    }
    for (const auto* packet : {&full, &difference}) {
        for (size_t length = 0; length < packet->size(); length += length < 64 ? 1 : 31) {
            wire.assign(packet->begin(), packet->begin()+length); output = sentinel;
            CHECK(!netSnapshotUnpack(wire, &baseline, output, error) && output == sentinel && !error.empty());
        }
        for (size_t offset = 0; offset < packet->size(); offset += offset < 32 ? 1 : 113) {
            wire = *packet; wire[offset] ^= 1; output = sentinel;
            const bool accepted = netSnapshotUnpack(wire, &baseline, output, error);
            // LZ4 can encode the same bytes in more than one way (unused token
            // bits included). Corruption must never produce a different state.
            CHECK(accepted ? output == (packet == &full ? baseline : state) : output == sentinel);
        }
    }
    for (size_t offset : {0u, 4u, 8u, 12u, 16u, 20u, 24u}) {
        wire = full; field(wire, offset, 0xffffffffu); output = sentinel;
        CHECK(!netSnapshotUnpack(wire, &baseline, output, error) && output == sentinel);
    }
    wire = sentinel;
    CHECK(!netSnapshotPack({}, nullptr, wire, delta, error) && wire == sentinel);
    auto huge = std::vector<uint8_t>(kNetSnapshotMaxBytes+1);
    CHECK(!netSnapshotPack(huge, nullptr, wire, delta, error) && wire == sentinel);
    huge.resize(kNetSnapshotMaxBytes);
    CHECK(netSnapshotPack(huge, nullptr, wire, delta, error));
    CHECK(netSnapshotUnpack(wire, nullptr, output, error) && output == huge);
    std::puts("PASS: full/delta snapshots, baseline recovery, shrink/grow, corruption, truncation, maximum size");
}
void malformedCorpus()
{
    // Deterministic malformed inputs run under ASan/UBSan in the test script.
    for (unsigned i = 0; i != 30000; ++i) {
        std::vector<uint8_t> bytes(randomValue()%512);
        for (auto& byte : bytes) byte = uint8_t(randomValue());
        netwire::Header header;
        netwire::Reader body(nullptr, 0);
        if (netwire::decode(bytes.data(), bytes.size(), header, body)) {
            CHECK(header.type >= netwire::Hello && header.type <= netwire::Event);
            CHECK(body.size + netwire::HeaderSize == bytes.size());
        }
        netwire::Reader command(bytes.data(), bytes.size()); netwire::Command ignored;
        netwire::readCommand(command, ignored);
        std::string error;
        const std::vector<uint8_t> sentinel = {1,2,3}; auto output = sentinel;
        bool accepted = netSnapshotUnpack(bytes, nullptr, output, error);
        CHECK(!accepted && output == sentinel);
    }
    std::puts("PASS: 30000 deterministic malformed wire/command/snapshot inputs");
}
}
int main()
{
    framing(); commands(); snapshots(); malformedCorpus();
    std::printf("PASS: network codecs (%u assertions)\n", checks);
}
