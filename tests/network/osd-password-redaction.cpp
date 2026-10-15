// SPDX-License-Identifier: GPL-2.0-or-later
#include "../../source/build/include/osd_redact.h"
#include <cstdio>
#include <cstdlib>

int main()
{
    const char* cases[][2] = {
        {"password swordfish", "password [redacted]"},
        {"  PaSsWoRd \"with spaces\";net_status", "  PaSsWoRd [redacted];net_status"},
        {"password \"a;b\\\"c\"; echo okay; password second", "password [redacted]; echo okay; password [redacted]"},
        {"echo \"password harmless; word\"; password hidden", "echo \"password harmless; word\"; password [redacted]"},
        {"\"password\" hidden", "\"password\" [redacted]"},
        {"password hidden;password \"\";password", "password [redacted];password [redacted];password [redacted]"},
        {";;password hidden;", ";;password [redacted];"},
        {"password_file sample", "password_file sample"},
        {"echo password", "echo password"},
        {"echo okay; password swordfish", "echo okay; password [redacted]"},
        {"password \"unterminated secret", "password [redacted]"},
        {"// password example; password comment", "// password example; password comment"},
        {"password \"\\ncontrol\";echo done", "password [redacted];echo done"},
        {"", ""}, {"   ", "   "},
    };
    for (auto& test : cases)
        if (OSD_RedactPasswords(test[0]) != test[1])
        {
            std::fprintf(stderr, "Password redaction test failed\n");
            return EXIT_FAILURE;
        }
    std::puts("PASS: console password redaction (15 token/quote/chaining cases)");
}
