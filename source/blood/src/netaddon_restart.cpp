// SPDX-License-Identifier: GPL-2.0-or-later
#include "netaddon_restart.h"
#include "netaddon_engine.h"
#include "neturi.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <initializer_list>
#include <limits>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#endif

namespace
{
size_t const maxHandoffBytes = netaddon::MaxProfileBytes + 65536;
size_t const maxArguments = 600;
size_t const maxStringBytes = 4096;
std::string executable, startupDirectory, pendingFile, restoredProfile;
std::vector<std::string> startupArguments, restoredArguments;
std::vector<char const*> restoredPointers;
std::chrono::steady_clock::time_point readyAt;
bool noticeStarted = false;

bool fail(std::string& error, char const* message)
{
    error = message;
    return false;
}

bool cleanString(std::string const& value, size_t limit = maxStringBytes)
{
    if (value.size() > limit) return false;
    for (unsigned char c : value)
        if (c < 32 || c == 127) return false;
    return true;
}

bool profileIdentity(std::string const& id)
{
    if (id.size() != 32) return false;
    return std::all_of(id.begin(), id.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

std::string optionName(std::string const& argument)
{
    if (argument.empty() || (argument[0] != '-' && argument[0] != '/')) return {};
    std::string name = argument.substr(1, argument.find('=') == std::string::npos
        ? std::string::npos : argument.find('=') - 1);
    for (char& c : name) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return name;
}

bool listed(std::string const& name, std::initializer_list<char const*> names)
{
    return std::any_of(names.begin(), names.end(), [&](char const* item) { return name == item; });
}

bool optionValue(std::vector<std::string> const& arguments, size_t& index, std::string& value)
{
    size_t const equals = arguments[index].find('=');
    if (equals != std::string::npos) value = arguments[index].substr(equals + 1);
    else if (index + 1 < arguments.size()) value = arguments[++index];
    else return false;
    return cleanString(value);
}

bool resourceOptions(std::vector<std::string> const& input, std::vector<std::string>& output, std::string& error)
{
    if (input.empty() || input.size() > maxArguments)
        return fail(error, "Invalid addon activation options.");
    for (size_t i = 0; i < input.size(); ++i)
    {
        std::string const name = optionName(input[i]);
        if (name == "noautoload" && input[i].find('=') == std::string::npos)
        {
            output.push_back("-noautoload");
            continue;
        }
        if (!listed(name, {"game_dir", "ini", "g", "grp", "h", "mh", "rff", "snd", "art", "j"}))
            return fail(error, "The addon activation profile contains an unsupported option.");
        std::string value;
        if (!optionValue(input, i, value) || value.empty())
            return fail(error, "The addon activation profile contains an invalid path.");
        // Equals is required: the legacy option parser treats a separate absolute
        // POSIX path (starting with '/') as another command-line switch.
        output.push_back("-" + name + "=" + value);
    }
    return true;
}

void write32(std::vector<unsigned char>& bytes, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) bytes.push_back(static_cast<unsigned char>(value >> (i * 8)));
}

void writeString(std::vector<unsigned char>& bytes, std::string const& value)
{
    write32(bytes, static_cast<uint32_t>(value.size()));
    bytes.insert(bytes.end(), value.begin(), value.end());
}

struct Reader
{
    std::vector<unsigned char> const& bytes;
    size_t cursor = 0;
    explicit Reader(std::vector<unsigned char> const& value) : bytes(value) {}
    bool integer(uint32_t& value)
    {
        if (bytes.size() - cursor < 4) return false;
        value = 0;
        for (unsigned i = 0; i < 4; ++i) value |= uint32_t(bytes[cursor++]) << (i * 8);
        return true;
    }
    bool string(std::string& value)
    {
        uint32_t count;
        if (!integer(count) || count > maxStringBytes || count > bytes.size() - cursor) return false;
        value.assign(reinterpret_cast<char const*>(bytes.data() + cursor), count);
        cursor += count;
        return cleanString(value);
    }
};

std::string currentDirectory()
{
    char path[maxStringBytes + 1];
#if defined(_WIN32)
    return _getcwd(path, sizeof(path)) ? path : "";
#else
    return getcwd(path, sizeof(path)) ? path : "";
#endif
}

std::string comparablePath(std::string value)
{
    if (value.empty()) return {};
#if defined(_WIN32)
    std::replace(value.begin(), value.end(), '\\', '/');
    if (value[0] != '/' && !(value.size() > 1 && value[1] == ':')) value = startupDirectory + "/" + value;
    char resolved[maxStringBytes + 1];
    DWORD const count = GetFullPathNameA(value.c_str(), sizeof(resolved), resolved, nullptr);
    if (count && count < sizeof(resolved)) value.assign(resolved, count);
    std::replace(value.begin(), value.end(), '\\', '/');
    for (char& c : value) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
#else
    if (value[0] != '/') value = startupDirectory + "/" + value;
    char resolved[maxStringBytes + 1];
    if (realpath(value.c_str(), resolved)) value = resolved;
#endif
    while (value.size() > 1 && value.back() == '/') value.pop_back();
    return value;
}

void removeHandoff(std::string const& path)
{
    if (!path.empty()) std::remove(path.c_str());
}

std::string executablePath(char const* argument)
{
#if defined(_WIN32)
    (void) argument;
    char path[32768];
    DWORD const length = GetModuleFileNameA(nullptr, path, sizeof(path));
    if (length && length < sizeof(path)) return std::string(path, length);
#else
    char path[maxStringBytes + 1];
#if defined(__linux__)
    char const* appimage = std::getenv("APPIMAGE");
    if (appimage && *appimage == '/' && cleanString(appimage) && access(appimage, X_OK) == 0)
        return appimage;
    ssize_t const count = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (count > 0 && size_t(count) < sizeof(path) - 1) return std::string(path, size_t(count));
#elif defined(__APPLE__)
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) == 0)
    {
        char resolved[maxStringBytes + 1];
        if (realpath(path, resolved)) return resolved;
    }
#elif defined(__FreeBSD__)
    ssize_t const count = readlink("/proc/curproc/file", path, sizeof(path) - 1);
    if (count > 0 && size_t(count) < sizeof(path) - 1) return std::string(path, size_t(count));
#elif defined(__NetBSD__)
    ssize_t const count = readlink("/proc/curproc/exe", path, sizeof(path) - 1);
    if (count > 0 && size_t(count) < sizeof(path) - 1) return std::string(path, size_t(count));
#endif
    if (argument && std::strchr(argument, '/') && realpath(argument, path)) return path;
    char const* environment = std::getenv("PATH");
    std::string const search = environment ? environment : "";
    size_t start = 0;
    while (argument && start <= search.size())
    {
        size_t end = search.find(':', start);
        if (end == std::string::npos) end = search.size();
        std::string const candidate = (end == start ? "." : search.substr(start, end - start)) + "/" + argument;
        if (access(candidate.c_str(), X_OK) == 0 && realpath(candidate.c_str(), path)) return path;
        start = end + 1;
    }
#endif
    return {};
}

