// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string>

// Follow the console tokenizer's spaces, quotes, escapes, and statement
// separators. Only an exact command name is sensitive, never another command's
// quoted argument. The caller must dispatch the original command separately.
inline std::string OSD_RedactPasswords(const std::string& command)
{
    std::string result;
    size_t at = 0;
    while (at < command.size())
    {
        size_t const start = at;
        while (at < command.size() && command[at] == ' ') ++at;
        if (at == command.size()) { result.append(command, start, at-start); break; }
        if (command[at] == ';') { result.append(command, start, ++at-start); continue; }

        auto token = [&]() {
            std::string value;
            if (command[at] == '"')
            {
                ++at;
                while (at < command.size())
                {
                    char ch = command[at++];
                    if (ch == '"') break;
                    if (ch == '\\' && at < command.size())
                    {
                        ch = command[at++];
                        if (ch == 'n') ch = '\n';
                        else if (ch == 'r') ch = '\r';
                    }
                    value.push_back(ch);
                }
            }
            else
                while (at < command.size() && command[at] != ' ' && command[at] != ';') value.push_back(command[at++]);
            return value;
        };
        std::string name = token();
        size_t const nameEnd = at;
        for (char& ch : name) if (ch >= 'A' && ch <= 'Z') ch += 'a'-'A';
        if (name.compare(0, 2, "//") == 0) { result.append(command, start, std::string::npos); break; }
        while (at < command.size())
        {
            while (at < command.size() && command[at] == ' ') ++at;
            if (at == command.size() || command[at] == ';') break;
            token();
        }
        if (name == "password") { result.append(command, start, nameEnd-start); result += " [redacted]"; }
        else result.append(command, start, at-start);
        if (at < command.size()) result.push_back(command[at++]);
    }
    return result;
}
