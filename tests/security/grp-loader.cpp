// SPDX-License-Identifier: GPL-2.0-or-later
// Exercise the actual archive mounting code, including failed/short OS reads.
#include "compat.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static unsigned readCalls = 0, shortReadCall = 0;
static ssize_t archiveRead(int fd, void *buffer, size_t length)
{
    if (++readCalls == shortReadCall && length)
        --length;
    return read(fd, buffer, length);
}
#undef Bread
#define Bread archiveRead
#include "../../source/build/src/vfs.cpp"

namespace loguru {
Verbosity current_verbosity_cutoff() { return Verbosity_OFF; }
void log(Verbosity, const char *, unsigned, const char *, ...) {}
[[noreturn]] void log_and_abort(int, const char *expression, const char *file, unsigned line, const char *, ...)
{ std::fprintf(stderr, "Allocator assertion: %s (%s:%u)\n", expression, file, line); std::abort(); }
}

namespace {
unsigned checks = 0;
using Bytes = std::vector<char>;
void check(bool value, const char *expression, int line)
{
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL line %d: %s\n", line, expression); std::exit(1); }
}
#define CHECK(expr) check(bool(expr), #expr, __LINE__)
void u32(Bytes &bytes, size_t offset, uint32_t value)
{
    CHECK(offset + 4 <= bytes.size());
    for (unsigned i = 0; i < 4; ++i) bytes[offset+i] = char(value >> (8*i));
}
void writeFile(const char *name, const Bytes &bytes)
{
    FILE *file = std::fopen(name, "wb"); CHECK(file);
    if (!bytes.empty()) CHECK(std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size());
    CHECK(std::fclose(file) == 0);
}
Bytes grp(const std::vector<std::string> &names, const std::vector<Bytes> &contents)
{
    CHECK(names.size() == contents.size());
    Bytes bytes(16 + names.size()*16);
    std::memcpy(bytes.data(), "KenSilverman", 12); u32(bytes, 12, names.size());
    for (size_t i = 0; i < names.size(); ++i)
    {
        CHECK(names[i].size() <= 12);
        std::memcpy(bytes.data()+16+i*16, names[i].data(), names[i].size());
        u32(bytes, 28+i*16, contents[i].size());
    }
    for (auto const &content : contents) bytes.insert(bytes.end(), content.begin(), content.end());
    return bytes;
}
Bytes ssi(unsigned version)
{
    size_t const headerSize = version == 2 ? 267 : 254;
    Bytes bytes(headerSize + 121 + 3);
    u32(bytes, 0, version); u32(bytes, 4, 1);
    bytes[8] = 4; std::memcpy(bytes.data()+9, "game", 4);
    if (version == 2) { bytes[41] = 3; std::memcpy(bytes.data()+42, "exe", 3); }
    bytes[headerSize] = 8; std::memcpy(bytes.data()+headerSize+1, "SSI.TEST", 8);
    u32(bytes, headerSize+13, 3);
    std::memcpy(bytes.data()+headerSize+121, "ssi", 3);
    return bytes;
}
std::string readMember(const char *name)
{
    int const fd = kopen4load(name, 0); CHECK(fd >= 0);
    int const length = kfilelength(fd); CHECK(length >= 0);
    std::string result(length, '\0');
    CHECK(kread(fd, &result[0], length) == length);
    kclose(fd); return result;
}
int nextDescriptor()
{
    int const descriptor = open("/dev/null", O_RDONLY); CHECK(descriptor >= 0);
    CHECK(close(descriptor) == 0); return descriptor;
}
void reject(const Bytes &bytes, unsigned injectedRead = 0)
{
    writeFile("bad.grp", bytes);
    int const previousCount = numgroupfiles;
    char *const previousList = gfilelist[0];
    int const previousDescriptor = nextDescriptor();
    readCalls = 0; shortReadCall = injectedRead;
    CHECK(initgroupfile("bad.grp") == -1);
    shortReadCall = 0;
    CHECK(numgroupfiles == previousCount);
    CHECK(gfilelist[0] == previousList);
    CHECK(groupfil[previousCount] == -1);
    CHECK(gfilelist[previousCount] == nullptr);
    CHECK(gfileoffs[previousCount] == nullptr);
    CHECK(groupname[previousCount] == nullptr);
    CHECK(nextDescriptor() == previousDescriptor);
    CHECK(readMember("BASE.TEST") == "base");
}
void unmountLast()
{
    CHECK(numgroupfiles > 1);
    int const group = --numgroupfiles;
    kclose_grp(group);
    DO_FREE_AND_NULL(gfilelist[group]); DO_FREE_AND_NULL(gfileoffs[group]); DO_FREE_AND_NULL(groupname[group]);
}
}

