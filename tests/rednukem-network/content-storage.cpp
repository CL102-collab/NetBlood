// Production addon codec/cache tests. No engine, archive extractor, or game data.
#include "rnet_addon.h"
#include "rnet_map_binding.h"

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

void mapBinding()
{
    rnet::content::MapBinding binding;std::string error;
    CHECK(binding.resolve("ROOM.MAP")==nullptr);
    // A malicious absolute/traversal Welcome cannot bypass a safe offer, even
    // when an identical map is already installed and no download is required.
    for(auto const& welcome:std::vector<std::string>{"/tmp/private/ROOM.MAP","../../ROOM.MAP",
        "C:\\private\\ROOM.MAP","/ROOM.MAP","./ROOM.MAP"})
    {
        CHECK(binding.announce(welcome,"ROOM.MAP",error));
        CHECK(binding.lookup()=="ROOM.MAP");
        CHECK(binding.resolve(welcome.c_str())==nullptr);
        binding.verified(binding.lookup());
        CHECK(std::string(binding.resolve(welcome.c_str()))=="ROOM.MAP");
        CHECK(binding.resolve("/different/ROOM.MAP")==nullptr);
    }
    CHECK(binding.announce("maps/ROOM.MAP","ROOM.MAP",error));
    CHECK(binding.lookup()=="maps/ROOM.MAP");
    binding.verified(binding.lookup());
    CHECK(std::string(binding.resolve("maps/ROOM.MAP"))=="maps/ROOM.MAP");
    CHECK(binding.resolve("ROOM.MAP")==nullptr);
    CHECK(binding.announce("/server/ROOM.MAP","room.map",error));
    binding.verified("/local/.rednukem-addon-cache/digest/room.map");
    CHECK(std::string(binding.resolve("/server/ROOM.MAP"))=="/local/.rednukem-addon-cache/digest/room.map");
    for(auto const& offer:std::vector<std::string>{"OTHER.MAP","maps/ROOM.MAP","../ROOM.MAP","ROOM.CON",""})
    {
        CHECK(!binding.announce("/server/ROOM.MAP",offer,error));
        CHECK(binding.resolve("/server/ROOM.MAP")==nullptr);
    }
    CHECK(binding.announce("ROOM.MAP","ROOM.MAP",error));
    binding.verified("ROOM.MAP");binding.clear();
    CHECK(binding.resolve("ROOM.MAP")==nullptr);
}

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
    rnetaddon::Profile profile;
    std::vector<Bytes> data;
    explicit Fixture(const std::string &name = "Test addon")
    {
        profile.name = name;
        const std::string ini = "[Episode1]\nTitle=Test addon\nMap1=maps/start.map\n";
        data.emplace_back(ini.begin(), ini.end());
        data.emplace_back(rnetaddon::ChunkBytes * 2 + 17);
        for (size_t i = 0; i < data.back().size(); ++i)
            data.back()[i] = uint8_t(i * 13 + i / 17);
        const std::string definition = "// Test definition\n";
        data.emplace_back(definition.begin(), definition.end());
        const char *paths[] = {"ADDON.CON", "packs/art.pk3", "defs/main.def"};
        for (size_t i = 0; i < data.size(); ++i) {
            rnetaddon::File file;
            file.path = paths[i]; file.size = data[i].size(); file.md5 = digest(data[i]);
            profile.files.push_back(file);
        }
        profile.selections.push_back({rnetaddon::SelectionKind::Script, paths[0]});
        profile.selections.push_back({rnetaddon::SelectionKind::Archive, paths[1]});
        profile.selections.push_back({rnetaddon::SelectionKind::Definition, paths[2]});
    }
    void install(const std::string &directory) const
    {
        for (size_t i = 0; i < data.size(); ++i) put(directory + "/" + profile.files[i].path, data[i]);
    }
};
bool decode(const Bytes &bytes, rnetaddon::Profile &profile, std::string &error)
{
    netwire::Reader reader(bytes.data(), bytes.size());
    return rnetaddon::decode(reader, profile, error);
}
Bytes rawProfile(const rnetaddon::Profile &profile)
{
    // Build hostile wire input without calling production validation/encoding.
    netwire::Writer writer;
    writer.u32(rnetaddon::ProfileVersion);
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
void rejection(const rnetaddon::Profile &profile)
{
    std::string error;
    CHECK(!rnetaddon::validate(profile, error));
    CHECK(!error.empty());
    Bytes bytes;
    CHECK(!rnetaddon::encode(profile, bytes, error));
    rnetaddon::Profile decoded;
    decoded.name = "unchanged on failure";
    CHECK(!decode(rawProfile(profile), decoded, error));
    CHECK(decoded.name == "unchanged on failure");
}
void codec()
{
    Fixture fixture;
    std::string error;
    CHECK(rnetaddon::validate(fixture.profile, error));
    Bytes bytes;
    CHECK(rnetaddon::encode(fixture.profile, bytes, error));
    CHECK(!bytes.empty());
    rnetaddon::Profile decoded;
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
    CHECK(rnetaddon::encode(decoded, encodedAgain, error));
    CHECK(encodedAgain == bytes);
    CHECK(rnetaddon::identity(decoded) == rnetaddon::identity(fixture.profile));
    CHECK(rnetaddon::identity(decoded).size() == 32);
    auto relabeled = fixture.profile;
    relabeled.name = "A different cosmetic display name";
    CHECK(rnetaddon::identity(relabeled) == rnetaddon::identity(fixture.profile));
    for (size_t length = 0; length < bytes.size(); ++length) {
        Bytes truncated(bytes.begin(), bytes.begin() + length);
        CHECK(!decode(truncated, decoded, error));
    }
    Bytes trailing = bytes; trailing.push_back(0);
    CHECK(!decode(trailing, decoded, error));
    Bytes oversized(rnetaddon::MaxProfileBytes + 1, 0);
    CHECK(!decode(oversized, decoded, error));
    Bytes badVersion = bytes; badVersion[0] ^= 0xff;
    CHECK(!decode(badVersion, decoded, error));
    auto changed = fixture.profile;
    changed.files[0].md5[3] ^= 1;
    CHECK(rnetaddon::identity(changed) != rnetaddon::identity(fixture.profile));
    changed = fixture.profile;
    std::swap(changed.selections[1], changed.selections[2]);
    CHECK(rnetaddon::identity(changed) != rnetaddon::identity(fixture.profile));
    changed = fixture.profile; changed.files[0].size++;
    CHECK(rnetaddon::identity(changed) != rnetaddon::identity(fixture.profile));
    changed = fixture.profile; changed.selections[0].path = "OTHER.CON";
    CHECK(rnetaddon::identity(changed) != rnetaddon::identity(fixture.profile));
    changed = fixture.profile; changed.selections[0].kind = rnetaddon::SelectionKind(255); rejection(changed);
    changed = fixture.profile;
    changed.selections.push_back({rnetaddon::SelectionKind::ScriptModule, "tiles%08x"}); rejection(changed);
    changed = fixture.profile;
    changed.selections.push_back({rnetaddon::SelectionKind::ScriptModule, "assets/custom.con"});
    CHECK(rnetaddon::validate(changed, error));
    changed = fixture.profile; changed.files.push_back(changed.files[0]); rejection(changed);
    changed = fixture.profile; changed.files.push_back(changed.files[0]);
    changed.files.back().path = "addon.con"; rejection(changed);
    changed = fixture.profile; changed.files.push_back(changed.files[0]);
    changed.files.back().path = "addon.con/other.def"; rejection(changed);
    changed = fixture.profile; changed.selections.push_back(changed.selections[0]); rejection(changed);
    changed = fixture.profile; changed.selections[0].path = "bad.exe"; rejection(changed);
    changed = fixture.profile; changed.files[0].path = "../escape.con"; rejection(changed);
    changed = fixture.profile; changed.files[0].path = "start.cfg"; rejection(changed);
    changed = fixture.profile; changed.files[0].path = "sub/NUL.con"; rejection(changed);
    changed = fixture.profile; changed.files[0].size = rnetaddon::MaxFileBytes + 1; rejection(changed);
    changed = fixture.profile; changed.files[0].size = std::numeric_limits<uint64_t>::max(); rejection(changed);
    changed = fixture.profile;
    for (unsigned i = 0; i < 5; ++i) {
        rnetaddon::File file; file.path = "extra" + std::to_string(i) + ".art";
        file.size = rnetaddon::MaxFileBytes; changed.files.push_back(file);
    }
    rejection(changed);
    changed = fixture.profile; changed.files.resize(rnetaddon::MaxFiles + 1); rejection(changed);
    changed = fixture.profile; changed.selections.resize(rnetaddon::MaxSelections + 1); rejection(changed);
    changed = fixture.profile; changed.name = "bad\nname"; rejection(changed);
    changed = fixture.profile; changed.name = std::string("bad\0name", 8); rejection(changed);
    changed = fixture.profile; changed.name.assign(129, 'a'); rejection(changed);
}
void preloadArchives()
{
    Fixture fixture;
    fixture.profile.selections.insert(fixture.profile.selections.begin(),
        {{rnetaddon::SelectionKind::PreloadArchive, "packs/base.grp"},
         {rnetaddon::SelectionKind::PreloadArchive, "packs/patch.pk3"}});
    std::string error;
    CHECK(rnetaddon::validate(fixture.profile, error));
    Bytes bytes;
    CHECK(rnetaddon::encode(fixture.profile, bytes, error));
    rnetaddon::Profile decoded;
    CHECK(decode(bytes, decoded, error));
    CHECK(decoded.selections.size() == fixture.profile.selections.size());
    for (size_t i = 0; i < decoded.selections.size(); ++i) {
        CHECK(decoded.selections[i].kind == fixture.profile.selections[i].kind);
        CHECK(decoded.selections[i].path == fixture.profile.selections[i].path);
    }
    CHECK(rnetaddon::identity(decoded) == rnetaddon::identity(fixture.profile));
    auto changed = fixture.profile;
    changed.selections[0].kind = rnetaddon::SelectionKind::Archive;
    CHECK(rnetaddon::validate(changed, error));
    CHECK(rnetaddon::identity(changed) != rnetaddon::identity(fixture.profile));
    changed = fixture.profile;
    std::swap(changed.selections[0], changed.selections[1]);
    CHECK(rnetaddon::identity(changed) != rnetaddon::identity(fixture.profile));
    for (const char *extension : {"grp", "zip", "pk3", "pk4", "ssi"}) {
        changed = fixture.profile;
        changed.selections[0].path = std::string("packs/preload.") + extension;
        CHECK(rnetaddon::validate(changed, error));
    }
    for (const char *path : {"packs/preload.exe", "packs/preload.con", "../escape.grp"}) {
        changed = fixture.profile;
        changed.selections[0].path = path;
        rejection(changed);
    }
}
void paths()
{
    for (const char *path : {"ADDON.CON", "packs/a.pk3", "textures/Wall 01.png", "maps/second.map", "maphacks/second.mhk"}) {
        CHECK(rnetaddon::validPath(path));
        CHECK(rnetaddon::allowedFile(path));
    }
    std::vector<std::string> unsafe = {
        "", ".", "..", "../bad.con", "dir/../bad.con", "./bad.con", "/bad.con",
        "//host/bad.con", "dir//bad.con", "dir/./bad.con", "dir/", "dir\\bad.con",
        "C:/bad.con", "C:bad.con", "bad.con:stream", "dir/bad.con ",
        "dir/bad.con.", "dir\n/bad.con", "bad\r.con", "bad\t.con",
        "CON.con", "aux.def", "NUL.pk3", "COM1.con", "lpt9.con", "dir/PRN.art",
        "CONIN$.con", "CONOUT$.con", "clock$.con", "bad?.con", "bad*.con", "bad|.con",
        std::string("bad\0.con", 8), std::string(rnetaddon::MaxPath + 1, 'a') + ".con"
    };
    for (const std::string &path : unsafe) {
        if (rnetaddon::validPath(path)) std::fprintf(stderr, "Accepted unsafe path: %s\n", path.c_str());
        CHECK(!rnetaddon::validPath(path));
    }
    for (const char *path : {"addon.exe", "launch.sh", "config.cfg", "plugin.dll", "script.bat", "script.cmd", "file.so", "addon.com"})
        CHECK(!rnetaddon::allowedFile(path));
}
void sourceFiles()
{
    Directory scope("sources");
    Fixture fixture;
    fixture.install("installed");
    std::string error;
    CHECK(rnetaddon::matches("installed", fixture.profile, error));
    for (size_t i = 0; i < fixture.data.size(); ++i) {
        const auto &expected = fixture.profile.files[i];
        rnetaddon::File inspected; inspected.path = expected.path;
        CHECK(rnetaddon::inspectFile("installed/" + expected.path, inspected, error));
        CHECK(inspected.path == expected.path);
        CHECK(inspected.size == expected.size);
        CHECK(inspected.md5 == expected.md5);
        Bytes received;
        for (size_t offset = 0; offset < fixture.data[i].size();) {
            Bytes chunk;
            CHECK(rnetaddon::readChunk("installed/" + expected.path, expected, offset,
                                     rnetaddon::ChunkBytes, chunk, error));
            CHECK(!chunk.empty()); CHECK(chunk.size() <= rnetaddon::ChunkBytes);
            received.insert(received.end(), chunk.begin(), chunk.end()); offset += chunk.size();
        }
        CHECK(received == fixture.data[i]);
        Bytes chunk;
        CHECK(!rnetaddon::readChunk("installed/" + expected.path, expected, expected.size + 1,
                                  rnetaddon::ChunkBytes, chunk, error));
        CHECK(!rnetaddon::readChunk("installed/" + expected.path, expected, 0, 0, chunk, error));
        CHECK(!rnetaddon::readChunk("installed/" + expected.path, expected, 0,
                                  rnetaddon::ChunkBytes + 1, chunk, error));
        CHECK(rnetaddon::readChunk("installed/" + expected.path, expected, expected.size,
                                 rnetaddon::ChunkBytes, chunk, error));
        CHECK(chunk.empty());
    }
    auto missing = fixture.profile;
    missing.files[0].path = "missing.con";
    CHECK(!rnetaddon::matches("installed", missing, error));
    CHECK(!error.empty());
    Bytes corrupt = fixture.data[0]; corrupt[0] ^= 1;
    put("installed/ADDON.CON", corrupt);
    CHECK(!rnetaddon::matches("installed", fixture.profile, error));
    put("installed/ADDON.CON", fixture.data[0]);
    CHECK(symlink("ADDON.CON", "installed/linked.con") == 0);
    rnetaddon::File inspected; inspected.path = "linked.con";
    CHECK(!rnetaddon::inspectFile("installed/linked.con", inspected, error));
    Bytes chunk;
    CHECK(!rnetaddon::readChunk("installed/linked.con", fixture.profile.files[0], 0,
                              rnetaddon::ChunkBytes, chunk, error));
    CHECK(symlink("installed", "linked") == 0);
    inspected.path = "ADDON.CON";
    CHECK(!rnetaddon::matches("linked", fixture.profile, error));
    CHECK(mkfifo("pipe.con", 0600) == 0);
    inspected.path = "pipe.con";
    CHECK(!rnetaddon::inspectFile("pipe.con", inspected, error));
    CHECK(!rnetaddon::readChunk("pipe.con", fixture.profile.files[0], 0,
                              rnetaddon::ChunkBytes, chunk, error));
}
void upload(rnetaddon::Cache &cache, const Fixture &fixture, size_t index)
{
    std::string error;
    const Bytes &data = fixture.data[index];
    for (size_t offset = 0; offset < data.size();) {
        size_t length = std::min(rnetaddon::ChunkBytes, data.size() - offset);
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
    rnetaddon::Cache cache;
    CHECK(cache.begin(fixture.profile, error));
    const std::string directory = cache.directory();
    CHECK(!directory.empty());
    CHECK(cache.nextMissingFile() == 0);
    CHECK(!cache.hasFile(fixture.data.size()));
    rnetaddon::Profile loaded;
    CHECK(!rnetaddon::loadCached(directory, loaded, error));
    CHECK(!cache.finish(error));
    for (size_t i = 0; i < fixture.data.size(); ++i) {
        CHECK(cache.nextMissingFile() == i);
        upload(cache, fixture, i);
        CHECK(!rnetaddon::loadCached(directory, loaded, error));
    }
    CHECK(cache.nextMissingFile() == fixture.data.size());
    CHECK(cache.finish(error));
    CHECK(rnetaddon::loadCached(directory, loaded, error));
    CHECK(rnetaddon::identity(loaded) == rnetaddon::identity(fixture.profile));
    CHECK(rnetaddon::matches(directory, fixture.profile, error));
    for (size_t i = 0; i < fixture.data.size(); ++i)
        CHECK(get(directory + "/" + fixture.profile.files[i].path) == fixture.data[i]);
    cache.cancel(); // Disconnecting must not remove an installed, verified addon.
    CHECK(rnetaddon::loadCached(directory, loaded, error));
    CHECK(cache.begin(fixture.profile, error));
    CHECK(cache.nextMissingFile() == fixture.data.size());
    for (size_t i = 0; i < fixture.data.size(); ++i) CHECK(cache.hasFile(i));
    CHECK(cache.finish(error));
    cache.cancel();
    Bytes corrupt = fixture.data[1]; corrupt.back() ^= 1;
    put(directory + "/" + fixture.profile.files[1].path, corrupt);
    CHECK(!rnetaddon::loadCached(directory, loaded, error));
    CHECK(cache.begin(fixture.profile, error));
    CHECK(cache.hasFile(0)); CHECK(!cache.hasFile(1)); CHECK(cache.hasFile(2));
    CHECK(cache.write(1, 0, fixture.data[1].data(), rnetaddon::ChunkBytes, error));
    CHECK(get(directory + "/" + fixture.profile.files[1].path) == corrupt);
    for (size_t offset = rnetaddon::ChunkBytes; offset < fixture.data[1].size();) {
        size_t length = std::min(rnetaddon::ChunkBytes, fixture.data[1].size() - offset);
        CHECK(cache.write(1, offset, fixture.data[1].data() + offset, length, error));
        offset += length;
    }
    CHECK(cache.finishFile(1, error));
    CHECK(get(directory + "/" + fixture.profile.files[1].path) == fixture.data[1]);
    CHECK(cache.finish(error));
    CHECK(rnetaddon::loadCached(directory, loaded, error));
}
void failures()
{
    Directory scope("failures");
    Fixture fixture;
    std::string error;
    rnetaddon::Cache cache;
    CHECK(cache.begin(fixture.profile, error));
    const std::string directory = cache.directory();
    CHECK(!cache.write(0, 1, fixture.data[0].data(), 1, error));
    CHECK(!cache.write(fixture.data.size(), 0, fixture.data[0].data(), 1, error));
    CHECK(!cache.finishFile(fixture.data.size(), error));
    CHECK(!cache.finishFile(0, error));
    CHECK(!cache.write(0, 0, fixture.data[0].data(), fixture.data[0].size() + 1, error));
    CHECK(!cache.write(0, 0, nullptr, 1, error));
    CHECK(!cache.write(0, 0, nullptr, 0, error));
    CHECK(!cache.write(1, 0, fixture.data[1].data(), rnetaddon::ChunkBytes + 1, error));
    CHECK(!cache.write(0, std::numeric_limits<uint64_t>::max(), fixture.data[0].data(), 1, error));
    CHECK(cache.write(0, 0, fixture.data[0].data(), 3, error));
    CHECK(!exists(directory + "/ADDON.CON"));
    CHECK(partialFiles(directory) == 1);
    CHECK(!cache.write(0, 0, fixture.data[0].data(), 3, error));
    CHECK(!cache.write(0, 4, fixture.data[0].data() + 4, 1, error));
    CHECK(!cache.write(1, 0, fixture.data[1].data(), 1, error));
    CHECK(!cache.finishFile(0, error));
    CHECK(!cache.finish(error));
    cache.cancel();
    CHECK(partialFiles(directory) == 0);
    rnetaddon::Profile loaded;
    CHECK(!rnetaddon::loadCached(directory, loaded, error));
    CHECK(cache.begin(fixture.profile, error));
    CHECK(!cache.hasFile(0));
    CHECK(cache.nextMissingFile() == 0);
    Bytes corrupt = fixture.data[0]; corrupt[0] ^= 1;
    CHECK(cache.write(0, 0, corrupt.data(), corrupt.size(), error));
    CHECK(!cache.finishFile(0, error));
    CHECK(partialFiles(directory) == 0);
    CHECK(!exists(directory + "/ADDON.CON"));
    CHECK(!cache.hasFile(0));
    CHECK(!cache.finish(error));
    CHECK(!rnetaddon::loadCached(directory, loaded, error));
    cache.cancel();
    { // Destruction after a partial transfer must behave like cancellation.
        rnetaddon::Cache interrupted;
        CHECK(interrupted.begin(fixture.profile, error));
        CHECK(interrupted.write(0, 0, fixture.data[0].data(), 3, error));
    }
    CHECK(partialFiles(directory) == 0);
    CHECK(!rnetaddon::loadCached(directory, loaded, error));
    CHECK(cache.begin(fixture.profile, error));
    CHECK(!cache.hasFile(0));
    for (size_t i = 0; i < fixture.data.size(); ++i) upload(cache, fixture, i);
    CHECK(cache.finish(error));
    CHECK(rnetaddon::loadCached(directory, loaded, error));
    CHECK(partialFiles(directory) == 0);
}
void cacheSymlinks()
{
    Directory scope("cache-symlinks");
    Fixture fixture;
    fixture.install("outside");
    const std::string outside = cwd() + "/outside";
    std::string error;
    rnetaddon::Cache cache;
    CHECK(cache.begin(fixture.profile, error));
    const std::string directory = cache.directory();
    cache.cancel();
    CHECK(symlink((outside + "/ADDON.CON").c_str(), (directory + "/ADDON.CON").c_str()) == 0);
    bool begun = cache.begin(fixture.profile, error);
    if (begun) {
        CHECK(!cache.hasFile(0));
        CHECK(cache.write(0, 0, fixture.data[0].data(), fixture.data[0].size(), error));
        CHECK(!cache.finishFile(0, error));
    }
    cache.cancel();
    CHECK(get("outside/ADDON.CON") == fixture.data[0]);
    CHECK(unlink((directory + "/ADDON.CON").c_str()) == 0);
    CHECK(symlink((outside + "/packs").c_str(), (directory + "/packs").c_str()) == 0);
    begun = cache.begin(fixture.profile, error);
    if (begun) {
        CHECK(!cache.hasFile(1));
        upload(cache, fixture, 0);
        for (size_t offset = 0; offset < fixture.data[1].size();) {
            size_t length = std::min(rnetaddon::ChunkBytes, fixture.data[1].size() - offset);
            CHECK(cache.write(1, offset, fixture.data[1].data() + offset, length, error));
            offset += length;
        }
        CHECK(!cache.finishFile(1, error));
    }
    cache.cancel();
    CHECK(get("outside/packs/art.pk3") == fixture.data[1]);
    rnetaddon::Profile loaded;
    CHECK(!rnetaddon::loadCached(directory, loaded, error));
}
void emptyAndRootLinks()
{
    Directory scope("empty-and-root-links");
    Fixture fixture;
    fixture.data[0].clear();
    fixture.profile.files[0].size = 0;
    fixture.profile.files[0].md5 = digest(fixture.data[0]);
    std::string error;
    rnetaddon::Cache cache;
    CHECK(cache.begin(fixture.profile, error));
    const std::string directory = cache.directory();
    CHECK(cache.finishFile(0, error));
    CHECK(cache.hasFile(0));
    for (size_t i = 1; i < fixture.data.size(); ++i) upload(cache, fixture, i);
    CHECK(cache.finish(error));
    rnetaddon::Profile loaded;
    CHECK(rnetaddon::loadCached(directory, loaded, error));
    CHECK(get(directory + "/ADDON.CON").empty());
    cache.cancel();
    // Moving a valid entry under another identity must not activate it.
    const std::string renamed = directory + "-wrong";
    CHECK(rename(directory.c_str(), renamed.c_str()) == 0);
    CHECK(!rnetaddon::loadCached(renamed, loaded, error));
    CHECK(symlink(renamed.c_str(), directory.c_str()) == 0);
    CHECK(!cache.begin(fixture.profile, error));
    CHECK(!rnetaddon::loadCached(directory, loaded, error));
    CHECK(unlink(directory.c_str()) == 0);
    CHECK(rename(renamed.c_str(), directory.c_str()) == 0);
    const size_t slash = directory.rfind('/');
    CHECK(slash != std::string::npos);
    const std::string root = directory.substr(0, slash);
    CHECK(rename(root.c_str(), (root + "-moved").c_str()) == 0);
    CHECK(symlink((root + "-moved").c_str(), root.c_str()) == 0);
    CHECK(!cache.begin(fixture.profile, error));
    CHECK(!rnetaddon::loadCached(directory, loaded, error));
}
} // namespace

int main()
{
    mapBinding(); paths(); codec(); preloadArchives(); sourceFiles(); transfer(); failures(); cacheSymlinks(); emptyAndRootLinks();
    std::printf("addon: %u checks passed\n", checks);
}
