// SPDX-License-Identifier: GPL-2.0-or-later
// Run only in an isolated test user profile/Wine prefix: this registers itself
// as that user's netblood:// and test Discord handlers and exercises shell launch.
// Wine 10 does not merge HKCU\Software\Classes into HKCR. For Wine only,
// NETBLOOD_TEST_WINE_CLASSES=1 explicitly enables a test-only HKCR mirror after
// verifying the real per-user registration; native Windows needs no mirror.
#include "neturireg.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <cstdio>
#include <string>

int main()
{
    wchar_t executable[32768];
    DWORD const length = GetModuleFileNameW(nullptr, executable, 32768);
    if (!length || length >= 32768) return 1;
    std::wstring const path(executable, length);
    std::wstring const directory = path.substr(0, path.find_last_of(L"\\/"));
    std::wstring const resultPath = directory + L"\\uri-launched.txt";
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 2;
    std::string error;
    if (argc > 1)
    {
        if (argc != 3) return 3;
        bool const game = std::wstring(argv[1]) == L"-connect-uri"
            && std::wstring(argv[2]) == L"netblood://example.com:23513";
        bool const discord = std::wstring(argv[1]) == L"-discord-launch"
            && std::wstring(argv[2]) == L"1234567890123456789";
        if (!game && !discord) return 3;
        LocalFree(argv);
        if (!neturi::prepareLaunch(error)) return 4;
        wchar_t current[32768];
        DWORD const count = GetCurrentDirectoryW(32768, current);
        if (!count || count >= 32768 || std::wstring(current, count) != directory) return 5;
        HANDLE file = CreateFileW(resultPath.c_str(), GENERIC_WRITE, 0, nullptr,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return 6;
        DWORD written = 0;
        bool const ok = WriteFile(file, "ok", 2, &written, nullptr) && written == 2;
        CloseHandle(file);
        return ok ? 0 : 7;
    }
    LocalFree(argv);
    for (char const* invalid : {"", "0", "18446744073709551616", "123456789012345678901",
                                "12/../../bad", "12%1", "12\" -dedicated", "-12"})
    {
        if (neturi::registerDiscordHandler(invalid, error) || error.empty()) return 21;
    }
    std::wstring escapedPath;
    for (wchar_t c : path)
    {
        if (c == L'%') escapedPath += L'%';
        escapedPath += c;
    }
    for (bool discord : {false, true})
    {
        bool const registered = discord ? neturi::registerDiscordHandler("1234567890123456789", error)
                                        : neturi::registerHandler(error);
        if (!registered)
        {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 8;
        }
        wchar_t command[32768];
        std::wstring const scheme = discord ? L"discord-1234567890123456789" : L"netblood";
        std::wstring const key = L"Software\\Classes\\" + scheme;
        std::wstring const expectedArguments = discord ? L"-discord-launch 1234567890123456789"
                                                      : L"-connect-uri \"%1\"";
        DWORD bytes = sizeof(command);
        if (RegGetValueW(HKEY_CURRENT_USER, (key + L"\\shell\\open\\command").c_str(),
                         nullptr, RRF_RT_REG_SZ, nullptr, command, &bytes) != ERROR_SUCCESS
            || std::wstring(command) != L"\"" + escapedPath + L"\" " + expectedArguments) return 9;
        bytes = sizeof(command);
        if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), L"URL Protocol",
                         RRF_RT_REG_SZ, nullptr, command, &bytes) != ERROR_SUCCESS
            || std::wstring(command) != L"") return 10;
        wchar_t wineMirror[2] = {};
        if (GetEnvironmentVariableW(L"NETBLOOD_TEST_WINE_CLASSES", wineMirror, 2) == 1
            && wineMirror[0] == L'1')
        {
            if (!GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version")) return 17;
            HKEY source = nullptr, destination = nullptr;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0,
                              KEY_READ, &source) != ERROR_SUCCESS) return 18;
            if (RegCreateKeyExW(HKEY_CLASSES_ROOT, scheme.c_str(), 0, nullptr, 0,
                                KEY_ALL_ACCESS, nullptr, &destination, nullptr) != ERROR_SUCCESS)
            {
                RegCloseKey(source);
                return 19;
            }
            LONG const copied = RegCopyTreeW(source, nullptr, destination);
            RegCloseKey(source);
            RegCloseKey(destination);
            if (copied != ERROR_SUCCESS) return 20;
            std::puts("Wine workaround enabled: verified HKCU handler mirrored to HKCR for dispatch test");
        }
        DeleteFileW(resultPath.c_str());
        SHELLEXECUTEINFOW launch = {};
        launch.cbSize = sizeof(launch);
        launch.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
        launch.lpVerb = L"open";
        // No Discord URI content should reach the process, even potential options.
        launch.lpFile = discord ? L"discord-1234567890123456789://--connect-uri/$touch%20ignored?%22%20-dedicated"
                                : L"netblood://example.com:23513";
        launch.lpDirectory = L"C:\\windows";
        launch.nShow = SW_HIDE;
        if (!ShellExecuteExW(&launch))
        {
            std::fprintf(stderr, "ShellExecuteExW: %lu\n", GetLastError());
            return 11;
        }
        if (!launch.hProcess) return 12;
        DWORD const wait = WaitForSingleObject(launch.hProcess, 10000);
        if (wait != WAIT_OBJECT_0)
        {
            TerminateProcess(launch.hProcess, 13);
            CloseHandle(launch.hProcess);
            return 13;
        }
        DWORD exitCode = 0;
        GetExitCodeProcess(launch.hProcess, &exitCode);
        CloseHandle(launch.hProcess);
        if (exitCode)
        {
            std::fprintf(stderr, "URI child exited with %lu\n", exitCode);
            return 14;
        }
        HANDLE file = CreateFileW(resultPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return 15;
        char result[3] = {};
        DWORD read = 0;
        bool const ok = ReadFile(file, result, sizeof(result), &read, nullptr)
            && read == 2 && std::string(result) == "ok";
        CloseHandle(file);
        DeleteFileW(resultPath.c_str());
        if (!ok) return 16;
    }
    std::puts("Windows NetBlood and Discord URL registry, ShellExecute launch and game directory checks passed");
    return 0;
}
