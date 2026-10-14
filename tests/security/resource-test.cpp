// SPDX-License-Identifier: GPL-2.0-or-later
// Real Resource registration, lookup and reads; only file I/O and logging are faked.
#include "resource.h"
#include "crc32.h"
#include "log.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {
unsigned checks = 0;
unsigned opens = 0;
std::map<std::string, std::string> files;
std::string activeFile;
std::vector<std::string> messages;
void check(bool result, const char *expression, int line)
{
    ++checks;
    if (!result) { std::fprintf(stderr, "FAIL line %d: %s\n", line, expression); std::exit(1); }
}
#define CHECK(value) check(bool(value), #value, __LINE__)

struct Fixture : Resource
{
    Fixture() { Init(nullptr); }
    ~Fixture()
    {
        // Resource's normal shutdown retains these indexes until process exit.
        Free(indexName); indexName = nullptr;
        Free(indexId); indexId = nullptr;
    }
};

void rejected(Fixture &resource, const char *name, const char *type, const char *directory = nullptr)
{
    unsigned const oldCount = resource.count, oldOpens = opens;
    size_t const oldMessages = messages.size();
    resource.AddExternalResource(name, type, 123, 0, directory);
    CHECK(resource.count == oldCount);
    CHECK(opens == oldOpens);
    CHECK(messages.size() == oldMessages + 1);
    CHECK(messages.back().find("Rejected") != std::string::npos);
}

void maliciousInputs()
{
    Fixture resource;
    // The pre-fix formatter truncated this existing filename, then strcpy used
    // the ORIGINAL DEF name. This fixture reaches the actual vulnerable copy.
    std::string const hugeName(4096, 'n'), hugeType(4096, 't');
    files[hugeName.substr(0, BMAX_PATH - 2)] = "original exploit prerequisite";
    rejected(resource, hugeName.c_str(), "QAV");
    files[("x." + hugeType).substr(0, BMAX_PATH - 2)] = "type overflow prerequisite";
    rejected(resource, "x", hugeType.c_str());
    rejected(resource, nullptr, "QAV");
    rejected(resource, "x", nullptr);
    for (size_t length : {size_t(BMAX_PATH), size_t(BMAX_PATH + 1), size_t(65536)})
    {
        std::string const longValue(length, 'x');
        rejected(resource, longValue.c_str(), "");
        rejected(resource, "", longValue.c_str());
        CHECK(resource.Lookup(longValue.c_str(), "QAV") == nullptr);
        CHECK(resource.Lookup("weapon", longValue.c_str()) == nullptr);
        CHECK(resource.Lookup(123u, longValue.c_str()) == nullptr);
        unsigned const oldCount = resource.count;
        char data = 1;
        resource.AddFromBuffer(longValue.c_str(), "DAT", &data, 1);
        resource.AddFromBuffer("x", longValue.c_str(), &data, 1);
        CHECK(resource.count == oldCount);
    }
    // Both fields fit separately, but their joined key or filename does not.
    std::string const half(BMAX_PATH / 2, 'h');
    rejected(resource, half.c_str(), half.c_str());
    rejected(resource, std::string(BMAX_PATH - 2, 'n').c_str(), "t");
    rejected(resource, "x", std::string(BMAX_PATH - 2, 't').c_str());
    rejected(resource, "weapon", "QAV", hugeName.c_str());
    rejected(resource, "weapon", "QAV", std::string(BMAX_PATH - 11, 'd').c_str());
    CHECK(resource.Lookup(nullptr, "QAV") == nullptr);
    CHECK(resource.Lookup("weapon", nullptr) == nullptr);
    CHECK(resource.Lookup(123u, nullptr) == nullptr);
}

void normalResources()
{
    Fixture resource;
    files["weapon.qav"] = "weapon data";
    resource.AddExternalResource("weapon", "qav", 100);
    CHECK(resource.count == 2);
    DICTNODE *node = resource.Lookup(100u, "qav");
    CHECK(node != nullptr);
    CHECK(std::string(node->name) == "WEAPON");
    CHECK(std::string(node->type) == "QAV");
    CHECK(std::string(node->path) == "weapon.qav");
    CHECK(node->size == files["weapon.qav"].size());
    CHECK(resource.Lookup("weapon", "qav") != nullptr);
    char data[32] = {};
    resource.Read(node, data);
    CHECK(std::string(data) == "weapon data");
    resource.AddExternalResource("weapon", "qav", 101);
    CHECK(resource.count == 2); // Existing external name is not reassigned.
    CHECK(resource.Lookup(101u, "qav") == nullptr);

    files["replacement.qav"] = "replacement data";
    resource.AddExternalResource("replacement", "qav", 100);
    node = resource.Lookup(100u, "qav");
    CHECK(node != nullptr && std::string(node->path) == "replacement.qav");
    CHECK(std::string(node->oldname) == "WEAPON");
    CHECK(node->size == files["replacement.qav"].size());
    // Dictionary growth and reindexing must preserve name and ID lookups.
    for (unsigned i = 0; i < 40; ++i)
    {
        std::string const name = "resource" + std::to_string(i);
        files[name + ".dat"] = name;
        resource.AddExternalResource(name.c_str(), "dat", 200 + i);
    }
    for (unsigned i = 0; i < 40; ++i)
    {
        std::string const name = "resource" + std::to_string(i);
        CHECK(resource.Lookup(name.c_str(), "dat") != nullptr);
        CHECK(resource.Lookup(200 + i, "dat") != nullptr);
    }
}

