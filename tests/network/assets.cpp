// SPDX-License-Identifier: GPL-2.0-or-later
#include "netassets.h"
#include <cstdlib>
#include <cstdio>

static unsigned checks;
#define CHECK(value) do { ++checks; if (!(value)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #value); std::exit(1); } } while (0)

static void testMD5()
{
    struct Vector { const char *input, *expected; };
    // RFC 1321 appendix A.5, independent published digests.
    const Vector vectors[] = {
        {"", "d41d8cd98f00b204e9800998ecf8427e"},
        {"a", "0cc175b9c0f1b6a831c399e269772661"},
        {"abc", "900150983cd24fb0d6963f7d28e17f72"},
        {"message digest", "f96b697d7cb7938d525a2f31aaf161d0"},
        {"abcdefghijklmnopqrstuvwxyz", "c3fcd3d76192e4007dfb496cca67e13b"},
        {"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789", "d174ab98d277d9f5a5611c2c9f419d9f"},
        {"12345678901234567890123456789012345678901234567890123456789012345678901234567890", "57edf4a22be3c955ac49da2e2107b67a"}
    };
    for (const auto &vector : vectors)
    {
        netassets::MD5 digest;
        digest.update(vector.input, std::strlen(vector.input));
        CHECK(netassets::hex(digest.finish()) == vector.expected);
        CHECK(netassets::hex(digest.finish()) == vector.expected);
        digest.update(nullptr, 0);
        CHECK(netassets::hex(digest.finish()) == vector.expected);
        netassets::MD5 incremental;
        for (const char *p = vector.input; *p; ++p)
            incremental.update(p, 1);
        CHECK(incremental.finish() == digest.finish());
    }
    netassets::MD5 continuing;
    continuing.update("a", 1);
    CHECK(netassets::hex(continuing.finish()) == vectors[1].expected);
    continuing.update("bc", 2);
    CHECK(netassets::hex(continuing.finish()) == vectors[2].expected);

    // Binary data crossing every padding/chunk boundary; golden digests generated
    // independently with Python hashlib/OpenSSL, not this implementation.
    struct Boundary { size_t length; const char *expected; };
    const Boundary boundaries[] = {
        {0, "d41d8cd98f00b204e9800998ecf8427e"},
        {1, "13c8ffd977013703a701cf8e11deac65"},
        {55, "d872aa0473a24da995ce4ac518ade767"},
        {56, "e23567645846677c205de80f9779081b"},
        {57, "1fe5e4a27e12484d7203d0b60c21c51f"},
        {63, "4775b66278a8fc132ff80923378216cd"},
        {64, "71e123b70c7aa64826fcfe472694cd1c"},
        {65, "5949948f26e35203661075214faa3966"},
        {127, "520bdc2dbab7c25d64263ffb242d9e98"},
        {128, "3e93b378458b77da96b2357c3bda8cc2"},
        {129, "d1ae06bbf9128955a34bedb6231eee62"},
        {1024, "d4883a545d61557ce12ff4e95350e997"},
        {4097, "82b1834165b5032bacc5f34d594f6a36"}
    };
    for (const auto &boundary : boundaries)
    {
        std::vector<uint8_t> data(boundary.length);
        for (size_t i = 0; i < data.size(); ++i)
            data[i] = uint8_t(i * 37 + 11);
        for (size_t chunk : {size_t(1), size_t(3), size_t(55), size_t(64), size_t(67), size_t(1024), size_t(4097)})
        {
            netassets::MD5 digest;
            for (size_t i = 0; i < data.size(); i += chunk)
                digest.update(data.data() + i, std::min(chunk, data.size() - i));
            CHECK(netassets::hex(digest.finish()) == boundary.expected);
        }
    }
    netassets::MD5 million;
    std::string block(1000, 'a');
    for (unsigned i = 0; i < 1000; ++i)
        million.update(block.data(), block.size());
    CHECK(netassets::hex(million.finish()) == "7707d6ae4e027c70eea2a935c2296f21");
}

static netassets::Asset asset(const std::string &key, const std::string &name, uint64_t size,
                              const std::string &contents)
{
    netassets::Asset result;
    result.key = key;
    result.name = name;
    result.size = size;
    netassets::MD5 digest;
    digest.update(contents.data(), contents.size());
    result.md5 = digest.finish();
    return result;
}

