// SPDX-License-Identifier: GPL-2.0-or-later
// Stable pre-game compatibility codecs, independent of engine state.
#include "netcompat.h"
#include <cstdio>
#include <cstdlib>

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)
using Bytes = std::vector<uint8_t>;

static void field(Bytes& bytes, size_t offset, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<uint8_t>(value >> (8 * i));
}

static bool gameDecode(const Bytes& bytes)
{
    netwire::Header header;
    netwire::Reader body(nullptr, 0);
    return netwire::decode(bytes.data(), bytes.size(), header, body);
}

static void rejectInfo(const Bytes& bytes)
{
    netcompat::ServerInfo output;
    output.requiredProtocol = 19;
    output.build = "preserved";
    CHECK(!netcompat::decodeServerInfo(bytes.data(), bytes.size(), output));
    CHECK(output.requiredProtocol == 19 && output.build == "preserved");
}

static void rejectReason(const Bytes& bytes)
{
    uint32_t protocol = 19;
    std::string reason = "preserved";
    CHECK(!netcompat::decodeReject(bytes.data(), bytes.size(), protocol, reason));
    CHECK(protocol == 19 && reason == "preserved");
}

static void serverInfo()
{
    netcompat::ServerInfo info;
    info.requiredProtocol = 7;
    info.build = "r1";
    Bytes const golden = {'N','B','V','1', 1,0,0,0, 7,0,0,0, 2,0,'r','1'};
    CHECK(netcompat::encodeServerInfo(info) == golden);
    CHECK(netcompat::isServerInfo(golden.data(), golden.size()));
    CHECK(!gameDecode(golden));
    for (size_t size = 0; size < golden.size(); ++size)
    {
        rejectInfo(Bytes(golden.begin(), golden.begin() + size));
        CHECK(netcompat::isServerInfo(golden.data(), size) == (size >= 4));
    }
    Bytes bad = golden; bad.push_back(0); rejectInfo(bad);
    bad = golden; field(bad, 0, netwire::Magic); rejectInfo(bad);
    CHECK(!netcompat::isServerInfo(bad.data(), bad.size()));
    for (uint32_t schema : {0u, 2u, UINT32_MAX})
    {
        bad = golden; field(bad, 4, schema); rejectInfo(bad);
    }
    bad = golden; field(bad, 8, 0); rejectInfo(bad);
    for (unsigned length : {0u, 1u, 3u, 97u, 255u})
    {
        bad = golden; bad[12] = length; rejectInfo(bad);
    }
    bad = golden; bad[13] = 1; rejectInfo(bad);

    for (uint32_t protocol : {1u, 6u, netwire::Version, netwire::Version + 1, UINT32_MAX})
    for (size_t length : {size_t(0), size_t(1), netcompat::MaxBuild})
    {
        info.requiredProtocol = protocol;
        info.build.assign(length, 'x');
        Bytes const bytes = netcompat::encodeServerInfo(info);
        netcompat::ServerInfo decoded;
        CHECK(netcompat::decodeServerInfo(bytes.data(), bytes.size(), decoded));
        CHECK(decoded.requiredProtocol == protocol && decoded.build == info.build);
        CHECK(netcompat::encodeServerInfo(decoded) == bytes);
    }
    info.build.clear();
    for (unsigned c = 32; c <= 126; ++c) info.build.push_back(static_cast<char>(c));
    auto printable = netcompat::encodeServerInfo(info);
    netcompat::ServerInfo decoded;
    CHECK(netcompat::decodeServerInfo(printable.data(), printable.size(), decoded) && decoded.build == info.build);
    info.requiredProtocol = 0;
    CHECK(netcompat::encodeServerInfo(info).empty());
    info.requiredProtocol = netwire::Version;
    info.build.assign(netcompat::MaxBuild + 1, 'x');
    CHECK(netcompat::encodeServerInfo(info).empty());
    for (unsigned c = 0; c <= 255; ++c)
        if (c < 32 || c > 126)
        {
            info.build = std::string(1, static_cast<char>(c));
            CHECK(netcompat::encodeServerInfo(info).empty());
            bad = golden; bad[14] = c; rejectInfo(bad);
        }
    CHECK(!netcompat::isServerInfo(nullptr, 4));
    CHECK(!netcompat::decodeServerInfo(nullptr, 14, decoded));
}

