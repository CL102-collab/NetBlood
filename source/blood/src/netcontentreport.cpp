// SPDX-License-Identifier: GPL-2.0-or-later
#include "netcontentreport.h"
#include "build.h"
#include "blood.h"
#include "keyboard.h"
#include "levels.h"
#include "menu.h"
#include "view.h"
#include <cerrno>
#include <cstdio>
#include <set>
#include <tuple>
#include <sys/stat.h>
#ifdef _WIN32
# include <windows.h>
#else
# include <fcntl.h>
# include <unistd.h>
#endif

namespace
{
constexpr char ReportName[] = "net-content-mismatch.txt";
constexpr int ReportFont = 3, TextWidth = 296, ContentTop = 43, ContentBottom = 139;
std::vector<netassets::Issue> reportIssues;
std::vector<std::vector<std::string>> pages;
std::string reportReason;
size_t currentPage = 0;
bool shown = false, reportSaved = false, previousUp = false, previousDown = false;
int lineHeight = 9;

const char *status(netassets::IssueKind kind)
{
    switch (kind)
    {
    case netassets::Missing: return "missing";
    case netassets::Mismatch: return "different";
    case netassets::NotLoaded: return "not loaded";
    }
    return "missing";
}

std::string decimal(uint64_t value)
{
    char text[32];
    std::snprintf(text, sizeof(text), "%llu", static_cast<unsigned long long>(value));
    return text;
}

bool saveReport(const std::string &contents)
{
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesA(ReportName);
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
        return false;
    static unsigned serial;
    char temporary[96];
    std::snprintf(temporary, sizeof(temporary), "%s.%lu.%u.part", ReportName,
                  static_cast<unsigned long>(GetCurrentProcessId()), ++serial);
    HANDLE file = CreateFileA(temporary, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    DWORD count = 0;
    bool ok = WriteFile(file, contents.data(), DWORD(contents.size()), &count, nullptr) &&
              count == contents.size() && FlushFileBuffers(file);
    if (!CloseHandle(file))
        ok = false;
    if (ok)
        ok = MoveFileExA(temporary, ReportName, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok)
        DeleteFileA(temporary);
    return ok;
#else
    const int directory = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0)
        return false;
    struct stat existing{};
    if (fstatat(directory, ReportName, &existing, AT_SYMLINK_NOFOLLOW) == 0)
    {
        if (!S_ISREG(existing.st_mode))
        {
            close(directory);
            return false;
        }
    }
    else if (errno != ENOENT)
    {
        close(directory);
        return false;
    }
    static unsigned serial;
    char temporary[96];
    std::snprintf(temporary, sizeof(temporary), "%s.%lu.%u.part", ReportName,
                  static_cast<unsigned long>(getpid()), ++serial);
    const int file = openat(directory, temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (file < 0)
    {
        close(directory);
        return false;
    }
    size_t offset = 0;
    while (offset < contents.size())
    {
        const ssize_t count = write(file, contents.data() + offset, contents.size() - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            break;
        offset += size_t(count);
    }
    bool ok = offset == contents.size() && fsync(file) == 0;
    if (close(file) < 0)
        ok = false;
    if (ok)
        ok = renameat(directory, temporary, directory, ReportName) == 0;
    if (!ok)
        unlinkat(directory, temporary, 0);
    close(directory);
    return ok;
#endif
}

std::vector<std::string> wrap(const std::string &text)
{
    std::vector<std::string> result;
    std::string line;
    for (char c : text)
    {
        std::string next = line + c;
        int width = 0;
        viewGetFontInfo(ReportFont, next.c_str(), &width, nullptr);
        if (width > TextWidth && !line.empty())
        {
            result.push_back(std::move(line));
            line.clear();
        }
        line += c;
    }
    result.push_back(std::move(line));
    return result;
}

void preparePages()
{
    int fontHeight = 0;
    viewGetFontInfo(ReportFont, nullptr, nullptr, &fontHeight);
    lineHeight = std::max(8, fontHeight + 1);
    const size_t linesPerPage = std::max(1, (ContentBottom - ContentTop) / lineHeight);
    pages.emplace_back();
    unsigned entries = 0;
    auto append = [&](const std::vector<std::string> &lines)
    {
        for (const auto &line : lines)
        {
            if (pages.back().size() == linesPerPage)
            {
                pages.emplace_back();
                entries = 0;
            }
            pages.back().push_back(line);
        }
    };
    if (reportIssues.empty())
    {
        append(wrap(reportReason));
        return;
    }
    for (size_t i = 0; i < reportIssues.size(); ++i)
    {
        const auto &issue = reportIssues[i];
        std::vector<std::string> lines = wrap(decimal(i + 1) + ". " + status(issue.kind) + ": " + issue.required.name);
        auto sizeLines = wrap("Size: " + decimal(issue.required.size) + " bytes");
        lines.insert(lines.end(), sizeLines.begin(), sizeLines.end());
        auto digestLines = wrap("MD5: " + netassets::hex(issue.required.md5));
        lines.insert(lines.end(), digestLines.begin(), digestLines.end());
        if (!pages.back().empty() && (entries >= 2 || pages.back().size() + lines.size() > linesPerPage))
        {
            pages.emplace_back();
            entries = 0;
        }
        append(lines);
        ++entries;
        if (pages.back().size() < linesPerPage)
            pages.back().emplace_back();
    }
}
}

void netContentReportShow(std::vector<netassets::Issue> issues, const std::string &reason)
{
    netContentReportClear();
    // Multiple resource IDs can refer to the same external file. The network
    // compares every semantic binding, but users only need each file once.
    std::set<std::tuple<netassets::IssueKind, std::string, uint64_t, netassets::Digest>> reported;
    for (auto &issue : issues)
        if (reported.emplace(issue.kind, issue.required.name, issue.required.size, issue.required.md5).second)
            reportIssues.push_back(std::move(issue));
    reportReason = reason.substr(0, 2048);
    for (char &c : reportReason)
        if (uint8_t(c) < 32 || uint8_t(c) == 127)
            c = ' ';
    shown = true;
    std::string contents = "Cannot join: " + reportReason + "\n\n";
    LOG_F(ERROR, "NET content mismatch: %s", reportReason.c_str());
    for (const auto &issue : reportIssues)
    {
        const std::string row = std::string("NET asset ") + status(issue.kind) + ": " + issue.required.name +
            " | size=" + decimal(issue.required.size) + " bytes | md5=" + netassets::hex(issue.required.md5);
        contents += row + "\n";
        LOG_F(ERROR, "%s", row.c_str());
    }
    const char *advice = "Install and select the same mod and game files as the server, then restart NBlood.";
    contents += std::string("\n") + advice + "\nThe listed sizes and MD5 sums describe the files/resources required by the server.\n";
    LOG_F(ERROR, "%s", advice);
    reportSaved = saveReport(contents);
    if (reportSaved)
        LOG_F(INFO, "NET content mismatch report saved to %s", ReportName);
    else
        LOG_F(ERROR, "NET could not save %s; the complete report is in this log", ReportName);
}

void netContentReportClear()
{
    reportIssues.clear();
    pages.clear();
    reportReason.clear();
    currentPage = 0;
    shown = reportSaved = previousUp = previousDown = false;
}

bool netContentReportDraw()
{
    if (!shown || gDedicated)
        return false;
    if (gGameStarted)
    {
        netContentReportClear();
        return false;
    }
    if (keystatus[sc_Escape])
    {
        netContentReportClear();
        return false;
    }
    if (gGameMenuMgr.m_bActive)
        return false;
    if (pages.empty())
        preparePages();
    const bool up = keystatus[sc_PgUp] != 0, down = keystatus[sc_PgDn] != 0;
    if (up && !previousUp && currentPage)
        --currentPage;
    if (down && !previousDown && currentPage + 1 < pages.size())
        ++currentPage;
    previousUp = up;
    previousDown = down;
    videoClearScreen(0);
    viewDrawText(ReportFont, "Cannot join: game files do not match", 160, 13, -128, 0, 1, 1);
    const std::string summary = reportIssues.empty() ? "Content compatibility check failed" :
        decimal(reportIssues.size()) + " required files/resources need attention";
    viewDrawText(ReportFont, summary.c_str(), 160, 26, -128, 0, 1, 1);
    int y = ContentTop;
    for (const auto &line : pages[currentPage])
    {
        viewDrawText(ReportFont, line.c_str(), 12, y, -128, 0, 0, 0);
        y += lineHeight;
    }
    viewDrawText(ReportFont, "Install/select the server's mod; restart NBlood.", 160, 146, -128, 0, 1, 1);
    const std::string saved = reportSaved ? "Saved: " + std::string(ReportName) : "Full details are in the console and log.";
    viewDrawText(ReportFont, saved.c_str(), 160, 157, -128, 0, 1, 1);
    const std::string pager = "Page " + decimal(currentPage + 1) + "/" + decimal(pages.size()) +
        "  PgUp/PgDn: browse  Esc: dismiss";
    viewDrawText(ReportFont, pager.c_str(), 160, 179, -128, 0, 1, 1);
    return true;
}
