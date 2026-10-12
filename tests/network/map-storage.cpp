// Exercise downloaded map validation and cache publication using real BLM bytes.
#include "netmap.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>
#ifdef _WIN32
# include <windows.h>
# include <direct.h>
#endif

namespace {
unsigned checks;
void check(bool condition, const char *expression, int line)
{
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, expression);
        std::exit(1);
    }
}
#define CHECK(x) check(bool(x), #x, __LINE__)
using Bytes = std::vector<uint8_t>;
void u16(Bytes& data, size_t at, unsigned value)
{
    data.at(at) = uint8_t(value); data.at(at+1) = uint8_t(value >> 8);
}
void u32(Bytes& data, size_t at, uint32_t value)
{
    for (unsigned n = 0; n < 4; ++n) data.at(at+n) = uint8_t(value >> (n*8));
}
uint32_t crc(const uint8_t *data, size_t length)
{
    uint32_t result = ~uint32_t(0);
    while (length--) {
        result ^= *data++;
        for (unsigned n = 0; n < 8; ++n)
            result = (result >> 1) ^ (0xedb88320u & uint32_t(-int32_t(result & 1)));
    }
    return ~result;
}
uint32_t fixCRC(Bytes& data)
{
    CHECK(data.size() >= 4);
    uint32_t value = crc(data.data(), data.size()-4);
    u32(data, data.size()-4, value);
    return value;
}
void crypt(Bytes& data, size_t at, size_t length, unsigned key)
{
    for (size_t n = 0; n < length; ++n) data.at(at+n) ^= uint8_t(key+n);
}
struct Map {
    Bytes data;
    size_t sector, wall, sprite;
    bool modern;
};
Map fixture(bool modern, bool extras = false, bool encryptedHeader = false)
{
    Map result; result.modern = modern;
    Bytes& b = result.data;
    b.resize(6+37+(modern ? 128 : 0)+2, 0);
    b[0] = 'B'; b[1] = 'L'; b[2] = 'M'; b[3] = 0x1a;
    u16(b, 4, modern ? 0x700 : 0x600);
    u16(b, 6+0x1f, 1); u16(b, 6+0x21, 4); u16(b, 6+0x23, 1);
    if (modern) {
        u32(b, 43+64, 56); u32(b, 43+68, 24); u32(b, 43+72, 60);
        crypt(b, 43, 128, 4);
        crypt(b, 171, 2, 2);
    }
    if (encryptedHeader) crypt(b, 6, 37, 0x7474614d);
    result.sector = b.size(); b.resize(b.size()+40, 0);
    u16(b, result.sector+2, 4);
    u32(b, result.sector+4, uint32_t(-4096));
    u32(b, result.sector+8, 4096);
    u16(b, result.sector+38, extras ? 1 : 0xffff);
    if (modern) crypt(b, result.sector, 40, 0);
    if (extras) b.resize(b.size()+60, 0);
    result.wall = b.size();
    for (unsigned n = 0; n < 4; ++n) {
        size_t at = b.size(); b.resize(at+32, 0);
        static const int coords[][2] = {{0,0},{1024,0},{1024,1024},{0,1024}};
        u32(b, at, coords[n][0]); u32(b, at+4, coords[n][1]);
        u16(b, at+8, (n+1)%4); u16(b, at+10, 0xffff); u16(b, at+12, 0xffff);
        u16(b, at+30, extras && n == 0 ? 1 : 0xffff);
        if (modern) crypt(b, at, 32, 0x7474614d);
        if (extras && n == 0) b.resize(b.size()+24, 0);
    }
    result.sprite = b.size(); b.resize(b.size()+44, 0);
    u32(b, result.sprite, 512); u32(b, result.sprite+4, 512);
    u16(b, result.sprite+30, 0xffff);
    u16(b, result.sprite+42, extras ? 1 : 0xffff);
    if (modern) crypt(b, result.sprite, 44, 0x7474614d);
    if (extras) b.resize(b.size()+56, 0);
    b.resize(b.size()+4); fixCRC(b);
    return result;
}
uint32_t mapCRC(const Bytes& b)
{
    return crc(b.data(), b.size()-4);
}
bool validates(const Bytes& b, uint32_t expected)
{
    std::string error;
    bool accepted = netMapValidate(b, expected, error);
    CHECK(accepted || !error.empty());
    return accepted;
}
void malformed(Bytes b, size_t at, unsigned value, unsigned width = 2)
{
    if (width == 2) u16(b, at, value); else u32(b, at, value);
    const uint32_t expected = fixCRC(b);
    CHECK(!validates(b, expected));
}
// Base records are encrypted in BLM7; their extended records are always plain.
void baseField(Map& map, size_t start, size_t length, unsigned key, size_t field, unsigned value)
{
    if (map.modern) crypt(map.data, start, length, key);
    u16(map.data, start+field, value);
    if (map.modern) crypt(map.data, start, length, key);
}
void extendedReferences()
{
    for (bool modern : {false, true}) {
        const Map normal = fixture(modern, true);
        for (size_t marker : {normal.sector+40+44, normal.sector+40+46}) {
            for (unsigned valid : {0xffffu, 16383u}) {
                Bytes data = normal.data; u16(data, marker, valid);
                CHECK(validates(data, fixCRC(data)));
            }
            for (unsigned invalid : {16384u, 32767u, 65534u})
                malformed(normal.data, marker, invalid);
        }
        for (size_t reference : {normal.sprite+44+30, normal.sprite+44+46}) {
            for (unsigned valid : {0xffffu, 16383u}) {
                Bytes data = normal.data; u16(data, reference, valid);
                CHECK(validates(data, fixCRC(data)));
            }
            for (unsigned invalid : {16384u, 32767u, 65534u})
                malformed(normal.data, reference, invalid);
        }
        for (unsigned type : {614u, 615u, 616u, 617u}) {
            Map moving = normal;
            baseField(moving, moving.sector, 40, 0, 34, type);
            CHECK(validates(moving.data, fixCRC(moving.data)));
            Map missing = moving;
            u16(missing.data, missing.sector+40+44, 0xffff);
            CHECK(!validates(missing.data, fixCRC(missing.data)));
            Map pastEnd = moving;
            u16(pastEnd.data, pastEnd.sector+40+44, 1);
            CHECK(!validates(pastEnd.data, fixCRC(pastEnd.data)));
            for (unsigned markerType : {3u, 5u, 8u}) {
                Map propagated = missing;
                baseField(propagated, propagated.sprite, 44, 0x7474614d, 26, 10);
                baseField(propagated, propagated.sprite, 44, 0x7474614d, 30, 0);
                baseField(propagated, propagated.sprite, 44, 0x7474614d, 38, markerType);
                CHECK(validates(propagated.data, fixCRC(propagated.data)));
                Map wrongOwner = propagated;
                baseField(wrongOwner, wrongOwner.sprite, 44, 0x7474614d, 30, 1);
                CHECK(!validates(wrongOwner.data, fixCRC(wrongOwner.data)));
                Map wrongStatus = propagated;
                baseField(wrongStatus, wrongStatus.sprite, 44, 0x7474614d, 26, 0);
                CHECK(!validates(wrongStatus.data, fixCRC(wrongStatus.data)));
                Map wrongType = propagated;
                baseField(wrongType, wrongType.sprite, 44, 0x7474614d, 38, 2);
                CHECK(!validates(wrongType.data, fixCRC(wrongType.data)));
            }
            Map second = moving;
            u16(second.data, second.sector+40+46, 0xffff);
            CHECK(validates(second.data, fixCRC(second.data)) == (type == 615 || type == 617));
            baseField(second, second.sprite, 44, 0x7474614d, 26, 10);
            baseField(second, second.sprite, 44, 0x7474614d, 30, 0);
            baseField(second, second.sprite, 44, 0x7474614d, 38, 4);
            CHECK(validates(second.data, fixCRC(second.data)));
        }
    }
    std::puts("PASS: extended sprite/sector references and moving-sector marker propagation");
}
void validation()
{
    CHECK(crc(reinterpret_cast<const uint8_t *>("123456789"), 9) == 0xcbf43926u);
    CHECK(netmap::MaxBytes == 16u*1024u*1024u);
    for (bool modern : {false, true}) for (bool extras : {false, true}) {
        const Map m = fixture(modern, extras);
        const uint32_t expected = mapCRC(m.data);
        CHECK(validates(m.data, expected));
        CHECK(!validates(m.data, expected ^ 1));
        for (size_t length = 0; length < m.data.size(); ++length) {
            CHECK(!validates(Bytes(m.data.begin(), m.data.begin()+length), expected));
            if (length >= 4) {
                Bytes structurallyShort(m.data.begin(), m.data.begin()+length);
                CHECK(!validates(structurallyShort, fixCRC(structurallyShort)));
            }
        }
        for (size_t at = 0; at < m.data.size(); ++at) {
            Bytes broken = m.data; broken[at] ^= 0x80;
            CHECK(!validates(broken, expected));
        }
        Bytes trailing = m.data; trailing.insert(trailing.end()-4, 0);
        CHECK(!validates(trailing, fixCRC(trailing)));
    }
    CHECK(validates(fixture(true, true, true).data, mapCRC(fixture(true, true, true).data)));
    const Map old = fixture(false);
    malformed(old.data, 4, 0x800);
    malformed(old.data, 6+0x0e, 1); // Starting sector outside map.
    for (unsigned sky : {9u, 31u, 0xffffu}) malformed(old.data, 6+0x10, sky);
    for (unsigned count : {0u, 0x7fffu, 0xffffu}) {
        malformed(old.data, 6+0x1f, count);
        malformed(old.data, 6+0x21, count);
    }
    malformed(old.data, 6+0x23, 0x7fff);
    malformed(old.data, old.sector, 4);
    malformed(old.data, old.sector+2, 5);
    malformed(old.data, old.wall+8, 4);
    malformed(old.data, old.wall+10, 4);
    malformed(old.data, old.wall+12, 1);
    malformed(old.data, old.sprite+24, 1);
    malformed(old.data, old.sprite+26, 1024);
    malformed(old.data, old.sprite+26, 0xffff);
    for (size_t tile : {old.sector+16, old.sector+24, old.wall+16, old.sprite+14}) {
        malformed(old.data, tile, 0xffff);
        malformed(old.data, tile, 0x7fff);
    }
    // E6M7 uses -1 to mean no overlay on ordinary unmasked walls.
    Bytes noOverlay = old.data;
    u16(noOverlay, old.wall+18, 0xffff);
    CHECK(validates(noOverlay, fixCRC(noOverlay)));
    for (unsigned flags : {16u, 32u, 48u})
        malformed(noOverlay, old.wall+14, flags);
    malformed(old.data, old.wall+18, 0xfffe);
    malformed(old.data, old.wall+18, 0x7fff);
    // E6M7 contains a two-wall sector; E3M5 has an asymmetric portal.
    // These shipped topologies remain loadable while all references stay bounded.
    Map degenerate = old;
    degenerate.data.erase(degenerate.data.begin()+old.wall+64, degenerate.data.begin()+old.wall+128);
    degenerate.sprite -= 64;
    u16(degenerate.data, 6+0x21, 2);
    u16(degenerate.data, degenerate.sector+2, 2);
    u16(degenerate.data, degenerate.wall+32+8, 0);
    CHECK(validates(degenerate.data, fixCRC(degenerate.data)));
    Bytes asymmetric = old.data;
    u16(asymmetric, old.wall+10, 1); u16(asymmetric, old.wall+12, 0);
    CHECK(validates(asymmetric, fixCRC(asymmetric)));
    const Map extended = fixture(true, true);
    for (auto size : {std::pair<size_t, uint32_t>{64,56}, {68,24}, {72,60}}) {
        for (uint32_t invalid : {0u, size.second+1, 0xffffffffu}) {
            Bytes bad = extended.data;
            crypt(bad, 43, 128, 4); u32(bad, 43+size.first, invalid); crypt(bad, 43, 128, 4);
            CHECK(!validates(bad, fixCRC(bad)));
        }
    }
    Bytes large(netmap::MaxBytes+1); CHECK(!validates(large, 0));
    uint32_t random = 0x96fe163au;
    for (unsigned run = 0; run < 2500; ++run) {
        Bytes mutated = (run & 1) ? extended.data : old.data;
        for (unsigned n = 0; n < 3; ++n) {
            random ^= random << 13; random ^= random >> 17; random ^= random << 5;
            mutated[random % (mutated.size()-4)] ^= uint8_t(random >> 24);
        }
        uint32_t expected = fixCRC(mutated);
        (void)validates(mutated, expected); // Deep structural parser fuzz; any verdict is valid.
    }
    std::puts("PASS: BLM6/7 fixtures, encrypted headers, extended records, every truncation and byte corruption, structural bounds and mutations");
}
Bytes read(const char *path)
{
    std::ifstream input(path, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}
void write(const char *path, const Bytes& data)
{
    std::ofstream output(path, std::ios::binary); CHECK(bool(output));
    output.write(reinterpret_cast<const char *>(data.data()), data.size()); CHECK(bool(output));
}
std::string resolved(const char *name)
{
    const char *path = netMapResolve(name);
    return path ? path : name;
}
bool makeLink(const char *target, const char *path, bool directory)
{
#ifdef _WIN32
    DWORD flags = directory ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0;
    // Unprivileged links require developer mode on recent native Windows.
    const bool created = CreateSymbolicLinkA(path, target, flags | 2) || CreateSymbolicLinkA(path, target, flags);
    if (created) {
        const DWORD attributes = GetFileAttributesA(path);
        // Wine 10's stub returns success without creating any link. Report
        // this platform limit instead of counting an unexercised guard.
        if (attributes == INVALID_FILE_ATTRIBUTES &&
            GetProcAddress(GetModuleHandleA("ntdll.dll"), "wine_get_version")) {
            std::puts("SKIP: Wine CreateSymbolicLinkA stub did not create a link");
            return false;
        }
        CHECK(attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT));
        return true;
    }
    const DWORD error = GetLastError();
    if (error == ERROR_PRIVILEGE_NOT_HELD || error == ERROR_CALL_NOT_IMPLEMENTED || error == ERROR_NOT_SUPPORTED) {
        std::printf("SKIP: symbolic link creation unavailable (Win32 error %lu)\n", (unsigned long)error);
        return false;
    }
    CHECK(false);
    return false;
#else
    (void)directory;
    CHECK(symlink(target, path) == 0);
    return true;
#endif
}
void removeLink(const char *path, bool directory)
{
#ifdef _WIN32
    CHECK(directory ? RemoveDirectoryA(path) : DeleteFileA(path));
#else
    (void)directory;
    CHECK(unlink(path) == 0);
#endif
}
void cache()
{
    const Map map = fixture(true, true);
    const uint32_t expected = mapCRC(map.data);
    std::string error;
    const Bytes sentinel = {'p','r','e','s','e','r','v','e'};
    write("installed.map", sentinel);
    CHECK(!netMapPrepare("installed.map", expected, error));
    CHECK(netMapInstall("installed.map", expected, map.data, error));
    const std::string cached = resolved("installed.map");
    const std::string cachedFile = cached+".MAP";
    CHECK(cached != "installed.map");
    CHECK(cached.find(".nblood-map-cache/") != std::string::npos);
    CHECK(cached.find("..") == std::string::npos);
    CHECK(read(cachedFile.c_str()) == map.data);
    CHECK(read("installed.map") == sentinel);
    CHECK(resolved("unrelated.map") == "unrelated.map");
    CHECK(resolved("INSTALLED.MAP") == cached);
    CHECK(resolved("installed") == cached);
    netMapClear(); CHECK(resolved("installed.map") == "installed.map");
    CHECK(netMapPrepare("installed.map", expected, error));
    CHECK(resolved("installed.map") == cached);
    netMapClear();
    write(cachedFile.c_str(), sentinel);
    CHECK(!netMapPrepare("installed.map", expected, error));
    CHECK(netMapInstall("installed.map", expected, map.data, error));
    CHECK(read(cachedFile.c_str()) == map.data);
    // Aliasing is session scoped; cached bytes persist for offline demo replay.
    netMapClear(); CHECK(read(cachedFile.c_str()) == map.data);
    for (const char *name : {"../escape.map", "/escape.map", "C:\\escape.map", "dir/../../escape.map"}) {
        CHECK(netMapInstall(name, expected, map.data, error));
        const std::string alias = resolved(name);
        CHECK(alias == cached);
        CHECK(alias.find(".nblood-map-cache/") != std::string::npos);
        CHECK(alias.find("..") == std::string::npos);
        CHECK(read((alias+".MAP").c_str()) == map.data);
        CHECK(access("../escape.map", F_OK) != 0);
        netMapClear();
        CHECK(netMapPrepare(name, expected, error));
        CHECK(resolved(name) == cached);
        netMapClear();
    }
    for (const std::string& name : {std::string(), std::string("bad\nname"), std::string("bad\x7f"), std::string(251, 'a')}) {
        CHECK(!netMapInstall(name.c_str(), expected, map.data, error));
        CHECK(!netMapPrepare(name.c_str(), expected, error));
        Bytes bytes; CHECK(!netMapRead(name.c_str(), bytes, error));
    }
    // A remote absolute/traversing identifier may hit the checksum cache, but
    // Prepare must not probe the indicated local file. Read is the server API
    // for its own configured map, and intentionally accepts such paths.
    const Map outside = fixture(false);
    const uint32_t outsideCRC = mapCRC(outside.data);
    write("../outside.map", outside.data);
    char current[4096];
#ifdef _WIN32
    CHECK(_getcwd(current, sizeof(current)) != nullptr);
#else
    CHECK(getcwd(current, sizeof(current)) != nullptr);
#endif
    const std::string absolute = std::string(current)+"/../outside.map";
    CHECK(!netMapPrepare("../outside.map", outsideCRC, error));
    CHECK(!netMapPrepare(absolute.c_str(), outsideCRC, error));
    Bytes configured; CHECK(netMapRead("../outside.map", configured, error));
    CHECK(configured == outside.data);
    CHECK(netMapRead(absolute.c_str(), configured, error));
    CHECK(configured == outside.data);
    CHECK(netMapInstall("../outside.map", expected, map.data, error));
    CHECK(read("../outside.map") == outside.data);
    CHECK(resolved("../outside.map") == cached);
    CHECK(netMapInstall(absolute.c_str(), expected, map.data, error));
    CHECK(read("../outside.map") == outside.data);
    CHECK(resolved(absolute.c_str()) == cached);
    netMapClear();
    CHECK(unlink("../outside.map") == 0);
    Bytes corrupt = map.data; corrupt[20] ^= 1;
    CHECK(!netMapInstall("bad.map", expected, corrupt, error));
    CHECK(resolved("bad.map") == "bad.map");
    CHECK(read(cachedFile.c_str()) == map.data);
    CHECK(!netMapPrepare("bad.map", expected ^ 1, error));
    CHECK(!netMapInstall("wrong-crc.map", expected ^ 1, map.data, error));
    CHECK(resolved("wrong-crc.map") == "wrong-crc.map");
    write("local.map", map.data);
    CHECK(netMapPrepare("local.map", expected, error));
    Bytes loaded;
    CHECK(netMapRead("local.map", loaded, error));
    CHECK(loaded == map.data);
    CHECK(netMapPrepare("installed.map", expected, error));
    CHECK(resolved("installed.map") == cached);
    CHECK(!netMapPrepare("missing.map", expected ^ 1, error));
    CHECK(resolved("installed.map") == "installed.map");
    netMapClear();
    // Refuse linked cache entries rather than overwrite an arbitrary link target.
    CHECK(unlink(cachedFile.c_str()) == 0);
    if (makeLink("../installed.map", cachedFile.c_str(), false)) {
        CHECK(!netMapInstall("linked.map", expected, map.data, error));
        CHECK(read("installed.map") == sentinel);
        removeLink(cachedFile.c_str(), false);
    }
    CHECK(netMapInstall("again.map", expected, map.data, error));
    netMapClear();
    CHECK(rename(".nblood-map-cache", "cache-saved") == 0);
#ifdef _WIN32
    CHECK(_mkdir("outside-cache") == 0);
#else
    CHECK(mkdir("outside-cache", 0700) == 0);
#endif
    if (makeLink("outside-cache", ".nblood-map-cache", true)) {
        CHECK(!netMapInstall("linked-directory.map", expected, map.data, error));
        removeLink(".nblood-map-cache", true);
    }
    CHECK(rename("cache-saved", ".nblood-map-cache") == 0);
    CHECK(read("installed.map") == sentinel);
    std::puts("PASS: checksum cache, local map preservation, offline reuse, scoped aliases and traversal (link checks where supported)");
}
}
int main(int argc, char **argv)
{
    validation(); extendedReferences(); cache();
    bool failed = false;
    for (int i = 1; i < argc; ++i) {
        const Bytes data = read(argv[i]);
        CHECK(data.size() >= 4);
        std::string error;
        if (!netMapValidate(data, mapCRC(data), error)) {
            std::fprintf(stderr, "FAIL real map %s: %s\n", argv[i], error.c_str());
            failed = true;
            continue;
        }
        std::printf("PASS: real map %s (%zu bytes)\n", argv[i], data.size());
    }
    std::printf("%s: %u map storage checks\n", failed ? "FAIL" : "PASS", checks);
    return failed ? 1 : 0;
}
