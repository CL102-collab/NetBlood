// SPDX-License-Identifier: GPL-2.0-or-later
#include "netmap.h"
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <array>
#include <sys/stat.h>
#ifdef _WIN32
# include <windows.h>
# include <direct.h>
#else
# include <fcntl.h>
# include <unistd.h>
#endif

#ifndef NETMAP_STANDALONE
# include "build.h"
# include "common_game.h"
# include "globals.h"
# include "resource.h"
#endif

namespace {
using Bytes = std::vector<uint8_t>;
constexpr uint32_t HeaderNew = 0x7474614du, HeaderOld = 0x4d617474u;
constexpr char CacheDirectory[] = ".nblood-map-cache";
constexpr size_t MaxName = 250; // Resource paths use fixed 256-byte buffers.
#ifdef NETMAP_STANDALONE
constexpr unsigned MaxSectors = 4096, MaxWalls = 16384, MaxSprites = 16384, MaxStatus = 1024, MaxTiles = 30720;
#else
constexpr unsigned MaxSectors = MAXSECTORS, MaxWalls = MAXWALLS, MaxSprites = MAXSPRITES, MaxStatus = MAXSTATUS, MaxTiles = MAXTILES;
static_assert(sizeof(sectortype) == 40 && sizeof(walltype) == 32 && sizeof(spritetype) == 44,
              "Network Blood maps require the legacy map record layout");
#endif
std::string aliasName, aliasPath;

uint16_t u16(const uint8_t* p) { return uint16_t(p[0] | (uint16_t(p[1]) << 8)); }
int16_t s16(const uint8_t* p) { return int16_t(u16(p)); }
uint32_t u32(const uint8_t* p) { return uint32_t(u16(p)) | (uint32_t(u16(p + 2)) << 16); }
bool fail(std::string& error, const char* why) { error = why; return false; }
uint32_t checksum(const uint8_t* bytes, size_t length)
{
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> result{};
        for (unsigned i = 0; i < 256; ++i)
        {
            uint32_t n = i;
            for (unsigned bit = 0; bit < 8; ++bit) n = (n >> 1) ^ ((n & 1) ? 0xedb88320u : 0);
            result[i] = n;
        }
        return result;
    }();
    uint32_t crc = ~0u;
    while (length--) crc = (crc >> 8) ^ table[(crc ^ *bytes++) & 255];
    return ~crc;
}
void crypt(uint8_t* bytes, size_t length, uint32_t key)
{ for (size_t i = 0; i < length; ++i) bytes[i] ^= uint8_t(key + i); }

std::string normalized(const char* path)
{
    std::string result = path ? path : "";
    for (char& c : result)
    {
        if (c == '\\') c = '/';
        if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
    }
    if (result.size() >= 4 && result.compare(result.size() - 4, 4, ".MAP") == 0) result.resize(result.size() - 4);
    return result;
}
bool validCanonical(const char* name)
{
    if (!name || !*name || std::strlen(name) > MaxName) return false;
    for (const char* p = name; *p; ++p)
        if (uint8_t(*p) < 32 || uint8_t(*p) == 127) return false;
    return true;
}
bool safeLocalName(const char* name)
{
    if (!validCanonical(name)) return false;
    std::string value = normalized(name);
    if (value.empty() || value[0] == '/') return false;
    size_t begin = 0;
    for (size_t i = 0; i <= value.size(); ++i)
    {
        if (i < value.size() && (uint8_t(value[i]) < 32 || value[i] == ':' || uint8_t(value[i]) == 127)) return false;
        if (i == value.size() || value[i] == '/')
        {
            const size_t count = i - begin;
            if (!count || (count == 1 && value[begin] == '.') || (count == 2 && value.compare(begin, count, "..") == 0)) return false;
            begin = i + 1;
        }
    }
    return true;
}
std::string cacheLeaf(uint32_t crc)
{
    char name[24];
    std::snprintf(name, sizeof(name), "%08x.MAP", unsigned(crc));
    return name;
}
bool cacheAbsolute(uint32_t crc, std::string& result, std::string& error)
{
    char cwd[4096];
#ifdef _WIN32
    if (!_getcwd(cwd, sizeof(cwd))) return fail(error, "Cannot locate map cache directory");
#else
    if (!getcwd(cwd, sizeof(cwd))) return fail(error, "Cannot locate map cache directory");
#endif
    result = std::string(cwd) + "/" + CacheDirectory + "/" + cacheLeaf(crc);
    if (result.size() > MaxName) return fail(error, "Map cache path exceeds the resource path limit");
    return true;
}

