// Production addon codec/cache tests. No engine, archive extractor, or game data.
#include "netaddon.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

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

void directories(const std::string &path)
{
    for (size_t pos = 0; pos < path.size(); ++pos)
        if (path[pos] == '/' && pos)
            CHECK(mkdir(path.substr(0, pos).c_str(), 0700) == 0 || errno == EEXIST);
    CHECK(mkdir(path.c_str(), 0700) == 0 || errno == EEXIST);
}
void put(const std::string &path, const Bytes &bytes)
{
    size_t slash = path.rfind('/');
    if (slash != std::string::npos) directories(path.substr(0, slash));
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    CHECK(bool(output));
    output.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    output.close();
    CHECK(bool(output));
}
Bytes get(const std::string &path)
{
    std::ifstream input(path, std::ios::binary);
    CHECK(bool(input));
    return Bytes(std::istreambuf_iterator<char>(input), {});
}
unsigned partialFiles(const std::string &path)
{
    DIR *directory = opendir(path.c_str());
    CHECK(directory != nullptr);
    unsigned count = 0;
    while (const dirent *entry = readdir(directory))
        if (std::string(entry->d_name).find(".download-") == 0) ++count;
    CHECK(closedir(directory) == 0);
    return count;
}
bool exists(const std::string &path)
{
    struct stat status;
    return lstat(path.c_str(), &status) == 0;
}
std::string cwd()
{
    char path[4096];
    CHECK(getcwd(path, sizeof(path)) != nullptr);
    return path;
}
struct Directory
{
    std::string previous;
    explicit Directory(const std::string &path) : previous(cwd())
    {
        directories(path);
        CHECK(chdir(path.c_str()) == 0);
    }
    ~Directory() { CHECK(chdir(previous.c_str()) == 0); }
};
netassets::Digest digest(const Bytes &bytes)
{
    netassets::MD5 hash;
    hash.update(bytes.data(), bytes.size());
    return hash.finish();
}
struct Fixture
{
    netaddon::Profile profile;
    std::vector<Bytes> data;
    explicit Fixture(const std::string &name = "Test addon")
    {
        profile.name = name;
        const std::string ini = "[Episode1]\nTitle=Test addon\nMap1=maps/start.map\n";
        data.emplace_back(ini.begin(), ini.end());
        data.emplace_back(netaddon::ChunkBytes * 2 + 17);
        for (size_t i = 0; i < data.back().size(); ++i)
            data.back()[i] = uint8_t(i * 13 + i / 17);
        const std::string definition = "// Test definition\n";
        data.emplace_back(definition.begin(), definition.end());
        const char *paths[] = {"ADDON.INI", "packs/art.pk3", "defs/main.def"};
        for (size_t i = 0; i < data.size(); ++i) {
            netaddon::File file;
            file.path = paths[i]; file.size = data[i].size(); file.md5 = digest(data[i]);
            profile.files.push_back(file);
        }
        profile.selections.push_back({netaddon::SelectionKind::Ini, paths[0]});
        profile.selections.push_back({netaddon::SelectionKind::Archive, paths[1]});
        profile.selections.push_back({netaddon::SelectionKind::Definition, paths[2]});
    }
    void install(const std::string &directory) const
    {
        for (size_t i = 0; i < data.size(); ++i) put(directory + "/" + profile.files[i].path, data[i]);
    }
};
bool decode(const Bytes &bytes, netaddon::Profile &profile, std::string &error)
{
    netwire::Reader reader(bytes.data(), bytes.size());
    return netaddon::decode(reader, profile, error);
}
Bytes rawProfile(const netaddon::Profile &profile)
{
    // Build hostile wire input without calling production validation/encoding.
    netwire::Writer writer;
    writer.u32(netaddon::ProfileVersion);
    writer.text(profile.name, 65535);
    writer.u16(uint16_t(profile.selections.size()));
    for (const auto &selection : profile.selections) {
        writer.u8(uint8_t(selection.kind)); writer.text(selection.path, 65535);
    }
    writer.u32(uint32_t(profile.files.size()));
    for (const auto &file : profile.files) {
        writer.text(file.path, 65535);
        writer.u32(uint32_t(file.size)); writer.u32(uint32_t(file.size >> 32));
        writer.bytes(file.md5.data(), file.md5.size());
    }
    return writer.data;
}
void rejection(const netaddon::Profile &profile)
{
    std::string error;
    CHECK(!netaddon::validate(profile, error));
    CHECK(!error.empty());
    Bytes bytes;
    CHECK(!netaddon::encode(profile, bytes, error));
    netaddon::Profile decoded;
    decoded.name = "unchanged on failure";
    CHECK(!decode(rawProfile(profile), decoded, error));
    CHECK(decoded.name == "unchanged on failure");
}
void codec()
{
    Fixture fixture;
    std::string error;
    CHECK(netaddon::validate(fixture.profile, error));
    Bytes bytes;
    CHECK(netaddon::encode(fixture.profile, bytes, error));
    CHECK(!bytes.empty());
    netaddon::Profile decoded;
    CHECK(decode(bytes, decoded, error));
    CHECK(decoded.name == fixture.profile.name);
    CHECK(decoded.files.size() == fixture.profile.files.size());
    CHECK(decoded.selections.size() == fixture.profile.selections.size());
    for (size_t i = 0; i < decoded.files.size(); ++i) {
        CHECK(decoded.files[i].path == fixture.profile.files[i].path);
        CHECK(decoded.files[i].size == fixture.profile.files[i].size);
        CHECK(decoded.files[i].md5 == fixture.profile.files[i].md5);
    }
    Bytes encodedAgain;
    CHECK(netaddon::encode(decoded, encodedAgain, error));
    CHECK(encodedAgain == bytes);
    CHECK(netaddon::identity(decoded) == netaddon::identity(fixture.profile));
    CHECK(netaddon::identity(decoded).size() == 32);
    auto relabeled = fixture.profile;
    relabeled.name = "A different cosmetic display name";
    CHECK(netaddon::identity(relabeled) == netaddon::identity(fixture.profile));
    for (size_t length = 0; length < bytes.size(); ++length) {
        Bytes truncated(bytes.begin(), bytes.begin() + length);
        CHECK(!decode(truncated, decoded, error));
    }
    Bytes trailing = bytes; trailing.push_back(0);
    CHECK(!decode(trailing, decoded, error));
    Bytes oversized(netaddon::MaxProfileBytes + 1, 0);
    CHECK(!decode(oversized, decoded, error));
    Bytes badVersion = bytes; badVersion[0] ^= 0xff;
    CHECK(!decode(badVersion, decoded, error));
    auto changed = fixture.profile;
    changed.files[0].md5[3] ^= 1;
    CHECK(netaddon::identity(changed) != netaddon::identity(fixture.profile));
    changed = fixture.profile;
    std::swap(changed.selections[1], changed.selections[2]);
    CHECK(netaddon::identity(changed) != netaddon::identity(fixture.profile));
    changed = fixture.profile; changed.files[0].size++;
    CHECK(netaddon::identity(changed) != netaddon::identity(fixture.profile));
    changed = fixture.profile; changed.selections[0].path = "OTHER.INI";
    CHECK(netaddon::identity(changed) != netaddon::identity(fixture.profile));
    changed = fixture.profile; changed.selections[0].kind = netaddon::SelectionKind(255); rejection(changed);
    changed = fixture.profile;
    changed.selections.push_back({netaddon::SelectionKind::Art, "tiles%08x"}); rejection(changed);
    changed = fixture.profile;
    changed.selections.push_back({netaddon::SelectionKind::Art, "assets/custom"});
    CHECK(netaddon::validate(changed, error));
    changed = fixture.profile; changed.files.push_back(changed.files[0]); rejection(changed);
    changed = fixture.profile; changed.files.push_back(changed.files[0]);
    changed.files.back().path = "addon.ini"; rejection(changed);
    changed = fixture.profile; changed.files.push_back(changed.files[0]);
    changed.files.back().path = "addon.ini/other.def"; rejection(changed);
    changed = fixture.profile; changed.selections.push_back(changed.selections[0]); rejection(changed);
    changed = fixture.profile; changed.selections[0].path = "bad.exe"; rejection(changed);
    changed = fixture.profile; changed.files[0].path = "../escape.ini"; rejection(changed);
    changed = fixture.profile; changed.files[0].path = "start.cfg"; rejection(changed);
    changed = fixture.profile; changed.files[0].path = "sub/NUL.ini"; rejection(changed);
    changed = fixture.profile; changed.files[0].size = netaddon::MaxFileBytes + 1; rejection(changed);
    changed = fixture.profile; changed.files[0].size = std::numeric_limits<uint64_t>::max(); rejection(changed);
    changed = fixture.profile;
    for (unsigned i = 0; i < 5; ++i) {
        netaddon::File file; file.path = "extra" + std::to_string(i) + ".art";
        file.size = netaddon::MaxFileBytes; changed.files.push_back(file);
    }
    rejection(changed);
    changed = fixture.profile; changed.files.resize(netaddon::MaxFiles + 1); rejection(changed);
    changed = fixture.profile; changed.selections.resize(netaddon::MaxSelections + 1); rejection(changed);
    changed = fixture.profile; changed.name = "bad\nname"; rejection(changed);
    changed = fixture.profile; changed.name = std::string("bad\0name", 8); rejection(changed);
    changed = fixture.profile; changed.name.assign(129, 'a'); rejection(changed);
}
void preloadArchives()
{
    Fixture fixture;
    fixture.profile.selections.insert(fixture.profile.selections.begin(),
        {{netaddon::SelectionKind::PreloadArchive, "packs/base.grp"},
         {netaddon::SelectionKind::PreloadArchive, "packs/patch.pk3"}});
    std::string error;
    CHECK(netaddon::validate(fixture.profile, error));
    Bytes bytes;
    CHECK(netaddon::encode(fixture.profile, bytes, error));
    netaddon::Profile decoded;
    CHECK(decode(bytes, decoded, error));
    CHECK(decoded.selections.size() == fixture.profile.selections.size());
    for (size_t i = 0; i < decoded.selections.size(); ++i) {
        CHECK(decoded.selections[i].kind == fixture.profile.selections[i].kind);
        CHECK(decoded.selections[i].path == fixture.profile.selections[i].path);
    }
    CHECK(netaddon::identity(decoded) == netaddon::identity(fixture.profile));
    auto changed = fixture.profile;
    changed.selections[0].kind = netaddon::SelectionKind::Archive;
    CHECK(netaddon::validate(changed, error));
    CHECK(netaddon::identity(changed) != netaddon::identity(fixture.profile));
    changed = fixture.profile;
    std::swap(changed.selections[0], changed.selections[1]);
    CHECK(netaddon::identity(changed) != netaddon::identity(fixture.profile));
    for (const char *extension : {"grp", "zip", "pk3", "pk4", "ssi"}) {
        changed = fixture.profile;
        changed.selections[0].path = std::string("packs/preload.") + extension;
        CHECK(netaddon::validate(changed, error));
    }
    for (const char *path : {"packs/preload.exe", "packs/preload.ini", "../escape.grp"}) {
        changed = fixture.profile;
        changed.selections[0].path = path;
        rejection(changed);
    }
}
void paths()
{
    for (const char *path : {"ADDON.INI", "packs/a.pk3", "textures/Wall 01.png", "maps/second.map", "maphacks/second.mhk"}) {
        CHECK(netaddon::validPath(path));
        CHECK(netaddon::allowedFile(path));
    }
    std::vector<std::string> unsafe = {
        "", ".", "..", "../bad.ini", "dir/../bad.ini", "./bad.ini", "/bad.ini",
        "//host/bad.ini", "dir//bad.ini", "dir/./bad.ini", "dir/", "dir\\bad.ini",
        "C:/bad.ini", "C:bad.ini", "bad.ini:stream", "dir/bad.ini ",
        "dir/bad.ini.", "dir\n/bad.ini", "bad\r.ini", "bad\t.ini",
        "CON.ini", "aux.def", "NUL.pk3", "COM1.ini", "lpt9.ini", "dir/PRN.art",
        "CONIN$.ini", "CONOUT$.ini", "clock$.ini", "bad?.ini", "bad*.ini", "bad|.ini",
        std::string("bad\0.ini", 8), std::string(netaddon::MaxPath + 1, 'a') + ".ini"
    };
    for (const std::string &path : unsafe) {
        if (netaddon::validPath(path)) std::fprintf(stderr, "Accepted unsafe path: %s\n", path.c_str());
        CHECK(!netaddon::validPath(path));
    }
    for (const char *path : {"addon.exe", "launch.sh", "config.cfg", "plugin.dll", "script.bat", "script.cmd", "file.so", "addon.com"})
        CHECK(!netaddon::allowedFile(path));
}
void sourceFiles()
{
    Directory scope("sources");
    Fixture fixture;
    fixture.install("installed");
    std::string error;
    CHECK(netaddon::matches("installed", fixture.profile, error));
    for (size_t i = 0; i < fixture.data.size(); ++i) {
        const auto &expected = fixture.profile.files[i];
        netaddon::File inspected; inspected.path = expected.path;
        CHECK(netaddon::inspectFile("installed/" + expected.path, inspected, error));
        CHECK(inspected.path == expected.path);
        CHECK(inspected.size == expected.size);
        CHECK(inspected.md5 == expected.md5);
        Bytes received;
        for (size_t offset = 0; offset < fixture.data[i].size();) {
            Bytes chunk;
            CHECK(netaddon::readChunk("installed/" + expected.path, expected, offset,
                                     netaddon::ChunkBytes, chunk, error));
            CHECK(!chunk.empty()); CHECK(chunk.size() <= netaddon::ChunkBytes);
            received.insert(received.end(), chunk.begin(), chunk.end()); offset += chunk.size();
        }
        CHECK(received == fixture.data[i]);
        Bytes chunk;
        CHECK(!netaddon::readChunk("installed/" + expected.path, expected, expected.size + 1,
                                  netaddon::ChunkBytes, chunk, error));
        CHECK(!netaddon::readChunk("installed/" + expected.path, expected, 0, 0, chunk, error));
        CHECK(!netaddon::readChunk("installed/" + expected.path, expected, 0,
                                  netaddon::ChunkBytes + 1, chunk, error));
        CHECK(netaddon::readChunk("installed/" + expected.path, expected, expected.size,
                                 netaddon::ChunkBytes, chunk, error));
        CHECK(chunk.empty());
    }
    auto missing = fixture.profile;
    missing.files[0].path = "missing.ini";
    CHECK(!netaddon::matches("installed", missing, error));
    CHECK(!error.empty());
    Bytes corrupt = fixture.data[0]; corrupt[0] ^= 1;
    put("installed/ADDON.INI", corrupt);
    CHECK(!netaddon::matches("installed", fixture.profile, error));
    put("installed/ADDON.INI", fixture.data[0]);
    CHECK(symlink("ADDON.INI", "installed/linked.ini") == 0);
    netaddon::File inspected; inspected.path = "linked.ini";
    CHECK(!netaddon::inspectFile("installed/linked.ini", inspected, error));
    Bytes chunk;
    CHECK(!netaddon::readChunk("installed/linked.ini", fixture.profile.files[0], 0,
                              netaddon::ChunkBytes, chunk, error));
    CHECK(symlink("installed", "linked") == 0);
    inspected.path = "ADDON.INI";
    CHECK(!netaddon::matches("linked", fixture.profile, error));
    CHECK(mkfifo("pipe.ini", 0600) == 0);
    inspected.path = "pipe.ini";
    CHECK(!netaddon::inspectFile("pipe.ini", inspected, error));
    CHECK(!netaddon::readChunk("pipe.ini", fixture.profile.files[0], 0,
                              netaddon::ChunkBytes, chunk, error));
}
void upload(netaddon::Cache &cache, const Fixture &fixture, size_t index)
{
    std::string error;
    const Bytes &data = fixture.data[index];
    for (size_t offset = 0; offset < data.size();) {
        size_t length = std::min(netaddon::ChunkBytes, data.size() - offset);
        CHECK(cache.write(index, offset, data.data() + offset, length, error));
        offset += length;
    }
    CHECK(cache.finishFile(index, error));
    CHECK(cache.hasFile(index));
}
void transfer()
{
    Directory scope("transfer");
    Fixture fixture;
    std::string error;
    netaddon::Cache cache;
    CHECK(cache.begin(fixture.profile, error));
    const std::string directory = cache.directory();
    CHECK(!directory.empty());
    CHECK(cache.nextMissingFile() == 0);
    CHECK(!cache.hasFile(fixture.data.size()));
    netaddon::Profile loaded;
    CHECK(!netaddon::loadCached(directory, loaded, error));
    CHECK(!cache.finish(error));
    for (size_t i = 0; i < fixture.data.size(); ++i) {
        CHECK(cache.nextMissingFile() == i);
        upload(cache, fixture, i);
        CHECK(!netaddon::loadCached(directory, loaded, error));
    }
    CHECK(cache.nextMissingFile() == fixture.data.size());
    CHECK(cache.finish(error));
    CHECK(netaddon::loadCached(directory, loaded, error));
    CHECK(netaddon::identity(loaded) == netaddon::identity(fixture.profile));
    CHECK(netaddon::matches(directory, fixture.profile, error));
    for (size_t i = 0; i < fixture.data.size(); ++i)
        CHECK(get(directory + "/" + fixture.profile.files[i].path) == fixture.data[i]);
    cache.cancel(); // Disconnecting must not remove an installed, verified addon.
    CHECK(netaddon::loadCached(directory, loaded, error));
    CHECK(cache.begin(fixture.profile, error));
    CHECK(cache.nextMissingFile() == fixture.data.size());
    for (size_t i = 0; i < fixture.data.size(); ++i) CHECK(cache.hasFile(i));
    CHECK(cache.finish(error));
    cache.cancel();
    Bytes corrupt = fixture.data[1]; corrupt.back() ^= 1;
    put(directory + "/" + fixture.profile.files[1].path, corrupt);
    CHECK(!netaddon::loadCached(directory, loaded, error));
    CHECK(cache.begin(fixture.profile, error));
    CHECK(cache.hasFile(0)); CHECK(!cache.hasFile(1)); CHECK(cache.hasFile(2));
    CHECK(cache.write(1, 0, fixture.data[1].data(), netaddon::ChunkBytes, error));
    CHECK(get(directory + "/" + fixture.profile.files[1].path) == corrupt);
    for (size_t offset = netaddon::ChunkBytes; offset < fixture.data[1].size();) {
        size_t length = std::min(netaddon::ChunkBytes, fixture.data[1].size() - offset);
        CHECK(cache.write(1, offset, fixture.data[1].data() + offset, length, error));
        offset += length;
    }
    CHECK(cache.finishFile(1, error));
    CHECK(get(directory + "/" + fixture.profile.files[1].path) == fixture.data[1]);
    CHECK(cache.finish(error));
    CHECK(netaddon::loadCached(directory, loaded, error));
}
void failures()
{
    Directory scope("failures");
    Fixture fixture;
    std::string error;
    netaddon::Cache cache;
    CHECK(cache.begin(fixture.profile, error));
    const std::string directory = cache.directory();
    CHECK(!cache.write(0, 1, fixture.data[0].data(), 1, error));
    CHECK(!cache.write(fixture.data.size(), 0, fixture.data[0].data(), 1, error));
    CHECK(!cache.finishFile(fixture.data.size(), error));
    CHECK(!cache.finishFile(0, error));
    CHECK(!cache.write(0, 0, fixture.data[0].data(), fixture.data[0].size() + 1, error));
    CHECK(!cache.write(0, 0, nullptr, 1, error));
    CHECK(!cache.write(0, 0, nullptr, 0, error));
    CHECK(!cache.write(1, 0, fixture.data[1].data(), netaddon::ChunkBytes + 1, error));
    CHECK(!cache.write(0, std::numeric_limits<uint64_t>::max(), fixture.data[0].data(), 1, error));
    CHECK(cache.write(0, 0, fixture.data[0].data(), 3, error));
    CHECK(!exists(directory + "/ADDON.INI"));
    CHECK(partialFiles(directory) == 1);
    CHECK(!cache.write(0, 0, fixture.data[0].data(), 3, error));
    CHECK(!cache.write(0, 4, fixture.data[0].data() + 4, 1, error));
    CHECK(!cache.write(1, 0, fixture.data[1].data(), 1, error));
    CHECK(!cache.finishFile(0, error));
    CHECK(!cache.finish(error));
    cache.cancel();
    CHECK(partialFiles(directory) == 0);
    netaddon::Profile loaded;
    CHECK(!netaddon::loadCached(directory, loaded, error));
    CHECK(cache.begin(fixture.profile, error));
    CHECK(!cache.hasFile(0));
    CHECK(cache.nextMissingFile() == 0);
    Bytes corrupt = fixture.data[0]; corrupt[0] ^= 1;
    CHECK(cache.write(0, 0, corrupt.data(), corrupt.size(), error));
    CHECK(!cache.finishFile(0, error));
    CHECK(partialFiles(directory) == 0);
    CHECK(!exists(directory + "/ADDON.INI"));
    CHECK(!cache.hasFile(0));
    CHECK(!cache.finish(error));
    CHECK(!netaddon::loadCached(directory, loaded, error));
    cache.cancel();
    { // Destruction after a partial transfer must behave like cancellation.
        netaddon::Cache interrupted;
        CHECK(interrupted.begin(fixture.profile, error));
        CHECK(interrupted.write(0, 0, fixture.data[0].data(), 3, error));
    }
    CHECK(partialFiles(directory) == 0);
    CHECK(!netaddon::loadCached(directory, loaded, error));
    CHECK(cache.begin(fixture.profile, error));
    CHECK(!cache.hasFile(0));
    for (size_t i = 0; i < fixture.data.size(); ++i) upload(cache, fixture, i);
    CHECK(cache.finish(error));
    CHECK(netaddon::loadCached(directory, loaded, error));
    CHECK(partialFiles(directory) == 0);
}
void cacheSymlinks()
{
    Directory scope("cache-symlinks");
    Fixture fixture;
    fixture.install("outside");
    const std::string outside = cwd() + "/outside";
    std::string error;
    netaddon::Cache cache;
    CHECK(cache.begin(fixture.profile, error));
    const std::string directory = cache.directory();
    cache.cancel();
    CHECK(symlink((outside + "/ADDON.INI").c_str(), (directory + "/ADDON.INI").c_str()) == 0);
    bool begun = cache.begin(fixture.profile, error);
    if (begun) {
        CHECK(!cache.hasFile(0));
        CHECK(cache.write(0, 0, fixture.data[0].data(), fixture.data[0].size(), error));
        CHECK(!cache.finishFile(0, error));
    }
    cache.cancel();
    CHECK(get("outside/ADDON.INI") == fixture.data[0]);
    CHECK(unlink((directory + "/ADDON.INI").c_str()) == 0);
    CHECK(symlink((outside + "/packs").c_str(), (directory + "/packs").c_str()) == 0);
    begun = cache.begin(fixture.profile, error);
    if (begun) {
        CHECK(!cache.hasFile(1));
        upload(cache, fixture, 0);
        for (size_t offset = 0; offset < fixture.data[1].size();) {
            size_t length = std::min(netaddon::ChunkBytes, fixture.data[1].size() - offset);
            CHECK(cache.write(1, offset, fixture.data[1].data() + offset, length, error));
            offset += length;
        }
        CHECK(!cache.finishFile(1, error));
    }
    cache.cancel();
    CHECK(get("outside/packs/art.pk3") == fixture.data[1]);
    netaddon::Profile loaded;
    CHECK(!netaddon::loadCached(directory, loaded, error));
}
void emptyAndRootLinks()
{
    Directory scope("empty-and-root-links");
    Fixture fixture;
    fixture.data[0].clear();
    fixture.profile.files[0].size = 0;
    fixture.profile.files[0].md5 = digest(fixture.data[0]);
    std::string error;
    netaddon::Cache cache;
    CHECK(cache.begin(fixture.profile, error));
    const std::string directory = cache.directory();
    CHECK(cache.finishFile(0, error));
    CHECK(cache.hasFile(0));
    for (size_t i = 1; i < fixture.data.size(); ++i) upload(cache, fixture, i);
    CHECK(cache.finish(error));
    netaddon::Profile loaded;
    CHECK(netaddon::loadCached(directory, loaded, error));
    CHECK(get(directory + "/ADDON.INI").empty());
    cache.cancel();
    // Moving a valid entry under another identity must not activate it.
    const std::string renamed = directory + "-wrong";
    CHECK(rename(directory.c_str(), renamed.c_str()) == 0);
    CHECK(!netaddon::loadCached(renamed, loaded, error));
    CHECK(symlink(renamed.c_str(), directory.c_str()) == 0);
    CHECK(!cache.begin(fixture.profile, error));
    CHECK(!netaddon::loadCached(directory, loaded, error));
    CHECK(unlink(directory.c_str()) == 0);
    CHECK(rename(renamed.c_str(), directory.c_str()) == 0);
    const size_t slash = directory.rfind('/');
    CHECK(slash != std::string::npos);
    const std::string root = directory.substr(0, slash);
    CHECK(rename(root.c_str(), (root + "-moved").c_str()) == 0);
    CHECK(symlink((root + "-moved").c_str(), root.c_str()) == 0);
    CHECK(!cache.begin(fixture.profile, error));
    CHECK(!netaddon::loadCached(directory, loaded, error));
}
} // namespace

int main()
{
    paths(); codec(); preloadArchives(); sourceFiles(); transfer(); failures(); cacheSymlinks(); emptyAndRootLinks();
    std::printf("addon: %u checks passed\n", checks);
}