static bool equal(const netassets::Asset &a, const netassets::Asset &b)
{
    return a.key == b.key && a.name == b.name && a.size == b.size && a.md5 == b.md5;
}

static std::vector<uint8_t> rawManifest(const std::vector<netassets::Asset> &assets)
{
    netwire::Writer writer;
    writer.u32(netassets::ManifestVersion);
    writer.u32(uint32_t(assets.size()));
    for (const auto &entry : assets)
    {
        writer.text(entry.key, 65535);
        writer.text(entry.name, 65535);
        writer.u32(uint32_t(entry.size));
        writer.u32(uint32_t(entry.size >> 32));
        writer.bytes(entry.md5.data(), entry.md5.size());
    }
    return writer.data;
}

static void rejected(const std::vector<uint8_t> &bytes)
{
    const auto sentinel = asset("existing", "existing.rff", 123, "unchanged");
    std::vector<netassets::Asset> decoded{sentinel};
    std::string error;
    netwire::Reader reader(bytes.data(), bytes.size());
    CHECK(!netassets::decodeManifest(reader, decoded, error));
    CHECK(!error.empty());
    CHECK(decoded.size() == 1 && equal(decoded.front(), sentinel));
}

static void rejected(const std::vector<netassets::Asset> &assets)
{
    std::vector<uint8_t> bytes{1,2,3};
    std::string error;
    CHECK(!netassets::encodeManifest(assets, bytes, error));
    CHECK(!error.empty());
    CHECK(bytes == std::vector<uint8_t>({1,2,3}));
    rejected(rawManifest(assets));
}