bool writeHandoff(std::vector<unsigned char> const& bytes, std::string& path, std::string& error)
{
    FILE* file = nullptr;
#if defined(_WIN32)
    char temporary[MAX_PATH + 1];
    DWORD const length = GetTempPathA(sizeof(temporary), temporary);
    if (!length || length >= sizeof(temporary))
        return fail(error, "Cannot create the private addon restart handoff.");
    // Explicitly permit only the current user, even if TEMP points to a shared
    // directory. These token/ACL APIs are available on Windows XP too.
    HANDLE token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return fail(error, "Cannot secure the private addon restart handoff.");
    DWORD tokenBytes = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &tokenBytes);
    std::vector<unsigned char> tokenData(tokenBytes);
    bool const tokenRead = tokenBytes && GetTokenInformation(token, TokenUser, tokenData.data(), tokenBytes, &tokenBytes);
    CloseHandle(token);
    if (!tokenRead) return fail(error, "Cannot identify the owner of the addon restart handoff.");
    PSID const owner = reinterpret_cast<TOKEN_USER*>(tokenData.data())->User.Sid;
    std::vector<unsigned char> aclBytes(sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD) + GetLengthSid(owner));
    PACL const acl = reinterpret_cast<PACL>(aclBytes.data());
    SECURITY_DESCRIPTOR descriptor;
    if (!InitializeAcl(acl, DWORD(aclBytes.size()), ACL_REVISION)
        || !AddAccessAllowedAce(acl, ACL_REVISION, FILE_ALL_ACCESS, owner)
        || !InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION)
        || !SetSecurityDescriptorDacl(&descriptor, TRUE, acl, FALSE)
        || !SetSecurityDescriptorControl(&descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED))
        return fail(error, "Cannot restrict access to the addon restart handoff.");
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), &descriptor, FALSE};
    HANDLE handle = INVALID_HANDLE_VALUE;
    for (unsigned attempt = 0; attempt < 128; ++attempt)
    {
        path = std::string(temporary) + "netblood-addon-restart-" + std::to_string(GetCurrentProcessId())
            + "-" + std::to_string(GetTickCount()) + "-" + std::to_string(attempt);
        handle = CreateFileA(path.c_str(), GENERIC_WRITE, 0, &security, CREATE_NEW,
            FILE_ATTRIBUTE_TEMPORARY, nullptr);
        if (handle != INVALID_HANDLE_VALUE || GetLastError() != ERROR_FILE_EXISTS) break;
        path.clear();
    }
    if (handle != INVALID_HANDLE_VALUE)
    {
        int const descriptor = _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_WRONLY | _O_BINARY);
        if (descriptor >= 0) { file = _fdopen(descriptor, "wb"); if (!file) _close(descriptor); }
        else CloseHandle(handle);
    }
    else path.clear();