int main(int argc, char **argv)
{
    engineCreateAllocator();
    CHECK(argc == 2); CHECK(chdir(argv[1]) == 0);
    Bytes const base = grp({"BASE.TEST"}, {{'b', 'a', 's', 'e'}});
    writeFile("base.grp", base); CHECK(initgroupfile("base.grp") == 0);
    CHECK(readMember("base.test") == "base");

    // Valid zero-entry archives, maximum-length names, empty members and
    // trailing padding remain compatible with the legacy format.
    writeFile("empty.grp", grp({}, {})); CHECK(initgroupfile("empty.grp") == 1); unmountLast();
    auto valid = grp({"123456789012", "ZERO"}, {{'o', 'k'}, {}});
    valid.push_back('!'); writeFile("valid.grp", valid);
    CHECK(initgroupfile("valid.grp") == 1);
    CHECK(readMember("123456789012") == "ok"); CHECK(readMember("ZERO").empty()); unmountLast();

    // Every partial header/index/data cutoff is rejected. High-bit counts and
    // values that wrapped old shifts/additions must never reach allocation.
    for (size_t length = 0; length < base.size(); ++length)
        reject(Bytes(base.begin(), base.begin()+length));
    for (uint32_t count : {2u, 0x08000000u, 0x10000000u, 0x7fffffffu, 0x80000000u, 0xffffffffu})
    { auto bytes = base; u32(bytes, 12, count); reject(bytes); }
    for (uint32_t length : {5u, 0x7fffffffu, 0x80000000u, 0xffffffffu})
    { auto bytes = base; u32(bytes, 28, length); reject(bytes); }
    auto sum = grp({"ONE", "TWO"}, {{'a'}, {'b'}});
    u32(sum, 28, 2); reject(sum); // Both entries fit alone, their sum does not.
    reject(base, 2); // GRP header short read, despite a complete file on disk.
    reject(base, 3); // GRP directory short read after size validation.

    for (unsigned version : {1u, 2u})
    {
        size_t const headerSize = version == 2 ? 267 : 254;
        auto const bytes = ssi(version);
        writeFile("valid.ssi", bytes); CHECK(initgroupfile("valid.ssi") == 1);
        CHECK(readMember("SSI.TEST") == "ssi"); unmountLast();
        auto empty = bytes; empty.resize(headerSize); u32(empty, 4, 0);
        writeFile("empty.ssi", empty); CHECK(initgroupfile("empty.ssi") == 1); unmountLast();
        for (size_t length = 0; length < bytes.size(); ++length)
            reject(Bytes(bytes.begin(), bytes.begin()+length));
        for (uint32_t count : {2u, 0x08000000u, 0x10000000u, 0x7fffffffu, 0x80000000u, 0xffffffffu})
        { auto bad = bytes; u32(bad, 4, count); reject(bad); }
        for (uint32_t length : {4u, 0x7fffffffu, 0x80000000u, 0xffffffffu})
        { auto bad = bytes; u32(bad, headerSize+13, length); reject(bad); }
        for (size_t offset : {size_t(8), headerSize})
        { auto bad = bytes; bad[offset] = char(0xff); reject(bad); }
        auto padding = bytes; padding[40] = 1; reject(padding);
        reject(bytes, 2); reject(bytes, 3); reject(bytes, 4);
    }

    // The exact largest representable member end offset remains supported.
    auto limit = grp({"LIMIT"}, {{}}); u32(limit, 28, uint32_t(INT32_MAX)-32u);
    writeFile("limit.grp", limit);
    int limitFd = open("limit.grp", O_WRONLY); CHECK(limitFd >= 0);
    CHECK(ftruncate(limitFd, INT32_MAX) == 0); CHECK(close(limitFd) == 0);
    CHECK(initgroupfile("limit.grp") == 1);
    limitFd = kopen4load("LIMIT", 0); CHECK(limitFd >= 0);
    CHECK(kfilelength(limitFd) == INT32_MAX-32); kclose(limitFd);
    unmountLast(); CHECK(unlink("limit.grp") == 0);

    // Archive lengths must not narrow through signed 32 bits, including a
    // 4-GiB length that would otherwise wrap back to a plausible small value.
    for (int64_t length : {int64_t(INT32_MAX)+1, (int64_t(1)<<32)+32})
    {
        writeFile("large.grp", grp({}, {}));
        int const fd = open("large.grp", O_WRONLY); CHECK(fd >= 0);
        CHECK(ftruncate(fd, length) == 0); CHECK(close(fd) == 0);
        CHECK(initgroupfile("large.grp") == -1); CHECK(numgroupfiles == 1);
        CHECK(unlink("large.grp") == 0);
    }

    // Embedded group sizes are bounded by the member, not the larger root
    // archive. A successful nested mount must still return the correct bytes.
    auto child = grp({"CHILD.TEST"}, {{'c'}});
    auto invalidChild = child; u32(invalidChild, 28, 2);
    writeFile("parent.grp", grp({"CHILD.GRP", "BAD.GRP", "TAIL"}, {child, invalidChild, Bytes(100, 't')}));
    CHECK(initgroupfile("parent.grp") == 1);
    CHECK(initgroupfile("CHILD.GRP") == 2); CHECK(readMember("CHILD.TEST") == "c"); unmountLast();
    CHECK(unlink("bad.grp") == 0);
    CHECK(initgroupfile("BAD.GRP") == -1); CHECK(numgroupfiles == 2);
    CHECK(readMember("TAIL") == std::string(100, 't')); unmountLast();

    // Real ZIP smoke test: a stored member remains mountable through the ZIP
    // dispatch, while truncated signature prefixes do not mount a group.
    // Python zipfile creates the fixture in the runner's temporary directory.
    char const zipSignature[] = "PK\3\4";
    for (size_t length = 0; length < 4; ++length)
        reject(Bytes(zipSignature, zipSignature+length));
    readCalls = 0; shortReadCall = 1;
    CHECK(initgroupfile("valid.zip") == -1); CHECK(numgroupfiles == 1);
    shortReadCall = 0;
    CHECK(initgroupfile("valid.zip") == MAXGROUPFILES);
    CHECK(numgroupfiles == 1); CHECK(readMember("ZIP.TEST") == "zip");
    kzuninit();

    uninitgroupfile();
    CHECK(numgroupfiles == 0);
    engineDestroyAllocator();
    std::printf("GRP/SSI loader: %u checks passed\n", checks);
    return 0;
}
