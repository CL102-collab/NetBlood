// SPDX-License-Identifier: GPL-2.0-or-later
#include "neturireg.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#elif defined(__linux__)
#include <cstdlib>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace neturi
{
namespace
{
#if defined(_WIN32)
bool windowsError(std::string& error, char const* operation, LONG code)
{
    error = std::string(operation) + " (Windows error " + std::to_string(code) + ").";
    return false;
}

bool executablePath(std::wstring& path, std::string& error)
{
    std::vector<wchar_t> buffer(256);
    for (;;)
    {
        DWORD const length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (!length)
            return windowsError(error, "Cannot locate the NetBlood executable", GetLastError());
        if (length < buffer.size())
        {
            path.assign(buffer.data(), length);
            // A quote cannot occur in a Windows file name; reject it defensively
            // before embedding the path in a registered command line.
            if (path.find_first_of(L"\"\r\n") != std::wstring::npos)
            {
                error = "The executable path cannot be represented safely in a URL handler.";
                return false;
            }
            return true;
        }
        if (buffer.size() >= 32768)
        {
            error = "The NetBlood executable path is too long.";
            return false;
        }
        buffer.resize(buffer.size() * 2);
    }
}

bool registryString(wchar_t const* subkey, wchar_t const* name,
                    std::wstring const& value, std::string& error)
{
    HKEY key;
    LONG result = RegCreateKeyExW(HKEY_CURRENT_USER, subkey, 0, nullptr, 0,
                                 KEY_SET_VALUE, nullptr, &key, nullptr);
    if (result != ERROR_SUCCESS)
        return windowsError(error, "Cannot create the per-user NetBlood URL handler", result);
    result = RegSetValueExW(key, name, 0, REG_SZ,
                           reinterpret_cast<BYTE const*>(value.c_str()),
                           static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    if (result != ERROR_SUCCESS)
        return windowsError(error, "Cannot write the per-user NetBlood URL handler", result);
    return true;
}
#elif defined(__linux__)
bool posixError(std::string& error, std::string const& operation, int code = errno)
{
    error = operation + ": " + std::strerror(code) + ".";
    return false;
}

bool safePath(std::string const& path, std::string& error)
{
    if (path.empty() || path[0] != '/')
    {
        error = "URL handler paths must be absolute.";
        return false;
    }
    for (unsigned char c : path)
    {
        if (c < 32 || c == 127)
        {
            error = "URL handler paths cannot contain control characters.";
            return false;
        }
    }
    return true;
}

bool executablePath(std::string& path, std::string& error)
{
    std::vector<char> buffer(256);
    for (;;)
    {
        ssize_t const length = readlink("/proc/self/exe", buffer.data(), buffer.size());
        if (length < 0)
            return posixError(error, "Cannot locate the NetBlood executable");
        if (static_cast<size_t>(length) < buffer.size())
        {
            path.assign(buffer.data(), static_cast<size_t>(length));
            if (!safePath(path, error))
                return false;
            struct stat info;
            if (stat(path.c_str(), &info) != 0)
                return posixError(error, "Cannot access the NetBlood executable " + path);
            return true;
        }
        if (buffer.size() >= 1048576)
        {
            error = "The NetBlood executable path is too long.";
            return false;
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::string directoryOf(std::string const& path)
{
    size_t const slash = path.find_last_of('/');
    return slash == 0 ? "/" : path.substr(0, slash);
}

bool createDirectories(std::string const& path, std::string& error)
{
    for (size_t end = 1; end <= path.size(); ++end)
    {
        if (end != path.size() && path[end] != '/')
            continue;
        std::string const part = path.substr(0, end);
        if (mkdir(part.c_str(), 0755) != 0 && errno != EEXIST)
            return posixError(error, "Cannot create URL handler directory " + part);
        struct stat info;
        if (stat(part.c_str(), &info) != 0)
            return posixError(error, "Cannot access URL handler directory " + part);
        if (!S_ISDIR(info.st_mode))
        {
            error = "The URL handler directory is not a directory: " + part + ".";
            return false;
        }
    }
    return true;
}

// Desktop string escaping is applied before parsing the Exec command line.
std::string desktopString(std::string const& value)
{
    std::string result;
    for (char c : value)
    {
        if (c == '\\')
            result += "\\\\";
        else if (c == ' ')
            result += "\\s";
        else
            result += c;
    }
    return result;
}

std::string desktopArgument(std::string const& value)
{
    std::string quoted = "\"";
    for (char c : value)
    {
        if (c == '"' || c == '`' || c == '$' || c == '\\')
            quoted += '\\';
        // Percent signs in an executable path must not become field codes.
        if (c == '%')
            quoted += '%';
        quoted += c;
    }
    quoted += '"';
    return desktopString(quoted);
}

bool writeDesktopFile(std::string const& path, std::string const& contents, std::string& error)
{
    std::string temporary = path + ".XXXXXX";
    std::vector<char> filename(temporary.begin(), temporary.end());
    filename.push_back('\0');
    int const fd = mkstemp(filename.data());
    if (fd < 0)
        return posixError(error, "Cannot create URL handler file " + path);
    int failure = 0;
    size_t offset = 0;
    while (offset < contents.size())
    {
        ssize_t const count = write(fd, contents.data() + offset, contents.size() - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
        {
            failure = count < 0 ? errno : EIO;
            break;
        }
        offset += static_cast<size_t>(count);
    }
    if (!failure && fchmod(fd, 0644) != 0)
        failure = errno;
    if (close(fd) != 0 && !failure)
        failure = errno;
    if (!failure && rename(filename.data(), path.c_str()) != 0)
        failure = errno;
    if (failure)
    {
        unlink(filename.data());
        return posixError(error, "Cannot write URL handler file " + path, failure);
    }
    return true;
}

bool setDefaultHandler(std::string& error)
{
    int descriptors[2];
    if (pipe(descriptors) != 0)
        return posixError(error, "Cannot start xdg-mime");
    if (fcntl(descriptors[1], F_SETFD, FD_CLOEXEC) != 0)
    {
        int const failure = errno;
        close(descriptors[0]);
        close(descriptors[1]);
        return posixError(error, "Cannot start xdg-mime", failure);
    }
    pid_t const child = fork();
    if (child < 0)
    {
        int const failure = errno;
        close(descriptors[0]);
        close(descriptors[1]);
        return posixError(error, "Cannot start xdg-mime", failure);
    }
    if (!child)
    {
        close(descriptors[0]);
        execlp("xdg-mime", "xdg-mime", "default", "netblood.desktop",
               "x-scheme-handler/netblood", static_cast<char*>(nullptr));
        int const failure = errno;
        // sizeof(int) is smaller than PIPE_BUF: a single write is atomic.
        while (write(descriptors[1], &failure, sizeof(failure)) < 0 && errno == EINTR) {}
        _exit(127);
    }
    close(descriptors[1]);
    int launchError = 0;
    ssize_t count;
    do { count = read(descriptors[0], &launchError, sizeof(launchError)); }
    while (count < 0 && errno == EINTR);
    close(descriptors[0]);
    int status;
    pid_t waited;
    do { waited = waitpid(child, &status, 0); }
    while (waited < 0 && errno == EINTR);
    if (waited < 0)
        return posixError(error, "Cannot wait for xdg-mime");
    if (count > 0 && launchError)
    {
        if (launchError == ENOENT)
        {
            error = "The desktop entry was created, but xdg-mime is missing. Install xdg-utils and register again.";
            return false;
        }
        return posixError(error, "The desktop entry was created, but xdg-mime could not start", launchError);
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    {
        error = "The desktop entry was created, but xdg-mime could not make NetBlood the default URL handler";
        if (WIFEXITED(status))
            error += " (exit status " + std::to_string(WEXITSTATUS(status)) + ")";
        error += ". Register again from your desktop user session.";
        return false;
    }
    return true;
}
#endif
} // namespace

bool registerHandler(std::string& error)
{
    error.clear();
#if defined(_WIN32)
    std::wstring path;
    if (!executablePath(path, error))
        return false;
    std::wstring commandPath;
    for (wchar_t c : path)
    {
        // ShellExecute expands %1 and other placeholders in the command.
        // Escape literal percent signs belonging to the executable path.
        if (c == L'%') commandPath += L'%';
        commandPath += c;
    }
    // Write the complete launch command before advertising the association.
    if (!registryString(L"Software\\Classes\\netblood\\shell\\open\\command", nullptr,
                        L"\"" + commandPath + L"\" -connect-uri \"%1\"", error)
        || !registryString(L"Software\\Classes\\netblood\\shell", nullptr, L"open", error)
        || !registryString(L"Software\\Classes\\netblood\\DefaultIcon", nullptr,
                           L"\"" + path + L"\",0", error)
        || !registryString(L"Software\\Classes\\netblood", nullptr, L"URL:NetBlood Protocol", error)
        || !registryString(L"Software\\Classes\\netblood", L"URL Protocol", L"", error))
        return false;
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return true;
#elif defined(__linux__)
    std::string path;
    if (!executablePath(path, error))
        return false;
    char const* const dataHome = std::getenv("XDG_DATA_HOME");
    std::string directory;
    if (dataHome && *dataHome)
        directory = dataHome;
    else
    {
        char const* const home = std::getenv("HOME");
        if (!home || !*home)
        {
            error = "Set HOME or an absolute XDG_DATA_HOME before registering the URL handler.";
            return false;
        }
        directory = std::string(home) + "/.local/share";
    }
    if (!safePath(directory, error))
        return false;
    directory += "/applications";
    if (!createDirectories(directory, error))
        return false;
    std::string command = desktopArgument(path);
    // GLib checks whether Exec's first token exists before expanding %% field
    // codes. A fixed env executable lets paths containing % work there too.
    if (path.find('%') != std::string::npos)
    {
        // env treats any argument containing '=' as an environment assignment,
        // even after '--'. Do not emit a handler that could misinterpret it.
        if (path.find('=') != std::string::npos)
        {
            error = "The executable path contains both '%' and '='. Move NetBlood to a different directory before registering.";
            return false;
        }
        if (access("/usr/bin/env", X_OK) != 0)
            return posixError(error, "Cannot access /usr/bin/env to launch the URL handler");
        command = "/usr/bin/env -- " + command;
    }
    std::string const contents = "[Desktop Entry]\nType=Application\nName=NetBlood\n"
        "Comment=Join a NetBlood multiplayer server\nExec=" + command
        + " -connect-uri %u\nPath=" + desktopString(directoryOf(path))
        + "\nTerminal=false\nNoDisplay=true\nMimeType=x-scheme-handler/netblood;\n";
    return writeDesktopFile(directory + "/netblood.desktop", contents, error)
        && setDefaultHandler(error);
#else
    error = "NetBlood URL handler registration is supported on Windows and Linux.";
    return false;
#endif
}

bool prepareLaunch(std::string& error)
{
    error.clear();
#if defined(_WIN32)
    std::wstring path;
    if (!executablePath(path, error))
        return false;
    size_t const slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos || !SetCurrentDirectoryW(path.substr(0, slash + 1).c_str()))
        return windowsError(error, "Cannot open the NetBlood game directory", GetLastError());
    return true;
#elif defined(__linux__)
    std::string path;
    if (!executablePath(path, error))
        return false;
    if (chdir(directoryOf(path).c_str()) != 0)
        return posixError(error, "Cannot open the NetBlood game directory");
    return true;
#else
    error = "NetBlood URL launching is supported on Windows and Linux.";
    return false;
#endif
}
} // namespace neturi