#else
    char const* temporary = std::getenv("TMPDIR");
    std::string pattern = temporary && *temporary == '/' ? temporary : "/tmp";
    pattern += "/netblood-addon-restart-XXXXXX";
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back(0);
    int const descriptor = mkstemp(writable.data());
    if (descriptor >= 0)
    {
        path = writable.data();
        file = fdopen(descriptor, "wb");
        if (!file) close(descriptor);
    }
#endif
    if (!file)
    {
        removeHandoff(path);
        return fail(error, "Cannot write the private addon restart handoff.");
    }
    bool const wrote = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    bool const closed = std::fclose(file) == 0;
    if (!wrote || !closed)
    {
        removeHandoff(path);
        return fail(error, "Cannot finish writing the private addon restart handoff.");
    }
    return true;
}

bool readHandoff(std::string const& path, std::vector<unsigned char>& bytes, std::string& error)
{
    if (!cleanString(path) || path.empty()) return fail(error, "Invalid addon restart handoff path.");
    FILE* file = nullptr;
#if defined(_WIN32)
    HANDLE handle = CreateFileA(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle != INVALID_HANDLE_VALUE)
    {
        BY_HANDLE_FILE_INFORMATION info;
        if (GetFileInformationByHandle(handle, &info)
            && !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
            && !info.nFileSizeHigh && info.nFileSizeLow <= maxHandoffBytes)
        {
            int const descriptor = _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_RDONLY | _O_BINARY);
            if (descriptor >= 0) { file = _fdopen(descriptor, "rb"); if (!file) _close(descriptor); }
            else CloseHandle(handle);
        }
        else CloseHandle(handle);
    }
#else
    int const descriptor = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (descriptor >= 0)
    {
        struct stat info;
        if (fstat(descriptor, &info) == 0 && S_ISREG(info.st_mode) && info.st_uid == geteuid()
            && !(info.st_mode & 0077) && info.st_nlink == 1 && info.st_size >= 0
            && uint64_t(info.st_size) <= maxHandoffBytes)
        {
            file = fdopen(descriptor, "rb");
            if (!file) close(descriptor);
        }
        else close(descriptor);
    }
