// SPDX-License-Identifier: GPL-2.0-or-later
// Portable addon descriptions and a streamed, content-addressed download cache.
#pragma once
#include "../../blood/src/netassets.h"
#include <memory>

namespace rnetaddon
{
constexpr uint32_t ProfileVersion = 1;
constexpr size_t MaxFiles = 8192;
constexpr size_t MaxSelections = 256;
constexpr size_t MaxPath = 200;
constexpr size_t MaxProfileBytes = 2u * 1024u * 1024u;
constexpr uint64_t MaxFileBytes = uint64_t(1) << 30;
constexpr uint64_t MaxTotalBytes = uint64_t(4) << 30;
constexpr size_t ChunkBytes = 16u * 1024u;

enum class SelectionKind : uint8_t
{
    Archive = 1, PreloadArchive, Definition, DefinitionModule, Script, ScriptModule
};
struct Selection
{
    SelectionKind kind = SelectionKind::Archive;
    std::string path;
};
struct File
{
    std::string path;
    uint64_t size = 0;
    netassets::Digest md5 = {};
};
struct Profile
{
    std::string name;
    std::vector<Selection> selections;
    std::vector<File> files;
};

bool validPath(const std::string &path);
bool allowedFile(const std::string &path);
bool validate(const Profile &profile, std::string &error);
bool encode(const Profile &profile, std::vector<uint8_t> &bytes, std::string &error);
bool decode(netwire::Reader &reader, Profile &profile, std::string &error);
// Display names are excluded: an installation folder may have a different
// label on each machine while its files and ordered resource selections match.
std::string identity(const Profile &profile);

// Local source paths are selected by the engine, never accepted from a peer.
// inspectFile preserves the supplied portable File::path.
bool inspectFile(const std::string &sourcePath, File &file, std::string &error);
bool matches(const std::string &directory, const Profile &profile, std::string &error);
bool readChunk(const std::string &sourcePath, const File &file, uint64_t offset,
               size_t maximum, std::vector<uint8_t> &bytes, std::string &error);

// Never extracts archives, modifies installed addons, follows cache symlinks, or
// accepts loose executable/console-script files. Each file is streamed in order and
// checked before publication; a profile marker is published last.
class Cache
{
    struct State;
    std::unique_ptr<State> state_;
public:
    Cache();
    ~Cache();
    Cache(const Cache &) = delete;
    Cache &operator=(const Cache &) = delete;
    bool begin(const Profile &profile, std::string &error);
    bool hasFile(size_t index) const;
    size_t nextMissingFile() const; // profile.files.size() when complete
    bool write(size_t index, uint64_t offset, const void *bytes, size_t length, std::string &error);
    bool finishFile(size_t index, std::string &error);
    bool finish(std::string &error);
    void cancel();
    const std::string &directory() const;
};

// A complete cache entry can be read after restarting. The manifest and all
// files are revalidated; partial transfers never qualify as installed addons.
bool loadCached(const std::string &directory, Profile &profile, std::string &error);
}