static void rejection()
{
    Bytes const golden = {
        'N','B','N','1', 6,0,0,0, 9,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,
        8,0,0,0, 6,0,'U','p','d','a','t','e'};
    CHECK(netcompat::encodeReject(6, "Update") == golden);
    CHECK(!gameDecode(golden));
    CHECK(!netcompat::isServerInfo(golden.data(), golden.size()));
    uint32_t protocol = 0;
    std::string reason;
    CHECK(netcompat::decodeReject(golden.data(), golden.size(), protocol, reason));
    CHECK(protocol == 6 && reason == "Update");
    for (size_t size = 0; size < golden.size(); ++size)
        rejectReason(Bytes(golden.begin(), golden.begin() + size));
    Bytes bad = golden; bad.push_back(0); rejectReason(bad);
    bad = golden; field(bad, 0, netcompat::ServerInfoMagic); rejectReason(bad);
    bad = golden; field(bad, 4, 0); rejectReason(bad);
    for (uint32_t type = 0; type <= netwire::TypeCount; ++type)
        if (type != netwire::Reject)
        {
            bad = golden; field(bad, 8, type); rejectReason(bad);
        }
    for (uint32_t length : {0u, 7u, 9u, UINT32_MAX})
    {
        bad = golden; field(bad, 24, length); rejectReason(bad);
    }
    for (unsigned length : {0u, 5u, 7u, 161u, 255u})
    {
        bad = golden; bad[28] = length; rejectReason(bad);
    }
    bad = golden; bad[29] = 1; rejectReason(bad);

    // Established older servers populate session/generation/sequence; these
    // fields do not turn a diagnostic Reject into a gameplay state update.
    bad = golden;
    field(bad, 12, 0x76543210); field(bad, 16, 0xfedcba98); field(bad, 20, UINT32_MAX);
    CHECK(netcompat::decodeReject(bad.data(), bad.size(), protocol, reason));
    CHECK(protocol == 6 && reason == "Update");

    for (uint32_t wireVersion : {1u, 6u, netwire::Version, netwire::Version + 1, UINT32_MAX})
    for (size_t length : {size_t(0), size_t(1), netcompat::MaxReason})
    {
        std::string const text(length, 'x');
        Bytes const bytes = netcompat::encodeReject(wireVersion, text);
        CHECK(netcompat::decodeReject(bytes.data(), bytes.size(), protocol, reason));
        CHECK(protocol == wireVersion && reason == text);
        CHECK(netcompat::encodeReject(protocol, reason) == bytes);
        CHECK(gameDecode(bytes) == (wireVersion == netwire::Version));
    }
    CHECK(netcompat::encodeReject(0, "Update").empty());
    CHECK(netcompat::encodeReject(7, std::string(netcompat::MaxReason + 1, 'x')).empty());
    for (unsigned c = 0; c <= 255; ++c)
        if (c < 32 || c > 126)
        {
            CHECK(netcompat::encodeReject(7, std::string(1, static_cast<char>(c))).empty());
            bad = golden; bad[30] = c; rejectReason(bad);
        }
    CHECK(!netcompat::decodeReject(nullptr, netwire::HeaderSize + 2, protocol, reason));

    // Cross-version diagnostic handling must never weaken gameplay decoding.
    for (uint32_t type : {uint32_t(netwire::Hello), uint32_t(netwire::Welcome),
                          uint32_t(netwire::Commands), uint32_t(netwire::Snapshot)})
    for (uint32_t wireVersion : {netwire::Version - 1, netwire::Version + 1})
    {
        netwire::Header header;
        header.type = type;
        Bytes packet = netwire::encode(header, {1, 2, 3});
        field(packet, 4, wireVersion);
        CHECK(!gameDecode(packet));
        rejectReason(packet);
        rejectInfo(packet);
    }
}

int main()
{
    serverInfo();
    rejection();
    std::printf("PASS: %u compatibility codec checks (stable golden bytes, bounds, atomic rejection and strict gameplay versions)\n", checks);
}