#endif
    if (!file) return fail(error, "Cannot read a private, regular addon restart handoff file.");
    bytes.resize(maxHandoffBytes + 1);
    size_t const count = std::fread(bytes.data(), 1, bytes.size(), file);
    bool const read = !std::ferror(file) && count <= maxHandoffBytes;
    std::fclose(file);
    removeHandoff(path); // Consume the credentials once, even for malformed files.
    if (!read) return fail(error, "The addon restart handoff is too large or unreadable.");
    bytes.resize(count);
    return true;
}

#if defined(_WIN32)
std::string quoteWindowsArgument(std::string const& value)
{
    std::string result = "\"";
    size_t slashes = 0;
    for (char c : value)
    {
        if (c == '\\') { ++slashes; continue; }
        result.append(c == '"' ? slashes * 2 + 1 : slashes, '\\');
        slashes = 0;
        result += c;
    }
    result.append(slashes * 2, '\\');
    return result + '"';
}
#endif
}

bool netAddonRestartInitialize(int& argc, char const* const*& argv, std::string& error)
{
    static bool cleanupRegistered = false;
    if (!cleanupRegistered)
    {
        std::atexit([] { removeHandoff(pendingFile); });
        cleanupRegistered = true;
    }
    executable = executablePath(argc > 0 ? argv[0] : nullptr);
    int internal = -1;
    for (int i = 1; i < argc; ++i)
        if (optionName(argv[i]) == "addon-restart") internal = i;
    if (internal < 0) return true;
    if (internal != 1 || (argc != 2 && argc != 3))
        return fail(error, "Addon restart handoffs cannot be combined with other command-line options.");
    std::string path = argv[1];
    size_t const equals = path.find('=');
    if (equals == std::string::npos)
    {
        if (argc != 3) return fail(error, "The addon restart handoff path is missing.");
        path = argv[2];
    }
    else
    {
        if (argc != 2) return fail(error, "Invalid addon restart handoff arguments.");
        path = path.substr(equals + 1);
    }
    std::vector<unsigned char> bytes;
    if (!readHandoff(path, bytes, error)) return false;
    Reader reader{bytes};
    uint32_t magic, version, created, parent, count;
    std::string addonDirectory;
    if (!reader.integer(magic) || magic != 0x4152424e || !reader.integer(version) || version != 1
        || !reader.integer(created) || !reader.integer(parent) || !reader.string(restoredProfile)
        || !profileIdentity(restoredProfile) || !reader.string(startupDirectory) || startupDirectory.empty()
        || !reader.string(addonDirectory)
        || !reader.integer(count) || !count || count > maxArguments)
        return fail(error, "The addon restart handoff is invalid.");
    std::time_t const now = std::time(nullptr);
    if (int64_t(now) < int64_t(created) || int64_t(now) - created > 300)
        return fail(error, "The addon restart handoff expired. Join the server again.");
    restoredArguments.clear();
    restoredArguments.push_back(executable);
    for (uint32_t i = 0; i < count; ++i)
    {
        std::string argument;
        if (!reader.string(argument) || argument.empty()) return fail(error, "The addon restart handoff contains invalid options.");
        restoredArguments.push_back(argument);
    }
    uint32_t profileBytes;
    if (!reader.integer(profileBytes) || profileBytes > netaddon::MaxProfileBytes || profileBytes != bytes.size() - reader.cursor)
        return fail(error, "The addon restart handoff has an invalid profile size.");
    netwire::Reader profileReader(bytes.data() + reader.cursor, profileBytes);
    netaddon::Profile profile;
    if (!netaddon::decode(profileReader, profile, error) || !profileReader.end()
        || netaddon::identity(profile) != restoredProfile || (addonDirectory.empty() && !profile.files.empty()))
        return fail(error, "The addon restart handoff profile is invalid.");
#if defined(_WIN32)
    // CreateProcess returns before the old instance releases its window/mutex.
    // Wait for that process before normal instance checking and SDL startup.
    if (parent && parent != GetCurrentProcessId())
    {
        HANDLE previous = OpenProcess(SYNCHRONIZE, FALSE, parent);
        if (previous) { DWORD const result = WaitForSingleObject(previous, 15000); CloseHandle(previous);
            if (result == WAIT_TIMEOUT) return fail(error, "The previous game instance did not finish shutting down."); }
    }
    if (_chdir(startupDirectory.c_str()) != 0)
#else
    (void) parent;
    if (chdir(startupDirectory.c_str()) != 0)
#endif
        return fail(error, "Cannot restore the game directory for addon activation.");
    if (!netAddonActivateProfile(profile, addonDirectory, error)) return false;
    // Definitions are selected explicitly by the server's typed profile. A
    // local environment override from an earlier addon must not reactivate it.
#if defined(_WIN32)
    _putenv("BLOODDEF=");
#else
    unsetenv("BLOODDEF");
#endif
    restoredPointers.clear();
    for (auto const& argument : restoredArguments) restoredPointers.push_back(argument.c_str());
    restoredPointers.push_back(nullptr);
    argc = int(restoredArguments.size());
    argv = restoredPointers.data();
    return true;
}