static void testManifest()
{
    using netassets::Asset;
    std::vector<Asset> required{
        asset("rff:system", "mod.rff", 4294967301ull, "archive contents"),
        asset("art:0", "tiles000.art", UINT64_MAX, "art contents"),
        asset("definition:test", "caf\xc3\xa9.def", 0, "")
    };
    std::vector<uint8_t> bytes;
    std::string error = "previous error";
    CHECK(netassets::encodeManifest(required, bytes, error));
    CHECK(error.empty());
    std::vector<Asset> decoded;
    netwire::Reader reader(bytes.data(), bytes.size());
    CHECK(netassets::decodeManifest(reader, decoded, error));
    CHECK(reader.end() && error.empty() && decoded.size() == required.size());
    for (size_t i = 0; i < required.size(); ++i)
        CHECK(equal(required[i], decoded[i]));
    CHECK(bytes == rawManifest(required));
    // No success or output replacement on a single truncated field.
    for (size_t size = 0; size < bytes.size(); ++size)
        rejected(std::vector<uint8_t>(bytes.begin(), bytes.begin() + size));
    auto malformed = bytes;
    malformed.push_back(0);
    rejected(malformed);
    malformed = bytes;
    malformed[0] = uint8_t(netassets::ManifestVersion + 1);
    rejected(malformed);
    malformed = bytes;
    for (size_t i = 4; i < 8; ++i)
        malformed[i] = 255;
    rejected(malformed);
    malformed = bytes;
    malformed[8] = malformed[9] = 255;
    rejected(malformed);
    rejected(std::vector<uint8_t>(netassets::MaxManifestBytes + 1, 0));
    // Atomic encoding and decoding on all invalid text and duplicate identities.
    auto invalid = required;
    invalid.push_back(required.front());
    rejected(invalid);
    for (const std::string &bad : {std::string(), std::string("line\nend"), std::string("nul\0end", 7),
                                  std::string("\x1b"), std::string("\x7f")})
    {
        invalid = required;
        invalid.front().key = bad;
        rejected(invalid);
        invalid = required;
        invalid.front().name = bad;
        rejected(invalid);
    }
    invalid = required;
    invalid.front().key.assign(netassets::MaxAssetKey + 1, 'k');
    rejected(invalid);
    invalid = required;
    invalid.front().name.assign(netassets::MaxAssetName + 1, 'n');
    rejected(invalid);

    std::vector<Asset> maximum;
    for (size_t i = 0; i < netassets::MaxAssets; ++i)
        maximum.push_back(asset("resource:" + std::to_string(i), "empty.qav", 0, ""));
    CHECK(netassets::encodeManifest(maximum, malformed, error));
    netwire::Reader maxReader(malformed.data(), malformed.size());
    CHECK(netassets::decodeManifest(maxReader, decoded, error));
    CHECK(decoded.size() == netassets::MaxAssets);
    maximum.push_back(asset("one-too-many", "extra.qav", 0, ""));
    rejected(maximum);
    maximum.pop_back();
    for (auto &entry : maximum)
    {
        entry.key.append(netassets::MaxAssetKey - entry.key.size(), 'k');
        entry.name.assign(netassets::MaxAssetName, 'n');
    }
    rejected(maximum); // Count valid, aggregate bound exceeded.
    maximum.resize(1);
    CHECK(netassets::encodeManifest(maximum, malformed, error));
    netwire::Reader longestReader(malformed.data(), malformed.size());
    CHECK(netassets::decodeManifest(longestReader, decoded, error));
    CHECK(equal(decoded.front(), maximum.front()));

    CHECK(netassets::encodeManifest({}, malformed, error));
    netwire::Reader emptyReader(malformed.data(), malformed.size());
    CHECK(netassets::decodeManifest(emptyReader, decoded, error) && decoded.empty());

    // Mutate trusted-size packets deeply; accepted inputs must remain canonical.
    uint32_t random = 0x870f1023u;
    for (unsigned iteration = 0; iteration < 4000; ++iteration)
    {
        malformed = bytes;
        random = random * 1664525u + 1013904223u;
        malformed[random % malformed.size()] ^= uint8_t(1u << (random >> 29));
        netwire::Reader mutationReader(malformed.data(), malformed.size());
        const bool valid = netassets::decodeManifest(mutationReader, decoded, error);
        CHECK(valid ? error.empty() : !error.empty());
        if (valid)
        {
            std::vector<uint8_t> canonical;
            CHECK(netassets::encodeManifest(decoded, canonical, error));
            CHECK(canonical == malformed);
        }
    }

    CHECK(netassets::compareManifest(required, required).empty());
    auto local = required;
    // Installation filename differences do not change semantic resource identity.
    local.front().name = "renamed.rff";
    std::reverse(local.begin(), local.end());
    CHECK(netassets::compareManifest(required, local).empty());
    local.push_back(asset("optional", "cosmetic.def", 0, ""));
    CHECK(netassets::compareManifest(required, local).empty());
    auto issues = netassets::compareManifest(required, {});
    CHECK(issues.size() == required.size());
    for (size_t i = 0; i < issues.size(); ++i)
        CHECK(issues[i].kind == netassets::Missing && equal(issues[i].required, required[i]));
    local = required;
    local.front().md5[15] ^= 1;
    issues = netassets::compareManifest(required, local);
    CHECK(issues.size() == 1 && issues.front().kind == netassets::Mismatch);
    CHECK(equal(issues.front().required, required.front()) && equal(issues.front().local, local.front()));
    local = required;
    ++local.front().size;
    issues = netassets::compareManifest(required, local);
    CHECK(issues.size() == 1 && issues.front().kind == netassets::Mismatch);
    local = required;
    local.front().key = "unrelated-file-with-identical-bytes";
    issues = netassets::compareManifest(required, local);
    CHECK(issues.size() == 1 && issues.front().kind == netassets::Missing);
}

int main(int argc, char **argv)
{
    testMD5();
    testManifest();
    if (argc > 1 && std::strcmp(argv[1], "--large") == 0)
    {
        // Real streaming past the 32-bit byte counter, bounded memory. Independent
        // reference: hashlib.md5(4 GiB of zeroes + b'length-over-32bit').
        netassets::MD5 digest;
        const std::vector<uint8_t> chunk(1024 * 1024, 0);
        for (unsigned i = 0; i < 4096; ++i)
            digest.update(chunk.data(), chunk.size());
        const char suffix[] = "length-over-32bit";
        digest.update(suffix, sizeof(suffix) - 1);
        CHECK(netassets::hex(digest.finish()) == "96208469a9d7061bdf70d377ff7ca50e");
    }
    std::printf("PASS asset manifest and MD5: %u checks\n", checks);
    return 0;
}
