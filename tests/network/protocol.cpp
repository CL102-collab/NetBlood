// Exercise the production wire and snapshot codecs without engine globals.
#include "netprotocol.h"
#include "netsnapshot.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <utility>

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
        'N','B','N','1', 7,0,0,0, 4,0,0,0, 0x12,0x34,0x56,0x78,
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
    bad = encoded; field(bad, 4, 6); CHECK(!decode(bad)); // The previous protocol has no cooperative life budget.
    header.type = 0; CHECK(netwire::encode(header, payload).empty());
    static_assert(netwire::Event == 11 && netwire::RequestMap == 12 && netwire::MapData == 13,
                  "Existing packet and demo message IDs must not change");
    static_assert(netwire::VoteRequest == 16 && netwire::VoteStatus == 17 && netwire::VoteOptions == 18,
                  "Map-vote messages append to existing message IDs");
    static_assert(netwire::CinematicState == 19 && netwire::CinematicDone == 20,
                  "Cinematic messages append to existing message IDs");
    static_assert(netwire::Scoreboard == 21, "Scoreboard metadata appends to existing message IDs");
    static_assert(netwire::Chat == 7 && netwire::PublicEndpoint == 22 &&
                  netwire::Intermission == 23 && netwire::TeamChat == 24,
                  "Team chat appends without changing public chat or existing packet-demo message IDs");
    for (uint32_t type = netwire::Hello; type < netwire::TypeCount; ++type) {
        header.type = type;
        CHECK(decode(netwire::encode(header, payload)));
    }
    header.type = netwire::TypeCount; CHECK(netwire::encode(header, payload).empty());
    bad = encoded; field(bad, 8, netwire::TypeCount); CHECK(!decode(bad));
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
void serverRules()
{
    netwire::ServerRules rules;
    rules.timeLimitMinutes = 10080; rules.scoreLimit = INT32_MAX;
    rules.maxPlayers = 8; rules.autoAim = 3;
    rules.rotationIndex = 255; rules.rotationCount = 256;
    rules.mapVoteEnabled = 1;
    rules.coopLives = 255;
    netwire::Writer writer; netwire::writeServerRules(writer, rules);
    const std::vector<uint8_t> expected = {
        0x60,0x27,0,0, 0xff,0xff,0xff,0x7f, 8,0,0,0, 3,0,0,0,
        0xff,0,0,0, 0,1,0,0, 1,0,0,0, 1,0,0,0, 1,0,0,0, 0xff,0,0,0};
    CHECK(writer.data == expected && writer.data.size() == netwire::ServerRulesBytes);
    netwire::Reader reader(writer.data.data(), writer.data.size());
    netwire::ServerRules decoded;
    CHECK(netwire::readServerRules(reader, decoded) && reader.end());
    netwire::Writer rewritten; netwire::writeServerRules(rewritten, decoded);
    CHECK(rewritten.data == expected);
    // Rejected packets must leave the previously authoritative rules intact.
    for (size_t length = 0; length < expected.size(); ++length) {
        netwire::Reader truncated(expected.data(), length);
        decoded = rules;
        CHECK(!netwire::readServerRules(truncated, decoded));
        rewritten.data.clear(); netwire::writeServerRules(rewritten, decoded);
        CHECK(rewritten.data == expected);
    }
    const std::pair<unsigned, uint32_t> invalid[] = {
        {0,10081}, {0,UINT32_MAX}, {4,0x80000000u}, {8,0}, {8,9}, {12,4},
        {16,256}, {16,UINT32_MAX}, {20,257}, {20,0}, {24,2}, {24,UINT32_MAX},
        {28,2}, {28,UINT32_MAX}, {32,2}, {32,UINT32_MAX}, {36,256}, {36,UINT32_MAX}};
    for (const auto& mutation : invalid) {
        auto bad = expected; field(bad, mutation.first, mutation.second);
        netwire::Reader malformed(bad.data(), bad.size());
        decoded = rules;
        CHECK(!netwire::readServerRules(malformed, decoded));
        rewritten.data.clear(); netwire::writeServerRules(rewritten, decoded);
        CHECK(rewritten.data == expected);
    }
    for (uint32_t cap = 1; cap <= 8; ++cap)
        for (uint32_t aim = 0; aim <= 3; ++aim)
        for (uint32_t enabled = 0; enabled <= 1; ++enabled)
        for (uint32_t holster = 0; holster <= 1; ++holster)
        for (uint32_t chase = 0; chase <= 1; ++chase)
        for (uint32_t lives : {0u, 1u, 4u, 255u}) {
            rules = netwire::ServerRules(); rules.maxPlayers = cap; rules.autoAim = aim; rules.mapVoteEnabled = enabled;
            rules.allowHolsterWeapon = holster; rules.allowChaseView = chase;
            rules.coopLives = lives;
            writer.data.clear(); netwire::writeServerRules(writer, rules);
            netwire::Reader minimal(writer.data.data(), writer.data.size());
            CHECK(netwire::readServerRules(minimal, decoded) && minimal.end());
            CHECK(decoded.maxPlayers == cap && decoded.autoAim == aim && decoded.mapVoteEnabled == enabled &&
                  decoded.allowHolsterWeapon == holster && decoded.allowChaseView == chase &&
                  decoded.coopLives == lives &&
                  !decoded.timeLimitMinutes && !decoded.scoreLimit);
        }
    std::puts("PASS: authoritative server rules, exact wire bytes, bounds, truncation and unchanged rejected output");
}
template<class Payload>
void voteRoundtrip(const Payload& source,
                   bool (*write)(netwire::Writer&, const Payload&),
                   bool (*read)(netwire::Reader&, Payload&))
{
    netwire::Writer wire;
    CHECK(write(wire, source));
    netwire::Reader reader(wire.data.data(), wire.data.size());
    Payload decoded;
    CHECK(read(reader, decoded) && reader.end());
    netwire::Writer copy;
    CHECK(write(copy, decoded) && copy.data == wire.data);
    for (size_t size = 0; size < wire.data.size(); ++size) {
        netwire::Reader truncated(wire.data.data(), size);
        decoded = source;
        CHECK(!read(truncated, decoded));
        copy.data.clear();
        CHECK(write(copy, decoded) && copy.data == wire.data);
    }
    auto extra = wire.data; extra.push_back(0);
    netwire::Reader trailing(extra.data(), extra.size());
    CHECK(!read(trailing, decoded));
}