void netAddonRestartRememberStartup(int argc, char const* const* argv)
{
    startupDirectory = currentDirectory();
    startupArguments.clear();
    for (int i = 1; i < argc; ++i) startupArguments.push_back(argv[i]);
}

bool netAddonRestartRequest(std::vector<std::string> const& resourceArguments,
    std::string const& host, uint16_t port, std::string const& password,
    std::string const& profileId, netaddon::Profile const& profile,
    std::string const& directory, std::string& error, std::string const& zeroTierNetwork)
{
    if (!pendingFile.empty()) return fail(error, "An addon activation restart is already pending.");
    if (!profileIdentity(profileId)) return fail(error, "The addon profile identifier is invalid.");
    std::vector<uint8_t> encodedProfile;
    if (!netaddon::encode(profile, encodedProfile, error)) return false;
    if (netaddon::identity(profile) != profileId || (directory.empty() && !profile.files.empty()) || !cleanString(directory))
        return fail(error, "The addon activation directory or profile identity is invalid.");
    if (profileId == restoredProfile)
        return fail(error, "The addon is still incompatible after activation. Check the console for missing or different files; automatic restart was stopped.");
    if (executable.empty() || startupDirectory.empty() || !cleanString(executable) || !cleanString(startupDirectory))
        return fail(error, "Cannot locate the executable or working directory needed to activate this addon.");
    neturi::Endpoint endpoint;
    std::string const authority = host.find(':') == std::string::npos ? host : "[" + host + "]";
    if (!port || !neturi::parse("netblood://" + authority + ":" + std::to_string(port), endpoint, error)) return false;
    if (!cleanString(password, 128)) return fail(error, "The saved server password is invalid.");
    if (!zeroTierNetwork.empty() && (zeroTierNetwork.size() != 16 ||
        !std::all_of(zeroTierNetwork.begin(), zeroTierNetwork.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        }))) return fail(error, "The saved ZeroTier network ID is invalid.");
    std::vector<std::string> arguments;
    std::string const previousAddon = comparablePath(netAddonActiveDirectory());
    // Carry local preferences and base-data search paths. Old addon, map, demo,
    // and server options must not survive activation of a different profile.
    for (size_t i = 0; i < startupArguments.size(); ++i)
    {
        std::string const name = optionName(startupArguments[i]);
        if (listed(name, {"usecwd", "noconsole"}) && startupArguments[i].find('=') == std::string::npos)
            arguments.push_back("-" + name);
        else if (listed(name, {"map", "nodudes", "playback", "record", "setupfile", "skill", "ini", "control", "vector",
            "pname", "art", "snd", "rff", "maxalloc", "server", "client", "cachesize", "g", "grp", "game_dir", "cfg",
            "port", "h", "mh", "j", "c", "conf", "s", "gametype", "maxplayers", "timelimit", "scorelimit", "autoaim",
            "weaponsstay", "maplist", "episode", "difficulty", "losekeys", "mapvote", "friendlyfire", "holsterweapon",
            "chaseview", "lives", "stunserver", "password"}))
        {
            std::string value;
            if (!optionValue(startupArguments, i, value)) return fail(error, "Cannot preserve a local startup option for addon activation.");
            if (listed(name, {"cfg", "setupfile", "j", "pname", "cachesize", "maxalloc", "stunserver"})
                && !(name == "j" && !previousAddon.empty() && comparablePath(value) == previousAddon))
                arguments.push_back("-" + name + "=" + value);
        }
    }
    if (!resourceOptions(resourceArguments, arguments, error)) return false;
    arguments.push_back("-nosetup");
    arguments.push_back("-quick");
    arguments.push_back("-nodemo");
    arguments.push_back("-client=" + endpoint.host);
    arguments.push_back("-port=" + std::to_string(endpoint.port));
    arguments.push_back("-password=" + password);
    if (!zeroTierNetwork.empty()) arguments.push_back("-zerotier=" + zeroTierNetwork);
    if (arguments.size() > maxArguments) return fail(error, "Too many addon activation options.");
    for (auto const& argument : arguments)
        if (!cleanString(argument)) return fail(error, "An addon activation option exceeds the restart handoff limit.");
    std::vector<unsigned char> bytes;
    write32(bytes, 0x4152424e); // NBRA
    write32(bytes, 1);
    write32(bytes, uint32_t(std::time(nullptr)));