#ifdef _WIN32
bool cacheDir(bool create, std::string& error)
{
    if (create && !CreateDirectoryA(CacheDirectory, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
        return fail(error, "Cannot create map cache directory");
    DWORD flags = GetFileAttributesA(CacheDirectory);
    return flags != INVALID_FILE_ATTRIBUTES && (flags & FILE_ATTRIBUTE_DIRECTORY) && !(flags & FILE_ATTRIBUTE_REPARSE_POINT)
        ? true : fail(error, "Map cache directory is missing or unsafe");
}
bool readDisk(const std::string& path, Bytes& bytes, std::string& error)
{
    HANDLE file = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return fail(error, "Cannot open map file");
    BY_HANDLE_FILE_INFORMATION info{};
    const bool regular = GetFileInformationByHandle(file, &info) && !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
        && info.nFileSizeHigh == 0 && info.nFileSizeLow >= 47 && info.nFileSizeLow <= netmap::MaxBytes;
    if (!regular) { CloseHandle(file); return fail(error, "Map file is not a bounded regular file"); }
    bytes.resize(info.nFileSizeLow);
    DWORD count = 0;
    bool ok = ReadFile(file, bytes.data(), DWORD(bytes.size()), &count, nullptr) && count == bytes.size();
    CloseHandle(file);
    if (!ok) { bytes.clear(); return fail(error, "Cannot read map file"); }
    return true;
}
bool readCache(uint32_t crc, Bytes& bytes, std::string& error)
{
    std::string path;
    return cacheDir(false, error) && cacheAbsolute(crc, path, error) && readDisk(path, bytes, error);
}
bool writeCache(uint32_t crc, const Bytes& bytes, std::string& error)
{
    std::string path;
    if (!cacheDir(true, error) || !cacheAbsolute(crc, path, error)) return false;
    DWORD attributes = GetFileAttributesA(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
        return fail(error, "Map cache destination is unsafe");
    static unsigned serial;
    char suffix[64];
    std::snprintf(suffix, sizeof(suffix), ".%lu.%u.part", (unsigned long)GetCurrentProcessId(), ++serial);
    const std::string temporary = path + suffix;
    HANDLE file = CreateFileA(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return fail(error, "Cannot create map cache temporary file");
    DWORD written = 0;
    bool ok = WriteFile(file, bytes.data(), DWORD(bytes.size()), &written, nullptr) && written == bytes.size() && FlushFileBuffers(file);
    CloseHandle(file);
    if (ok) ok = cacheDir(false, error) && MoveFileExA(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!ok) { DeleteFileA(temporary.c_str()); return fail(error, "Cannot publish downloaded map in cache"); }
    return true;
}
#else
struct Descriptor
{
    int fd;
    explicit Descriptor(int value) : fd(value) {}
    ~Descriptor() { if (fd >= 0) close(fd); }
};
int cacheDir(bool create, std::string& error)
{
    if (create && mkdir(CacheDirectory, 0700) < 0 && errno != EEXIST)
    { fail(error, "Cannot create map cache directory"); return -1; }
    int fd = open(CacheDirectory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) fail(error, "Map cache directory is missing or unsafe");
    return fd;
}
bool readFD(int fd, Bytes& bytes, std::string& error)
{
    struct stat status{};
    if (fd < 0 || fstat(fd, &status) < 0 || !S_ISREG(status.st_mode) || status.st_size < 47 || uint64_t(status.st_size) > netmap::MaxBytes)
        return fail(error, "Map file is not a bounded regular file");
    bytes.resize(size_t(status.st_size));
    size_t pos = 0;
    while (pos < bytes.size())
    {
        ssize_t count = read(fd, bytes.data() + pos, bytes.size() - pos);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { bytes.clear(); return fail(error, "Cannot read map file"); }
        pos += size_t(count);
    }
    return true;
}
#ifdef NETMAP_STANDALONE
bool readDisk(const std::string& path, Bytes& bytes, std::string& error)
{
    Descriptor file(open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
    return readFD(file.fd, bytes, error);
}
#endif
bool readCache(uint32_t crc, Bytes& bytes, std::string& error)
{
    Descriptor directory(cacheDir(false, error));
    if (directory.fd < 0) return false;
    Descriptor file(openat(directory.fd, cacheLeaf(crc).c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
    return readFD(file.fd, bytes, error);
}
bool writeCache(uint32_t crc, const Bytes& bytes, std::string& error)
{
    Descriptor directory(cacheDir(true, error));
    if (directory.fd < 0) return false;
    const std::string leaf = cacheLeaf(crc);
    struct stat status{};
    if (fstatat(directory.fd, leaf.c_str(), &status, AT_SYMLINK_NOFOLLOW) == 0)
    {
        if (!S_ISREG(status.st_mode)) return fail(error, "Map cache destination is unsafe");
    }
    else if (errno != ENOENT) return fail(error, "Cannot inspect map cache destination");
    static unsigned serial;
    char suffix[64];
    std::snprintf(suffix, sizeof(suffix), ".%lu.%u.part", (unsigned long)getpid(), ++serial);
    const std::string temporary = leaf + suffix;
    Descriptor file(openat(directory.fd, temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (file.fd < 0) return fail(error, "Cannot create map cache temporary file");
    size_t pos = 0;
    while (pos < bytes.size())
    {
        ssize_t count = write(file.fd, bytes.data() + pos, bytes.size() - pos);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        pos += size_t(count);
    }
    const bool ok = pos == bytes.size() && fsync(file.fd) == 0
        && renameat(directory.fd, temporary.c_str(), directory.fd, leaf.c_str()) == 0;
    if (!ok) { unlinkat(directory.fd, temporary.c_str(), 0); return fail(error, "Cannot publish downloaded map in cache"); }
    return true;
}
#endif

#ifndef NETMAP_STANDALONE
bool readResource(const char* name, Bytes& bytes, std::string& error)
{
    // Mirror Lookup's external-before-archive order without Resource::Read's
    // fatal error path, or creating resource nodes for untrusted names.
    const int savedMode = pathsearchmode;
    pathsearchmode = 1;
    std::string candidates[2] = { name, name };
    if (normalized(name).size() != std::strlen(name)) candidates[1].resize(candidates[1].size() - 4);
    bool found = false, ok = false;
    for (const auto& candidate : candidates)
    {
        const std::string filename = candidate + ".MAP";
        int file = kopen4loadfrommod(filename.c_str(), 0);
        if (file >= 0)
        {
            found = true;
            const int size = kfilelength(file);
            if (size >= 47 && size <= int(netmap::MaxBytes))
            {
                bytes.resize(size);
                ok = kread(file, bytes.data(), size) == size;
            }
            kclose(file);
        }
        else
        {
            std::string key = candidate;
            for (char& c : key) if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
            DICTNODE* node = *gSysRes.Probe(key.c_str(), "MAP");
            if (node)
            {
                found = true;
                if (node->size >= 47 && node->size <= netmap::MaxBytes)
                {
                    bytes.resize(node->size);
                    if (node->flags & DICT_BUFFER)
                    {
                        ok = node->buffer != nullptr;
                        if (ok) std::memcpy(bytes.data(), node->buffer, bytes.size());
                    }
                    else if (!(node->flags & DICT_EXTERNAL))
                    {
                        const int length = kfilelength(gSysRes.handle);
                        ok = length >= 0 && uint64_t(node->offset) + node->size <= uint64_t(length)
                            && klseek(gSysRes.handle, node->offset, SEEK_SET) >= 0
                            && kread(gSysRes.handle, bytes.data(), node->size) == int(node->size);
                        if (ok && (node->flags & DICT_CRYPT)) gSysRes.Crypt(bytes.data(), int(std::min(size_t(256), bytes.size())), 0);
                    }
                }
            }
        }
        if (found) break;
    }
    pathsearchmode = savedMode;
    if (!ok) { bytes.clear(); return fail(error, "Map is missing, unreadable or exceeds the download limit"); }
    return true;
}
#endif
}

bool netMapValidate(const std::vector<uint8_t>& bytes, uint32_t expectedCRC, std::string& error)
{
    error.clear();
    if (bytes.size() < 47 || bytes.size() > netmap::MaxBytes) return fail(error, "Map length is out of range");
    if (std::memcmp(bytes.data(), "BLM\x1a", 4)) return fail(error, "Map signature is invalid");
    const uint16_t version = u16(bytes.data() + 4);
    const bool encrypted = (version & 0xff00) == 0x700;
    if (!encrypted && (version & 0xff00) != 0x600) return fail(error, "Map version is unsupported");
    const size_t end = bytes.size() - 4;
    if (u32(bytes.data() + end) != expectedCRC || checksum(bytes.data(), end) != expectedCRC)
        return fail(error, "Map checksum does not match the server");
    uint8_t header[37];
    std::memcpy(header, bytes.data() + 6, sizeof(header));
    uint32_t marker = u32(header + 22);
    if (marker != 0 && marker != HeaderNew && marker != HeaderOld) crypt(header, sizeof(header), HeaderNew);
    marker = u32(header + 22);
    if ((!encrypted && marker != 0) || (encrypted && marker != 0 && marker != HeaderNew && marker != HeaderOld))
        return fail(error, "Map header marker is invalid");
    const int sectors = s16(header + 31), walls = s16(header + 33), sprites = s16(header + 35), sky = s16(header + 16);
    if (sectors <= 0 || unsigned(sectors) > MaxSectors || walls <= 0 || unsigned(walls) > MaxWalls || sprites < 0 || unsigned(sprites) > MaxSprites
        || s16(header + 14) < 0 || s16(header + 14) >= sectors || sky < 0 || sky > 8)
        return fail(error, "Map counts, starting sector or sky size are invalid");
    const uint32_t revision = u32(header + 27);
    if (revision > uint32_t(INT_MAX / 44)) return fail(error, "Map revision is out of range");
    unsigned xSprite = 56, xWall = 24, xSector = 60;
    size_t position = 43;
    if (encrypted)
    {
        if (end - position < 128) return fail(error, "Map extended header is truncated");
        uint8_t extended[128];
        std::memcpy(extended, bytes.data() + position, sizeof(extended));
        crypt(extended, sizeof(extended), walls);
        position += sizeof(extended);
        xSprite = u32(extended + 64); xWall = u32(extended + 68); xSector = u32(extended + 72);
        if (xSprite > 56 || xWall > 24 || xSector > 60)
            return fail(error, "Map extended record sizes are invalid");
    }
    const size_t skyBytes = size_t(2) << sky;
    if (skyBytes > end - position) return fail(error, "Map sky data is truncated");
    position += skyBytes;
    struct Sector { int type = 0, marker0 = -1, marker1 = -1; bool extended = false; };
    struct Wall { int point, next, sector; };
    std::vector<Sector> sectorInfo(sectors);
    std::vector<Wall> wallInfo(walls);
    std::vector<int> owners(walls, -1);
    unsigned xSectors = 0, xWalls = 0, xSprites = 0;
    auto record = [&](uint8_t* destination, size_t count, uint32_t key) {
        if (count > end - position) return false;
        std::memcpy(destination, bytes.data() + position, count);
        position += count;
        if (encrypted) crypt(destination, count, key);
        return true;
    };
    auto extra = [&](int value, unsigned count, unsigned required, unsigned& used, unsigned maximum) {
        if (value <= 0) return value >= -1;
        if (count != required || ++used >= maximum || count > end - position) return false;
        position += count;
        return true;
    };
    auto tile = [](int value) { return value >= 0 && unsigned(value) < MaxTiles; };
    for (int i = 0; i < sectors; ++i)
    {
        uint8_t data[40];
        if (!record(data, sizeof(data), revision * 40)) return fail(error, "Map sectors are truncated");
        const int first = s16(data), count = s16(data + 2), extraIndex = s16(data + 38);
        const size_t extended = position;
        // E6M7 contains a two-wall degenerate sector used by the original map.
        if (first < 0 || count < 2 || first > walls - count || !tile(s16(data + 16)) || !tile(s16(data + 24))
            || !extra(s16(data + 38), xSector, 60, xSectors, 4096))
            return fail(error, "Map sector records are invalid");
        sectorInfo[i].type = s16(data + 34);
        sectorInfo[i].extended = extraIndex > 0;
        if (extraIndex > 0)
        {
            const unsigned marker0 = u16(bytes.data() + extended + 44), marker1 = u16(bytes.data() + extended + 46);
            if ((marker0 != 65535 && marker0 >= MaxSprites) || (marker1 != 65535 && marker1 >= MaxSprites))
                return fail(error, "Map extended sector marker references are invalid");
            sectorInfo[i].marker0 = marker0 == 65535 ? -1 : int(marker0);
            sectorInfo[i].marker1 = marker1 == 65535 ? -1 : int(marker1);
        }
        for (int w = first; w < first + count; ++w)
        {
            if (owners[w] != -1) return fail(error, "Map sectors overlap wall ranges");
            owners[w] = i;
        }
    }
    for (int i = 0; i < walls; ++i)
    {
        uint8_t data[32];
        if (!record(data, sizeof(data), (revision * 40) | HeaderNew)) return fail(error, "Map walls are truncated");
        const int point = s16(data + 8), next = s16(data + 10), sector = s16(data + 12);
        const int overlay = s16(data + 18);
        const bool validOverlay = tile(overlay) || (overlay == -1 && !(u16(data + 14) & 48));
        if (owners[i] < 0 || point < 0 || point >= walls || owners[point] != owners[i]
            || next < -1 || next >= walls || sector < -1 || sector >= sectors || ((next == -1) != (sector == -1))
            || !tile(s16(data + 16)) || !validOverlay || !extra(s16(data + 30), xWall, 24, xWalls, 16384))
            return fail(error, "Map wall references or records are invalid");
        wallInfo[i] = { point, next, sector };
    }
    // A point2 permutation gives closed loops, including sectors with holes.
    std::vector<unsigned> incoming(walls, 0);
    for (int i = 0; i < walls; ++i)
    {
        const Wall& wall = wallInfo[i];
        if (++incoming[wall.point] != 1 || wall.point == i) return fail(error, "Map wall loops are invalid");
        // CP02 has a stale nextsector and E3M5 has nonreciprocal nextwall
        // references. Preserve shipped topology while bounding every index.
        if (wall.next == i) return fail(error, "Map portal references itself");
    }
    for (int i = 0; i < sprites; ++i)
    {
        uint8_t data[44];
        if (!record(data, sizeof(data), (revision * 44) | HeaderNew)) return fail(error, "Map sprites are truncated");
        const size_t extended = position;
        if (!tile(s16(data + 14)) || s16(data + 24) < 0 || s16(data + 24) >= sectors
            || s16(data + 26) < 0 || unsigned(s16(data + 26)) >= MaxStatus
            || !extra(s16(data + 42), xSprite, 56, xSprites, 16384))
            return fail(error, "Map sprite references or records are invalid");
        if (s16(data + 42) > 0)
        {
            const int target = s16(bytes.data() + extended + 30), burnSource = s16(bytes.data() + extended + 46);
            if (target < -1 || (target >= 0 && unsigned(target) >= MaxSprites) || burnSource < -1
                || (burnSource >= 0 && unsigned(burnSource) >= MaxSprites))
                return fail(error, "Map extended sprite references are invalid");
        }
        // Match dbLoadMap's marker propagation before checking the marker
        // references that the legacy moving-sector initializer dereferences.
        const int owner = s16(data + 30), type = s16(data + 38);
        if (s16(data + 26) == 10 && owner >= 0 && owner < sectors && sectorInfo[owner].extended)
        {
            if (type == 3 || type == 5 || type == 8) sectorInfo[owner].marker0 = i;
            else if (type == 4) sectorInfo[owner].marker1 = i;
        }
    }
    for (const auto& sector : sectorInfo)
        if (sector.extended && sector.type >= 614 && sector.type <= 617)
        {
            if (sector.marker0 < 0 || sector.marker0 >= sprites
                || ((sector.type == 614 || sector.type == 616) && (sector.marker1 < 0 || sector.marker1 >= sprites)))
                return fail(error, "Map moving sector is missing its markers");
        }
    if (position != end) return fail(error, "Map has unexpected trailing data");
    return true;
}

bool netMapRead(const char* name, std::vector<uint8_t>& bytes, std::string& error)
{
    bytes.clear(); error.clear();
    if (!validCanonical(name)) return fail(error, "Map name is invalid");
#ifdef NETMAP_STANDALONE
    std::string filename = name;
    if (normalized(name).size() == filename.size()) filename += ".MAP";
    if (!readDisk(filename, bytes, error)) return false;
#else
    if (!readResource(name, bytes, error)) return false;
#endif
    if (!netMapValidate(bytes, u32(bytes.data() + bytes.size() - 4), error)) { bytes.clear(); return false; }
    return true;
}

bool netMapPrepare(const char* name, uint32_t expectedCRC, std::string& error)
{
    netMapClear();
    if (!validCanonical(name)) return fail(error, "Map name is invalid");
    Bytes bytes;
    // A server's absolute or traversing map name is an opaque identifier. It
    // may resolve from the checksum cache, but must not direct client reads.
    if (safeLocalName(name) && netMapRead(name, bytes, error) && netMapValidate(bytes, expectedCRC, error))
    {
#ifndef NETMAP_STANDALONE
        LOG_F(INFO, "NET map local match crc=%08x", expectedCRC);
#endif
        return true;
    }
    bytes.clear();
    if (!readCache(expectedCRC, bytes, error) || !netMapValidate(bytes, expectedCRC, error)) return false;
    std::string path;
    if (!cacheAbsolute(expectedCRC, path, error)) return false;
    aliasName = normalized(name);
    aliasPath = path.substr(0, path.size() - 4);
#ifndef NETMAP_STANDALONE
    LOG_F(INFO, "NET map cache hit crc=%08x", expectedCRC);
#endif
    error.clear();
    return true;
}

bool netMapInstall(const char* name, uint32_t expectedCRC, const std::vector<uint8_t>& bytes, std::string& error)
{
    netMapClear();
    if (!validCanonical(name)) return fail(error, "Map name is invalid");
    if (!netMapValidate(bytes, expectedCRC, error)) return false;
    std::string path;
    if (!cacheAbsolute(expectedCRC, path, error) || !writeCache(expectedCRC, bytes, error)) return false;
    aliasName = normalized(name);
    aliasPath = path.substr(0, path.size() - 4);
    error.clear();
    return true;
}

void netMapClear() { aliasName.clear(); aliasPath.clear(); }
const char* netMapResolve(const char* path)
{
    return !aliasName.empty() && normalized(path) == aliasName ? aliasPath.c_str() : path;
}