template<class Payload>
void voteInvalid(const Payload& invalid, bool (*write)(netwire::Writer&, const Payload&))
{
    netwire::Writer wire; wire.data = {73};
    CHECK(!write(wire, invalid) && wire.data == std::vector<uint8_t>({73}));
}

void mapVotes()
{
    using namespace netwire;
    static_assert(uint32_t(VoteCommand::List) == 1 && uint32_t(VoteCommand::Map) == 2
                  && uint32_t(VoteCommand::Restart) == 3 && uint32_t(VoteCommand::Next) == 4
                  && uint32_t(VoteCommand::Yes) == 5 && uint32_t(VoteCommand::No) == 6
                  && uint32_t(VoteCommand::QueryStatus) == 7 && uint32_t(VoteCommand::Shuffle) == 8,
                  "Team shuffle appends to existing vote commands");
    VoteRequestPayload request;
    for (VoteCommand command : {VoteCommand::List, VoteCommand::Map, VoteCommand::Restart,
                               VoteCommand::Next, VoteCommand::Yes, VoteCommand::No, VoteCommand::QueryStatus,
                               VoteCommand::Shuffle}) {
        request.command = command;
        request.id = command == VoteCommand::Yes || command == VoteCommand::No ? 0x78563412 : 0;
        request.map = command == VoteCommand::Map ? "E1M2.MAP" : "";
        voteRoundtrip(request, writeVoteRequest, readVoteRequest);
    }
    request.command = VoteCommand::Map; request.id = 0; request.map = "E1M2";
    Writer encoded; CHECK(writeVoteRequest(encoded, request));
    CHECK(encoded.data == std::vector<uint8_t>({2,0,0,0, 0,0,0,0, 4,0,'E','1','M','2'}));
    for (const std::string& map : {std::string(), std::string(256, 'x'), std::string("A\nB"),
                                  std::string("A\0B", 3), std::string("A\x7f")}) {
        request.map = map; voteInvalid(request, writeVoteRequest);
    }
    request.map = std::string(255, 'x'); voteRoundtrip(request, writeVoteRequest, readVoteRequest);
    request.id = 1; voteInvalid(request, writeVoteRequest);
    request.map.clear(); request.command = VoteCommand::Yes; request.id = 0;
    voteInvalid(request, writeVoteRequest);
    for (uint32_t command : {0u, 9u, UINT32_MAX}) {
        request.command = VoteCommand(command); voteInvalid(request, writeVoteRequest);
    }
    const std::pair<unsigned, uint32_t> invalidRequest[] = {{0,0}, {0,9}, {4,1}, {0,8}};
    for (const auto& mutation : invalidRequest) {
        auto bytes = encoded.data; field(bytes, mutation.first, mutation.second);
        Reader reader(bytes.data(), bytes.size());
        request.command = VoteCommand::QueryStatus;
        CHECK(!readVoteRequest(reader, request) && request.command == VoteCommand::QueryStatus);
    }
    request.command = VoteCommand::Shuffle; request.id = 0; request.map.clear();
    Writer shuffle; CHECK(writeVoteRequest(shuffle, request));
    CHECK(shuffle.data == std::vector<uint8_t>({8,0,0,0, 0,0,0,0, 0,0}));
    request.id = 1; voteInvalid(request, writeVoteRequest);
    request.id = 0; request.map = "E1M2"; voteInvalid(request, writeVoteRequest);
    field(shuffle.data, 4, 1);
    Reader invalidShuffle(shuffle.data.data(), shuffle.data.size());
    request.command = VoteCommand::QueryStatus; request.map = "unchanged";
    CHECK(!readVoteRequest(invalidShuffle, request) && request.command == VoteCommand::QueryStatus
          && request.map == "unchanged");

    VoteStatusPayload status;
    voteRoundtrip(status, writeVoteStatus, readVoteStatus);
    status.id = 0x78563412; status.state = VoteState::Active; status.action = 1;
    status.remainingMs = 30000; status.eligible = 3; status.yes = 1; status.no = 1; status.required = 2;
    status.caller = 2; status.canVote = 1; status.cooldownMs = 120000; status.rejected = 1;
    status.target = "E1M2"; status.notice = "Another map vote is already active.";
    voteRoundtrip(status, writeVoteStatus, readVoteStatus);
    for (uint32_t action : {2u, 3u, 4u}) {
        auto other = status; other.action = action;
        other.target = action == 4 ? "Shuffle teams" : "E1M2";
        voteRoundtrip(other, writeVoteStatus, readVoteStatus);
    }
    encoded.data.clear(); CHECK(writeVoteStatus(encoded, status));
    const std::pair<unsigned, uint32_t> invalidStatus[] = {
        {0,0}, {4,5}, {8,0}, {8,5}, {12,30001}, {16,0}, {16,9}, {20,4}, {24,3},
        {28,1}, {28,3}, {32,8}, {36,2}, {40,2}, {44,120001}, {48,2}};
    for (const auto& mutation : invalidStatus) {
        auto bytes = encoded.data; field(bytes, mutation.first, mutation.second);
        Reader reader(bytes.data(), bytes.size());
        VoteStatusPayload ignored; ignored.id = 73;
        CHECK(!readVoteStatus(reader, ignored) && ignored.id == 73);
    }
    for (VoteState state : {VoteState::Passed, VoteState::Failed, VoteState::Cancelled}) {
        status.state = state; status.remainingMs = 0; status.canVote = 0;
        voteRoundtrip(status, writeVoteStatus, readVoteStatus);
        status.remainingMs = 1; voteInvalid(status, writeVoteStatus); status.remainingMs = 0;
        status.canVote = 1; voteInvalid(status, writeVoteStatus); status.canVote = 0;
    }
    status.target = ""; voteInvalid(status, writeVoteStatus);
    status.target = "E1M2"; status.notice = std::string(161, 'x'); voteInvalid(status, writeVoteStatus);
    status = VoteStatusPayload(); status.id = 1; voteInvalid(status, writeVoteStatus);

    VoteOptionsPayload options;
    voteRoundtrip(options, writeVoteOptions, readVoteOptions);
    options.enabled = 1; options.actions = 6; options.listRequested = 1;
    voteRoundtrip(options, writeVoteOptions, readVoteOptions);
    options.actions = 1; options.maps = {"E1M1", "E1M2.MAP"};
    voteRoundtrip(options, writeVoteOptions, readVoteOptions);
    for (uint32_t actions : {8u, 9u, 14u, 15u}) {
        auto other = options; other.actions = actions;
        if (!(actions & 1)) other.maps.clear();
        voteRoundtrip(other, writeVoteOptions, readVoteOptions);
    }
    encoded.data.clear(); CHECK(writeVoteOptions(encoded, options));
    for (const auto& mutation : std::vector<std::pair<unsigned, uint32_t>>{
            {0,0}, {0,2}, {4,0}, {4,8}, {4,16}, {4,17}, {8,2}, {12,257}, {12,UINT32_MAX}}) {
        auto bytes = encoded.data; field(bytes, mutation.first, mutation.second);
        Reader reader(bytes.data(), bytes.size());
        VoteOptionsPayload ignored; ignored.maps = {"unchanged"};
        CHECK(!readVoteOptions(reader, ignored) && ignored.maps == std::vector<std::string>({"unchanged"}));
    }
    options.maps = {""}; voteInvalid(options, writeVoteOptions);
    options.maps = {"E1M1\nE1M2"}; voteInvalid(options, writeVoteOptions);
    options.maps.assign(257, "E1M1"); voteInvalid(options, writeVoteOptions);
    options.maps.assign(256, "E1M1"); voteRoundtrip(options, writeVoteOptions, readVoteOptions);
    std::puts("PASS: map/team-shuffle vote requests, statuses and options; bounds, truncation, control characters and unchanged rejected output");
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
void mapTransfer()
{
    netwire::MapRequestPayload request;
    request.slotGeneration = 0x08070605;
    request.mapCRC = 0x78563412;
    request.offset = netwire::MapChunkBytes;
    netwire::Writer writer;
    CHECK(netwire::writeMapRequest(writer, request));
    CHECK(writer.data == std::vector<uint8_t>({5,6,7,8, 0x12,0x34,0x56,0x78, 0,0x40,0,0}));
    const auto requestBytes = writer.data;
    for (uint32_t offset = 0; offset <= netwire::MaxMapBytes; offset += netwire::MapChunkBytes) {
        request.offset = offset;
        writer = netwire::Writer();
        CHECK(netwire::writeMapRequest(writer, request));
        netwire::Reader reader(writer.data.data(), writer.data.size());
        netwire::MapRequestPayload decoded;
        CHECK(netwire::readMapRequest(reader, decoded) && reader.end());
        CHECK(decoded.slotGeneration == request.slotGeneration && decoded.mapCRC == request.mapCRC && decoded.offset == offset);
    }
    for (size_t length = 0; length < requestBytes.size(); ++length) {
        netwire::Reader reader(requestBytes.data(), length);
        netwire::MapRequestPayload decoded; decoded.slotGeneration = 91;
        CHECK(!netwire::readMapRequest(reader, decoded) && decoded.slotGeneration == 91);
    }
    for (uint32_t offset : {1u, netwire::MapChunkBytes-1, netwire::MaxMapBytes+1,
                            netwire::MaxMapBytes+netwire::MapChunkBytes, UINT32_MAX}) {
        request.offset = offset;
        writer = netwire::Writer(); writer.data = {19};
        CHECK(!netwire::writeMapRequest(writer, request) && writer.data == std::vector<uint8_t>({19}));
        auto invalid = requestBytes; field(invalid, 8, offset);
        netwire::Reader reader(invalid.data(), invalid.size());
        netwire::MapRequestPayload decoded;
        CHECK(!netwire::readMapRequest(reader, decoded));
    }
    auto trailing = requestBytes; trailing.push_back(0);
    netwire::Reader requestTrailing(trailing.data(), trailing.size());
    CHECK(!netwire::readMapRequest(requestTrailing, request));

    std::vector<uint8_t> payload(netwire::MapChunkBytes);
    for (auto &byte : payload) byte = uint8_t(randomValue());
    netwire::MapDataPayload chunk;
    chunk.mapCRC = 0x78563412; chunk.data = payload.data();
    for (uint32_t total : {1u, netwire::MapChunkBytes-1, netwire::MapChunkBytes,
                           netwire::MapChunkBytes+1, 2*netwire::MapChunkBytes-1,
                           netwire::MaxMapBytes-1, netwire::MaxMapBytes}) {
        chunk.totalBytes = total;
        for (uint32_t offset = 0; offset < total; offset += netwire::MapChunkBytes) {
            chunk.offset = offset;
            chunk.length = std::min(netwire::MapChunkBytes, total-offset);
            writer = netwire::Writer();
            CHECK(netwire::writeMapData(writer, chunk));
            CHECK(writer.data.size() == 16+chunk.length);
            netwire::Reader reader(writer.data.data(), writer.data.size());
            netwire::MapDataPayload decoded;
            CHECK(netwire::readMapData(reader, decoded) && reader.end());
            CHECK(decoded.mapCRC == chunk.mapCRC && decoded.totalBytes == total &&
                  decoded.offset == offset && decoded.length == chunk.length);
            CHECK(decoded.data == writer.data.data()+16 &&
                  std::equal(decoded.data, decoded.data+decoded.length, payload.begin()));
        }
    }
    chunk.totalBytes = netwire::MapChunkBytes+1; chunk.offset = 0; chunk.length = netwire::MapChunkBytes;
    writer = netwire::Writer(); CHECK(netwire::writeMapData(writer, chunk));
    const auto fullChunk = writer.data;
    CHECK(std::vector<uint8_t>(fullChunk.begin(), fullChunk.begin()+16) ==
          std::vector<uint8_t>({0x12,0x34,0x56,0x78, 1,0x40,0,0, 0,0,0,0, 0,0x40,0,0}));
    for (size_t length = 0; length < fullChunk.size(); ++length) {
        netwire::Reader reader(fullChunk.data(), length);
        netwire::MapDataPayload decoded; decoded.mapCRC = 91;
        CHECK(!netwire::readMapData(reader, decoded) && decoded.mapCRC == 91 && !decoded.data);
    }
    for (auto invalidField : {std::make_pair(4u, 0u), std::make_pair(4u, netwire::MaxMapBytes+1),
                              std::make_pair(4u, UINT32_MAX), std::make_pair(8u, 1u),
                              std::make_pair(8u, chunk.totalBytes), std::make_pair(8u, UINT32_MAX),
                              std::make_pair(12u, 0u), std::make_pair(12u, netwire::MapChunkBytes-1),
                              std::make_pair(12u, netwire::MapChunkBytes+1), std::make_pair(12u, UINT32_MAX)}) {
        auto invalid = fullChunk; field(invalid, invalidField.first, invalidField.second);
        netwire::Reader reader(invalid.data(), invalid.size());
        netwire::MapDataPayload decoded; decoded.mapCRC = 91;
        CHECK(!netwire::readMapData(reader, decoded) && decoded.mapCRC == 91 && !decoded.data);
    }
    trailing = fullChunk; trailing.push_back(0);
    netwire::Reader chunkTrailing(trailing.data(), trailing.size());
    netwire::MapDataPayload decoded;
    CHECK(!netwire::readMapData(chunkTrailing, decoded));
    // Transfer bytes are opaque. The file's complete CRC is verified after
    // assembly; changing payload bytes is valid wire data, not a framing error.
    auto changed = fullChunk; changed.back() ^= 0xff;
    netwire::Reader changedReader(changed.data(), changed.size());
    CHECK(netwire::readMapData(changedReader, decoded) && decoded.data[decoded.length-1] == changed.back());
    for (unsigned invalid = 0; invalid < 7; ++invalid) {
        auto bad = chunk;
        switch (invalid) {
            case 0: bad.totalBytes = 0; break;
            case 1: bad.totalBytes = netwire::MaxMapBytes+1; break;
            case 2: bad.offset = 1; break;
            case 3: bad.offset = bad.totalBytes; break;
            case 4: bad.length = 0; break;
            case 5: bad.length = netwire::MapChunkBytes+1; break;
            case 6: bad.data = nullptr; break;
        }
        writer = netwire::Writer(); writer.data = {19};
        CHECK(!netwire::writeMapData(writer, bad) && writer.data == std::vector<uint8_t>({19}));
    }
    std::puts("PASS: map requests/chunks, golden bytes, maximum maps, partial final chunks, truncation and bounds");
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
            CHECK(header.type >= netwire::Hello && header.type < netwire::TypeCount);
            CHECK(body.size + netwire::HeaderSize == bytes.size());
        }
        netwire::Reader command(bytes.data(), bytes.size()); netwire::Command ignored;
        netwire::readCommand(command, ignored);
        netwire::Reader request(bytes.data(), bytes.size()); netwire::MapRequestPayload ignoredRequest;
        netwire::readMapRequest(request, ignoredRequest);
        netwire::Reader chunk(bytes.data(), bytes.size()); netwire::MapDataPayload ignoredChunk;
        netwire::readMapData(chunk, ignoredChunk);
        netwire::Reader rules(bytes.data(), bytes.size()); netwire::ServerRules ignoredRules;
        netwire::readServerRules(rules, ignoredRules);
        netwire::Reader voteRequest(bytes.data(), bytes.size()); netwire::VoteRequestPayload ignoredVoteRequest;
        netwire::readVoteRequest(voteRequest, ignoredVoteRequest);
        netwire::Reader voteStatus(bytes.data(), bytes.size()); netwire::VoteStatusPayload ignoredVoteStatus;
        netwire::readVoteStatus(voteStatus, ignoredVoteStatus);
        netwire::Reader voteOptions(bytes.data(), bytes.size()); netwire::VoteOptionsPayload ignoredVoteOptions;
        netwire::readVoteOptions(voteOptions, ignoredVoteOptions);
        std::string error;
        const std::vector<uint8_t> sentinel = {1,2,3}; auto output = sentinel;
        bool accepted = netSnapshotUnpack(bytes, nullptr, output, error);
        CHECK(!accepted && output == sentinel);
    }
    std::puts("PASS: 30000 deterministic malformed wire/command/map/snapshot inputs");
}
}
int main()
{
    framing(); serverRules(); mapVotes(); commands(); mapTransfer(); snapshots(); malformedCorpus();
    std::printf("PASS: network codecs (%u assertions)\n", checks);
}
