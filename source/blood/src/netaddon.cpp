// SPDX-License-Identifier: GPL-2.0-or-later
#include "netaddon.h"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <limits>
#include <set>
#include <sys/stat.h>
#ifdef _WIN32
# include <windows.h>
# include <direct.h>
#else
# include <fcntl.h>
# include <unistd.h>
#endif

namespace netaddon
{
namespace
{
constexpr char CacheRoot[] = ".nblood-addon-cache";
constexpr char Marker[] = ".netblood-profile";
bool fail(std::string &error, const std::string &why) { error = why; return false; }
std::string folded(std::string text)
{
    for (char &c : text) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return text;
}
std::string extension(const std::string &path)
{
    const size_t dot = path.find_last_of('.'), slash = path.find_last_of('/');
    return dot == std::string::npos || (slash != std::string::npos && dot < slash)
        ? std::string() : folded(path.substr(dot + 1));
}
bool selectionType(const Selection &selection)
{
    const auto ext = extension(selection.path);
    switch (selection.kind)
    {
    case SelectionKind::Ini: return ext == "ini";
    case SelectionKind::Rff: case SelectionKind::Snd: return ext == "rff";
    // The engine appends its own %03i.ART format suffix to this prefix. A
    // server-provided percent directive would become a printf format string.
    case SelectionKind::Art: return selection.path.find('%') == std::string::npos;
    case SelectionKind::Definition: case SelectionKind::DefinitionModule: return ext == "def";
    case SelectionKind::Archive: case SelectionKind::PreloadArchive:
        return ext == "grp" || ext == "zip" || ext == "pk3" || ext == "pk4" || ext == "ssi";
    }
    return false;
}

#ifdef _WIN32
struct Handle
{
    HANDLE value = INVALID_HANDLE_VALUE;
    Handle() = default;
    explicit Handle(HANDLE h) : value(h) {}
    ~Handle() { reset(); }
    Handle(Handle &&other) noexcept : value(other.value) { other.value = INVALID_HANDLE_VALUE; }
    Handle &operator=(Handle &&other) noexcept
    { if (this != &other) { reset(); value = other.value; other.value = INVALID_HANDLE_VALUE; } return *this; }
    void reset() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); value = INVALID_HANDLE_VALUE; }
    explicit operator bool() const { return value != INVALID_HANDLE_VALUE; }
};
#else
struct Handle
{
    int value = -1;
    Handle() = default;
    explicit Handle(int fd) : value(fd) {}
    ~Handle() { reset(); }
    Handle(Handle &&other) noexcept : value(other.value) { other.value = -1; }
    Handle &operator=(Handle &&other) noexcept
    { if (this != &other) { reset(); value = other.value; other.value = -1; } return *this; }
    void reset() { if (value >= 0) close(value); value = -1; }
    explicit operator bool() const { return value >= 0; }
};
#endif
struct Directory
{
    Handle handle;
    std::string path;
    explicit operator bool() const { return bool(handle); }
};