void maximumLengths()
{
    for (auto const &key : std::vector<std::pair<std::string, std::string>>{
             {std::string(BMAX_PATH - 1, 'n'), ""},
             {std::string(BMAX_PATH - 5, 'n'), "dat"},
             {"n", std::string(BMAX_PATH - 3, 't')}})
    {
        Fixture resource;
        std::string const filename = key.first + (key.second.empty() ? "" : ".") + key.second;
        CHECK(filename.size() == BMAX_PATH - 1);
        files[filename] = "boundary data";
        size_t const oldMessages = messages.size();
        resource.AddExternalResource(key.first.c_str(), key.second.c_str(), 42);
        CHECK(resource.count == 2);
        CHECK(messages.size() == oldMessages);
        auto *node = resource.Lookup(42u, key.second.c_str());
        CHECK(node != nullptr);
        CHECK(std::string(node->path) == filename);
        char data[32] = {};
        resource.Read(node, data);
        CHECK(activeFile == filename);
        CHECK(std::string(data) == "boundary data");
    }
    Fixture resource;
    std::string const directory(BMAX_PATH - 12, 'd');
    files["weapon.QAV"] = "directory data";
    files[directory + "/weapon.QAV"] = "directory data";
    resource.AddExternalResource("weapon", "QAV", 123, 0, directory.c_str());
    auto *node = resource.Lookup(123u, "QAV");
    CHECK(node != nullptr && std::strlen(node->path) == BMAX_PATH - 1);
    char data[32] = {};
    resource.Read(node, data);
    CHECK(activeFile == directory + "/weapon.QAV");
    CHECK(std::string(data) == "directory data");
}

void hashCompatibility()
{
    Fixture resource;
    // Empty indexes expose the initial hash bucket. Check the old joined byte
    // representation, including direct Probe callers whose keys exceed 255.
    for (auto const &name : {std::string(""), std::string("WEAPON"), std::string(BMAX_PATH - 1, 'N'), std::string(4096, 'N')})
    {
        for (auto const &type : {std::string(""), std::string("QAV"), std::string(4096, 'T')})
        {
            std::string const joined = type + name;
            auto const hash = Bcrc32(joined.data(), joined.size(), 0) & (resource.buffSize - 1);
            CHECK(resource.Probe(name.c_str(), type.c_str()) == resource.indexName + hash);
        }
    }
    for (unsigned id : {0u, 1u, 123u, 0x80000000u, 0xffffffffu})
    {
        for (auto const &type : {std::string(""), std::string("DAT"), std::string(4096, 'T')})
        {
            std::string joined(reinterpret_cast<const char *>(&id), sizeof(id));
            joined += type;
            auto const hash = Bcrc32(joined.data(), joined.size(), 0) & (resource.buffSize - 1);
            CHECK(resource.Probe(id, type.c_str()) == resource.indexId + hash);
        }
    }
}
}

buildvfs_kfd kopen4loadfrommod(const char *name, char)
{
    ++opens;
    activeFile = name;
    return files.count(activeFile) ? 1 : buildvfs_kfd_invalid;
}
int32_t kfilelength(buildvfs_kfd) { return files.at(activeFile).size(); }
int32_t kread(buildvfs_kfd, void *data, int32_t count)
{
    auto const &contents = files.at(activeFile);
    CHECK(count >= 0 && size_t(count) <= contents.size());
    std::memcpy(data, contents.data(), count);
    return count;
}
int32_t klseek(buildvfs_kfd, int32_t, int32_t) { return 0; }
void kclose(buildvfs_kfd) {}
void _SetErrorLoc(const char *, int) {}
void _ThrowError(const char *format, ...)
{
    va_list arguments; va_start(arguments, format);
    std::vfprintf(stderr, format, arguments); va_end(arguments);
    std::abort();
}
void __dassert(const char *expression, const char *file, int line)
{
    std::fprintf(stderr, "Assertion: %s (%s:%d)\n", expression, file, line);
    std::abort();
}
namespace loguru {
Verbosity current_verbosity_cutoff() { return Verbosity_MAX; }
void log(Verbosity, const char *, unsigned, const char *format, ...)
{
    char buffer[512];
    va_list arguments; va_start(arguments, format);
    std::vsnprintf(buffer, sizeof(buffer), format, arguments); va_end(arguments);
    messages.emplace_back(buffer);
}
}

int main()
{
    initcrc32table();
    maliciousInputs();
    normalResources();
    maximumLengths();
    hashCompatibility();
    std::printf("Resource security regression checks: %u passed\n", checks);
}
