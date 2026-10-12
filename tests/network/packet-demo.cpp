#include "netdemo.h"
#include "netprotocol.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>

namespace {
unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)
void write(const std::string& path, const std::vector<uint8_t>& bytes)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    CHECK(bool(file)); file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size()); CHECK(bool(file));
}
bool consume(const std::string& path)
{
    NetDemoInfo info; std::string error;
    if (!netDemoStartPlayback(path.c_str(), info, error)) return false;
    std::vector<uint8_t> packet; uint32_t tick = 0;
    for (unsigned i = 0; i != 1000 && netDemoIsPlaying(); ++i)
        netDemoNextPacket(std::numeric_limits<uint64_t>::max(), packet, tick);
    bool result = !netDemoIsPlaying() && netDemoLastError().empty();
    netDemoStopPlayback(); return result;
}
}
int main(int argc, char** argv)
{
    CHECK(argc == 2);
    const std::string path = std::string(argv[1])+"/roundtrip.nbpd";
    const std::string mutated = std::string(argv[1])+"/mutated.nbpd";
    NetDemoInfo source; source.protocolVersion = netwire::Version;
    source.mapCrc = 0x10203040; source.contentCrc = 0xfedcba98;
    source.localSlot = 7; source.mapName = "E1M1.MAP";
    std::string error;
    netwire::Header header; header.type = netwire::Welcome;
    const auto welcome = netwire::encode(header, {1,2,3});
    header.type = netwire::Snapshot;
    const auto first = netwire::encode(header, {19,0,5,7,255});
    header.sequence = 2; const auto second = netwire::encode(header, {8,9,10});
    CHECK(netDemoStartRecording(path.c_str(), source, error) && netDemoIsRecording());
    NetDemoInfo info;
    CHECK(!netDemoStartPlayback(path.c_str(), info, error));
    CHECK(!netDemoStartRecording(path.c_str(), source, error));
    CHECK(netDemoRecordPacket(0xfffffffeu, welcome));
    CHECK(netDemoRecordPacket(0xfffffffeu, first));
    CHECK(netDemoRecordPacket(0xffffffffu, second));
    CHECK(netDemoRecordPacket(1, first)); // Clock wraps, three ticks after start.
    netDemoStopRecording(); CHECK(!netDemoIsRecording() && netDemoLastError().empty());
    CHECK(netDemoStartPlayback(path.c_str(), info, error));
    CHECK(info.protocolVersion == source.protocolVersion && info.mapCrc == source.mapCrc &&
          info.contentCrc == source.contentCrc && info.localSlot == source.localSlot &&
          info.mapName == source.mapName && info.tickRate == 30);
    std::vector<uint8_t> packet; uint32_t tick = 0;
    CHECK(netDemoNextPacket(0, packet, tick) && packet == welcome && tick == 0xfffffffeu);
    CHECK(netDemoNextPacket(0, packet, tick) && packet == first && tick == 0xfffffffeu);
    CHECK(!netDemoNextPacket(33, packet, tick) && netDemoIsPlaying() && netDemoLastError().empty());
    CHECK(netDemoNextPacket(34, packet, tick) && packet == second && tick == 0xffffffffu);
    CHECK(!netDemoNextPacket(99, packet, tick) && netDemoIsPlaying());
    CHECK(netDemoNextPacket(100, packet, tick) && packet == first && tick == 1);
    CHECK(!netDemoNextPacket(100, packet, tick));
    CHECK(!netDemoIsPlaying() && netDemoLastError().empty());
    CHECK(consume(path));
    std::ifstream file(path, std::ios::binary);
    const std::vector<uint8_t> original((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    CHECK(original.size() > 200 && original[0] == 'N' && original[7] == 0x1a);
    for (size_t size = 0; size < original.size(); ++size) {
        write(mutated, std::vector<uint8_t>(original.begin(), original.begin()+size));
        CHECK(!consume(mutated) && !netDemoLastError().empty());
    }
    for (size_t offset = 0; offset < original.size(); ++offset) {
        auto bytes = original; bytes[offset] ^= 1;
        write(mutated, bytes); CHECK(!consume(mutated) && !netDemoLastError().empty());
    }
    auto bytes = original; bytes.push_back(0); write(mutated, bytes); CHECK(!consume(mutated));
    CHECK(netDemoStartRecording(mutated.c_str(), source, error));
    CHECK(netDemoRecordPacket(100, welcome));
    CHECK(netDemoRecordPacket(103, first));
    CHECK(netDemoRecordPacket(0, welcome, true));
    CHECK(netDemoRecordPacket(2, second));
    netDemoStopRecording();
    CHECK(netDemoStartPlayback(mutated.c_str(), info, error));
    CHECK(netDemoNextPacket(0, packet, tick) && packet == welcome && tick == 100);
    CHECK(netDemoNextPacket(100, packet, tick) && packet == first && tick == 103);
    CHECK(netDemoNextPacket(100, packet, tick) && packet == welcome && tick == 0);
    CHECK(!netDemoNextPacket(166, packet, tick) && netDemoIsPlaying());
    CHECK(netDemoNextPacket(167, packet, tick) && packet == second && tick == 2);
    CHECK(!netDemoNextPacket(167, packet, tick) && !netDemoIsPlaying() && netDemoLastError().empty());
    CHECK(netDemoStartRecording(mutated.c_str(), source, error));
    CHECK(netDemoRecordPacket(100, welcome));
    CHECK(!netDemoRecordPacket(99, first) && !netDemoIsRecording() && !netDemoLastError().empty());
    CHECK(!consume(mutated));
    CHECK(netDemoStartRecording(mutated.c_str(), source, error));
    CHECK(!netDemoRecordPacket(1, {}) && !netDemoIsRecording());
    CHECK(netDemoStartRecording(mutated.c_str(), source, error));
    CHECK(!netDemoRecordPacket(1, std::vector<uint8_t>(kNetDemoMaxPacketBytes+1)) && !netDemoIsRecording());
    for (int slot : {-2, 8}) { info = source; info.localSlot = slot; CHECK(!netDemoStartRecording(mutated.c_str(), info, error)); }
    info = source; info.mapName = ""; CHECK(!netDemoStartRecording(mutated.c_str(), info, error));
    info = source; info.mapName = std::string("bad\0map", 7); CHECK(!netDemoStartRecording(mutated.c_str(), info, error));
    info = source; info.tickRate = 1001; CHECK(!netDemoStartRecording(mutated.c_str(), info, error));
    std::printf("PASS: packet demo exact roundtrip/pacing/tick-wrap, exclusive state, every-byte truncation/corruption, bounds (%u assertions)\n", checks);
}