Directory openDirectory(const std::string &path)
{
    Directory result;
    result.path = path;
#ifdef _WIN32
    result.handle = Handle(CreateFileA(path.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    BY_HANDLE_FILE_INFORMATION info{};
    if (result.handle && (!GetFileInformationByHandle(result.handle.value, &info)
        || !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)))
        result.handle.reset();
#else
    result.handle = Handle(open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
#endif
    return result;
}
Directory childDirectory(const Directory &parent, const std::string &leaf, bool create)
{
    Directory result;
    if (!parent) return result;
    result.path = parent.path + "/" + leaf;
#ifdef _WIN32
    if (create && !CreateDirectoryA(result.path.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return result;
    return openDirectory(result.path);
#else
    if (create && mkdirat(parent.handle.value, leaf.c_str(), 0700) < 0 && errno != EEXIST) return result;
    result.handle = Handle(openat(parent.handle.value, leaf.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    return result;
#endif
}
Directory parentDirectory(const Directory &root, const std::string &path, std::string &leaf, bool create)
{
    // Duplicate the root descriptor, retaining the trusted directory if its
    // pathname is renamed while a transfer is in progress.
    Directory parent;
    parent.path = root.path;
#ifdef _WIN32
    parent = openDirectory(root.path);
#else
    if (root) parent.handle = Handle(dup(root.handle.value));
#endif
    size_t start = 0, slash;
    while (parent && (slash = path.find('/', start)) != std::string::npos)
    {
        parent = childDirectory(parent, path.substr(start, slash - start), create);
        start = slash + 1;
    }
    leaf = path.substr(start);
    return parent;
}
Handle openFile(const Directory &directory, const std::string &leaf, bool create)
{
    if (!directory) return Handle();
#ifdef _WIN32
    const std::string path = directory.path + "/" + leaf;
    return Handle(CreateFileA(path.c_str(), create ? GENERIC_WRITE : GENERIC_READ,
        create ? 0 : FILE_SHARE_READ, nullptr, create ? CREATE_NEW : OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
#else
    return Handle(openat(directory.handle.value, leaf.c_str(), create
        ? O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC
        : O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600));
#endif
}
bool regularSize(const Handle &file, uint64_t &size)
{
    if (!file) return false;
#ifdef _WIN32
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file.value, &info)
        || (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return false;
    size = uint64_t(info.nFileSizeLow) | (uint64_t(info.nFileSizeHigh) << 32);
#else
    struct stat status{};
    if (fstat(file.value, &status) < 0 || !S_ISREG(status.st_mode) || status.st_size < 0) return false;
    size = uint64_t(status.st_size);
#endif
    return size <= MaxFileBytes;
}
bool readBytes(const Handle &file, void *bytes, size_t length)
{
    auto *output = static_cast<uint8_t *>(bytes);
    while (length)
    {
#ifdef _WIN32
        DWORD count = 0;
        if (!ReadFile(file.value, output, DWORD(length), &count, nullptr) || !count) return false;
#else
        const ssize_t count = read(file.value, output, length);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
#endif
        output += count;
        length -= size_t(count);
    }
    return true;
}
bool writeBytes(const Handle &file, const void *bytes, size_t length)
{
    auto *input = static_cast<const uint8_t *>(bytes);
    while (length)
    {
#ifdef _WIN32
        DWORD count = 0;
        if (!WriteFile(file.value, input, DWORD(length), &count, nullptr) || !count) return false;
#else
        const ssize_t count = write(file.value, input, length);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
#endif
        input += count;
        length -= size_t(count);
    }
    return true;
}
bool flush(const Handle &file)
{
#ifdef _WIN32
    return FlushFileBuffers(file.value) != 0;
#else
    return fsync(file.value) == 0;
#endif
}
void removeFile(const Directory &root, const std::string &leaf)
{
    if (!root || leaf.empty()) return;
#ifdef _WIN32
    DeleteFileA((root.path + "/" + leaf).c_str());
#else
    unlinkat(root.handle.value, leaf.c_str(), 0);
#endif
}
bool publish(const Directory &from, const std::string &temporary, const Directory &to, const std::string &leaf)
{
    if (!from || !to) return false;
    // Replacing a regular incomplete cache file is allowed. Reject symlink,
    // directory, and device destinations instead of following them.
#ifdef _WIN32
    const std::string target = to.path + "/" + leaf;
    const DWORD flags = GetFileAttributesA(target.c_str());
    if (flags != INVALID_FILE_ATTRIBUTES && (flags & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return false;
    return MoveFileExA((from.path + "/" + temporary).c_str(), target.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    struct stat status{};
    if (fstatat(to.handle.value, leaf.c_str(), &status, AT_SYMLINK_NOFOLLOW) == 0)
    { if (!S_ISREG(status.st_mode)) return false; }
    else if (errno != ENOENT) return false;
    return renameat(from.handle.value, temporary.c_str(), to.handle.value, leaf.c_str()) == 0;
#endif
}
std::string temporaryName()
{
    static unsigned serial = 0;
    char buffer[72];
#ifdef _WIN32
    const unsigned long process = GetCurrentProcessId();
#else
    const unsigned long process = getpid();
#endif
    std::snprintf(buffer, sizeof(buffer), ".download-%lu-%u.part", process, ++serial);
    return buffer;
}
bool digestFile(const Handle &handle, File &file, std::string &error)
{
    if (!regularSize(handle, file.size)) return fail(error, "Addon file is not a bounded regular file: " + file.path);
    netassets::MD5 digest;
    uint8_t buffer[64u * 1024u];
    uint64_t remaining = file.size;
    while (remaining)
    {
        const size_t length = size_t(std::min<uint64_t>(remaining, sizeof(buffer)));
        if (!readBytes(handle, buffer, length)) return fail(error, "Cannot read addon file: " + file.path);
        digest.update(buffer, length);
        remaining -= length;
    }
    uint64_t after;
    if (!regularSize(handle, after) || after != file.size) return fail(error, "Addon file changed while being read: " + file.path);
    file.md5 = digest.finish();
    error.clear();
    return true;
}
bool matchesFile(const Directory &root, const File &expected, std::string &error)
{
    std::string leaf;
    Directory parent = parentDirectory(root, expected.path, leaf, false);
    Handle handle = openFile(parent, leaf, false);
    File actual;
    actual.path = expected.path;
    return digestFile(handle, actual, error) && (actual.size == expected.size && actual.md5 == expected.md5
        ? true : fail(error, "Addon file does not match server: " + expected.path));
}
Handle openSource(const std::string &path)
{
#ifdef _WIN32
    return Handle(CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                             FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
#else
    return Handle(open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
#endif
}
}

bool validPath(const std::string &path)
{
    if (path.empty() || path.size() > MaxPath) return false;
    size_t start = 0;
    for (size_t i = 0; i <= path.size(); ++i)
    {
        if (i < path.size())
        {
            const unsigned char c = path[i];
            if (c < 32 || c == 127 || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"'
                || c == '<' || c == '>' || c == '|') return false;
        }
        if (i == path.size() || path[i] == '/')
        {
            const std::string part = path.substr(start, i - start);
            if (part.empty() || part[0] == '.' || part.back() == '.' || part.back() == ' ') return false;
            const std::string stem = folded(part.substr(0, part.find('.')));
            if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul"
                || stem == "clock$" || stem == "conin$" || stem == "conout$"
                || (stem.size() == 4 && (stem.compare(0, 3, "com") == 0 || stem.compare(0, 3, "lpt") == 0)
                    && stem[3] >= '0' && stem[3] <= '9')) return false;
            start = i + 1;
        }
    }
    return true;
}
bool allowedFile(const std::string &path)
{
    if (!validPath(path)) return false;
    static const std::set<std::string> extensions = {
        "ini", "def", "rff", "rfs", "art", "grp", "zip", "pk3", "pk4", "ssi",
        "map", "mhk", "pal", "plu", "dat", "bin", "raw", "sfx", "seq", "qav", "kvx", "vox",
        "png", "jpg", "jpeg", "tga", "pcx", "bmp", "dds", "md2", "md3",
        "wav", "ogg", "flac", "voc", "mid", "midi", "xm", "it", "mod", "s3m",
        "smk", "ogv", "ivf", "anm"
    };
    return extensions.count(extension(path)) != 0;
}
bool validate(const Profile &profile, std::string &error)
{
    if (!netassets::validText(profile.name, 128)) return fail(error, "Invalid addon display name");
    if (profile.files.size() > MaxFiles || profile.selections.size() > MaxSelections)
        return fail(error, "Addon has too many files or launch selections");
    std::set<SelectionKind> singular;
    for (const auto &selection : profile.selections)
    {
        if (!validPath(selection.path) || !selectionType(selection)) return fail(error, "Invalid addon launch selection: " + selection.path);
        if (selection.kind != SelectionKind::Archive && selection.kind != SelectionKind::PreloadArchive
            && selection.kind != SelectionKind::DefinitionModule
            && !singular.insert(selection.kind).second) return fail(error, "Duplicate addon launch selection");
    }
    std::set<std::string> files;
    uint64_t total = 0;
    size_t bytes = 12 + profile.name.size();
    for (const auto &selection : profile.selections) bytes += 3 + selection.path.size();
    for (const auto &file : profile.files)
    {
        if (!allowedFile(file.path)) return fail(error, "Unsupported or unsafe addon filename: " + file.path);
        const std::string key = folded(file.path);
        if (!files.insert(key).second) return fail(error, "Duplicate addon filename: " + file.path);
        if (file.size > MaxFileBytes || file.size > MaxTotalBytes - total) return fail(error, "Addon download exceeds size limits");
        total += file.size;
        bytes += 26 + file.path.size();
        if (bytes > MaxProfileBytes) return fail(error, "Addon profile exceeds size limit");
    }
    // A file cannot also be an ancestor directory, including case-only aliases.
    for (const auto &file : files)
        for (size_t slash = file.find('/'); slash != std::string::npos; slash = file.find('/', slash + 1))
            if (files.count(file.substr(0, slash))) return fail(error, "Conflicting addon file and directory names");
    error.clear();
    return true;
}
bool encode(const Profile &profile, std::vector<uint8_t> &bytes, std::string &error)
{
    if (!validate(profile, error)) return false;
    netwire::Writer writer;
    writer.u32(ProfileVersion);
    writer.text(profile.name, 128);
    writer.u16(uint16_t(profile.selections.size()));
    for (const auto &selection : profile.selections)
    { writer.u8(uint8_t(selection.kind)); writer.text(selection.path, MaxPath); }
    writer.u32(uint32_t(profile.files.size()));
    for (const auto &file : profile.files)
    {
        writer.text(file.path, MaxPath);
        writer.u32(uint32_t(file.size)); writer.u32(uint32_t(file.size >> 32));
        writer.bytes(file.md5.data(), file.md5.size());
    }
    bytes.swap(writer.data);
    error.clear();
    return true;
}
bool decode(netwire::Reader &reader, Profile &profile, std::string &error)
{
    if (reader.pos > reader.size || reader.size - reader.pos > MaxProfileBytes) return fail(error, "Addon profile exceeds size limit");
    if (reader.u32() != ProfileVersion) return fail(error, "Unsupported addon profile version");
    Profile candidate;
    candidate.name = reader.text(128);
    const size_t selections = reader.u16();
    if (!reader.ok || selections > MaxSelections) return fail(error, "Invalid addon launch selection count");
    for (size_t i = 0; i < selections; ++i)
    {
        Selection selection;
        selection.kind = SelectionKind(reader.u8());
        selection.path = reader.text(MaxPath);
        candidate.selections.push_back(std::move(selection));
    }
    const size_t files = reader.u32();
    if (!reader.ok || files > MaxFiles) return fail(error, "Invalid addon file count");
    for (size_t i = 0; i < files; ++i)
    {
        File file;
        file.path = reader.text(MaxPath);
        const uint32_t low = reader.u32();
        file.size = low | (uint64_t(reader.u32()) << 32);
        if (!reader.bytes(file.md5.data(), file.md5.size())) return fail(error, "Truncated addon file entry");
        candidate.files.push_back(std::move(file));
    }
    if (!reader.end()) return fail(error, "Trailing or truncated addon profile data");
    if (!validate(candidate, error)) return false;
    profile = std::move(candidate);
    return true;
}
std::string identity(const Profile &profile)
{
    std::vector<uint8_t> bytes;
    std::string error;
    if (!validate(profile, error)) return {};
    Profile canonical = profile;
    canonical.name = "NetBlood addon";
    if (!encode(canonical, bytes, error)) return {};
    netassets::MD5 digest;
    digest.update(bytes.data(), bytes.size());
    return netassets::hex(digest.finish());
}
bool inspectFile(const std::string &sourcePath, File &file, std::string &error)
{ return digestFile(openSource(sourcePath), file, error); }
bool matches(const std::string &directory, const Profile &profile, std::string &error)
{
    if (!validate(profile, error)) return false;
    Directory root = openDirectory(directory);
    if (!root) return fail(error, "Addon directory is missing or unsafe");
    for (const auto &file : profile.files) if (!matchesFile(root, file, error)) return false;
    error.clear();
    return true;
}
bool readChunk(const std::string &sourcePath, const File &file, uint64_t offset,
               size_t maximum, std::vector<uint8_t> &bytes, std::string &error)
{
    bytes.clear();
    if (file.size > MaxFileBytes || offset > file.size || maximum == 0 || maximum > ChunkBytes)
        return fail(error, "Invalid addon download offset or chunk size");
    Handle source = openSource(sourcePath);
    uint64_t size;
    if (!regularSize(source, size) || size != file.size) return fail(error, "Addon source file changed or is unavailable: " + file.path);
#ifdef _WIN32
    LARGE_INTEGER position;
    position.QuadPart = LONGLONG(offset);
    if (!SetFilePointerEx(source.value, position, nullptr, FILE_BEGIN)) return fail(error, "Cannot seek addon source file");
#else
    if (lseek(source.value, off_t(offset), SEEK_SET) < 0) return fail(error, "Cannot seek addon source file");
#endif
    bytes.resize(size_t(std::min<uint64_t>(maximum, file.size - offset)));
    if (!readBytes(source, bytes.data(), bytes.size())) { bytes.clear(); return fail(error, "Cannot read addon source file"); }
    error.clear();
    return true;
}

struct Cache::State
{
    Profile profile;
    std::vector<bool> present;
    Directory root;
    Handle file;
    std::string temporary;
    size_t active = std::numeric_limits<size_t>::max();
    uint64_t offset = 0;
    netassets::MD5 digest;
    ~State() { file.reset(); removeFile(root, temporary); }
};
Cache::Cache() = default;
Cache::~Cache() = default;
bool Cache::begin(const Profile &profile, std::string &error)
{
    cancel();
    if (!validate(profile, error)) return false;
    char cwd[4096];
#ifdef _WIN32
    if (!_getcwd(cwd, sizeof(cwd))) return fail(error, "Cannot locate addon cache directory");
#else
    if (!getcwd(cwd, sizeof(cwd))) return fail(error, "Cannot locate addon cache directory");
#endif
    Directory working = openDirectory(cwd);
    Directory cache = childDirectory(working, CacheRoot, true);
    std::unique_ptr<State> state(new State);
    state->root = childDirectory(cache, identity(profile), true);
    if (!state->root) return fail(error, "Cannot open a safe addon cache directory");
    state->profile = profile;
    state->present.resize(profile.files.size());
    for (size_t i = 0; i < profile.files.size(); ++i)
    {
        std::string ignored;
        state->present[i] = matchesFile(state->root, profile.files[i], ignored);
    }
    state_ = std::move(state);
    error.clear();
    return true;
}
bool Cache::hasFile(size_t index) const
{ return state_ && index < state_->present.size() && state_->present[index]; }
size_t Cache::nextMissingFile() const
{
    if (!state_) return 0;
    return size_t(std::find(state_->present.begin(), state_->present.end(), false) - state_->present.begin());
}
bool Cache::write(size_t index, uint64_t offset, const void *bytes, size_t length, std::string &error)
{
    if (!state_ || index >= state_->profile.files.size() || state_->present[index] || length > ChunkBytes
        || (length && !bytes)) return fail(error, "Invalid addon download chunk");
    State &state = *state_;
    const auto &expected = state.profile.files[index];
    if (offset > expected.size || length > expected.size - offset || (!length && expected.size))
        return fail(error, "Addon download chunk exceeds the declared file size");
    if (!state.file)
    {
        if (offset != 0) return fail(error, "Addon download must begin at offset zero");
        removeFile(state.root, state.temporary);
        state.temporary = temporaryName();
        state.file = openFile(state.root, state.temporary, true);
        if (!state.file) return fail(error, "Cannot create addon download temporary file");
        state.active = index;
        state.offset = 0;
        state.digest = netassets::MD5();
    }
    if (state.active != index || state.offset != offset) return fail(error, "Out-of-order addon download chunk");
    if (!writeBytes(state.file, bytes, length)) return fail(error, "Cannot write addon download (check free disk space)");
    state.digest.update(bytes, length);
    state.offset += length;
    error.clear();
    return true;
}
bool Cache::finishFile(size_t index, std::string &error)
{
    if (!state_ || index >= state_->profile.files.size()) return fail(error, "Invalid completed addon file");
    if (state_->present[index]) { error.clear(); return true; }
    State &state = *state_;
    const auto &expected = state.profile.files[index];
    if (!expected.size && !state.file && !write(index, 0, nullptr, 0, error)) return false;
    if (!state.file || state.active != index || state.offset != expected.size)
        return fail(error, "Addon file download is incomplete: " + expected.path);
    if (state.digest.finish() != expected.md5)
    {
        state.file.reset(); removeFile(state.root, state.temporary); state.temporary.clear();
        return fail(error, "Downloaded addon file failed its MD5 checksum: " + expected.path);
    }
    if (!flush(state.file)) return fail(error, "Cannot flush addon download to disk");
    state.file.reset();
    std::string leaf;
    Directory parent = parentDirectory(state.root, expected.path, leaf, true);
    if (!parent || !publish(state.root, state.temporary, parent, leaf)) return fail(error, "Cannot safely publish addon file: " + expected.path);
    state.temporary.clear();
    state.present[index] = true;
    state.active = std::numeric_limits<size_t>::max();
    error.clear();
    return true;
}
bool Cache::finish(std::string &error)
{
    if (!state_ || nextMissingFile() != state_->profile.files.size()) return fail(error, "Addon download is incomplete");
    State &state = *state_;
    for (const auto &file : state.profile.files) if (!matchesFile(state.root, file, error)) return false;
    std::vector<uint8_t> bytes;
    if (!encode(state.profile, bytes, error)) return false;
    const std::string temporary = temporaryName();
    Handle marker = openFile(state.root, temporary, true);
    bool ok = marker && writeBytes(marker, bytes.data(), bytes.size()) && flush(marker);
    marker.reset();
    if (ok) ok = publish(state.root, temporary, state.root, Marker);
    if (!ok) { removeFile(state.root, temporary); return fail(error, "Cannot publish completed addon profile"); }
    error.clear();
    return true;
}
void Cache::cancel() { state_.reset(); }
const std::string &Cache::directory() const
{
    static const std::string empty;
    return state_ ? state_->root.path : empty;
}
bool loadCached(const std::string &directory, Profile &profile, std::string &error)
{
    // Open both cache-controlled path components independently. Opening only
    // the final digest directory would follow a replaced cache-root symlink.
    const size_t slash = directory.find_last_of("/\\");
    if (slash == std::string::npos) return fail(error, "Invalid addon cache directory");
    const std::string cacheId = directory.substr(slash + 1);
    if (cacheId.size() != 32 || cacheId.find_first_not_of("0123456789abcdef") != std::string::npos)
        return fail(error, "Invalid addon cache identity");
    const std::string parentPath = directory.substr(0, slash);
    const size_t parentSlash = parentPath.find_last_of("/\\");
    if (parentPath.substr(parentSlash == std::string::npos ? 0 : parentSlash + 1) != CacheRoot)
        return fail(error, "Invalid addon cache root");
    Directory cache = openDirectory(parentPath);
    Directory root = childDirectory(cache, cacheId, false);
    Handle marker = openFile(root, Marker, false);
    uint64_t size;
    if (!regularSize(marker, size) || size > MaxProfileBytes) return fail(error, "Completed addon profile is missing or unsafe");
    std::vector<uint8_t> bytes(size_t(size), uint8_t(0));
    if (!readBytes(marker, bytes.data(), bytes.size())) return fail(error, "Cannot read completed addon profile");
    netwire::Reader reader(bytes.data(), bytes.size());
    Profile candidate;
    if (!decode(reader, candidate, error)) return false;
    if (cacheId != identity(candidate))
        return fail(error, "Addon cache identity does not match its profile");
    for (const auto &file : candidate.files) if (!matchesFile(root, file, error)) return false;
    profile = std::move(candidate);
    error.clear();
    return true;
}
}