#if defined(_WIN32)
    write32(bytes, GetCurrentProcessId());
#else
    write32(bytes, uint32_t(getpid()));
#endif
    writeString(bytes, profileId);
    writeString(bytes, startupDirectory);
    writeString(bytes, directory);
    write32(bytes, uint32_t(arguments.size()));
    for (auto const& argument : arguments) writeString(bytes, argument);
    write32(bytes, uint32_t(encodedProfile.size()));
    bytes.insert(bytes.end(), encodedProfile.begin(), encodedProfile.end());
    if (bytes.size() > maxHandoffBytes) return fail(error, "The addon activation options exceed the restart handoff limit.");
    if (!writeHandoff(bytes, pendingFile, error)) { pendingFile.clear(); return false; }
    noticeStarted = false;
    return true;
}

bool netAddonRestartPending() { return !pendingFile.empty(); }
bool netAddonRestartReady()
{
    if (!netAddonRestartPending()) return false;
    if (!noticeStarted)
    {
        // A profile can arrive before startup has finished creating the window.
        // Start the notice interval when the main presentation loop first polls.
        readyAt = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        noticeStarted = true;
    }
    return std::chrono::steady_clock::now() >= readyAt;
}
bool netAddonRestartActive() { return !restoredProfile.empty(); }

bool netAddonRestartLaunch(std::string& error)
{
    if (pendingFile.empty()) return fail(error, "No addon activation restart was prepared.");
    std::string const argument = "-addon-restart=" + pendingFile;
#if defined(_WIN32)
    std::string command = quoteWindowsArgument(executable) + " " + quoteWindowsArgument(argument);
    std::vector<char> writable(command.begin(), command.end());
    writable.push_back(0);
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (CreateProcessA(executable.c_str(), writable.data(), nullptr, nullptr, FALSE, 0, nullptr,
        startupDirectory.c_str(), &startup, &process))
    {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        pendingFile.clear();
        return true;
    }
    error = "Cannot restart NetBlood to activate the addon (Windows error " + std::to_string(GetLastError()) + ").";
#else
    char* const arguments[] = {const_cast<char*>(executable.c_str()), const_cast<char*>(argument.c_str()), nullptr};
    // execv executes the current binary directly; no shell interprets paths,
    // addon names, addresses, passwords, or any part of the server profile.
    execv(executable.c_str(), arguments);
    error = "Cannot restart NetBlood to activate the addon: " + std::string(std::strerror(errno)) + ".";
#endif
    removeHandoff(pendingFile);
    pendingFile.clear();
    return false;
}
