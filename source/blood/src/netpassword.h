// SPDX-License-Identifier: GPL-2.0-or-later
// Optional plaintext credentials appended to the client Hello packet.
#pragma once

#include "netprotocol.h"
#include <utility>

namespace netpassword
{
constexpr size_t MaxBytes = 128;
constexpr const char* MissingReason = "Server password required";
constexpr const char* IncorrectReason = "Incorrect server password";
constexpr const char* InvalidReason = "Invalid server password";

inline bool validate(const std::string& password, std::string& error)
{
    if (password.size() > MaxBytes)
    {
        error = "Password must be at most 128 bytes.";
        return false;
    }
    for (unsigned char character : password)
        if (character < 32 || character == 127)
        {
            error = "Password cannot contain control characters.";
            return false;
        }
    error.clear();
    return true;
}

inline bool writeExtension(netwire::Writer& writer, const std::string& password)
{
    std::string error;
    if (!validate(password, error)) return false;
    // Preserve the original Hello layout when no credential is configured.
    if (!password.empty()) writer.text(password, MaxBytes);
    return true;
}

inline bool readExtension(netwire::Reader& reader, std::string& password)
{
    std::string candidate = reader.pos < reader.size ? reader.text(MaxBytes) : std::string();
    std::string error;
    if (!reader.end() || !validate(candidate, error)) return false;
    password = std::move(candidate);
    return true;
}

enum class CheckResult { Accepted, Missing, Incorrect };

inline CheckResult check(const std::string& required, const std::string& supplied)
{
    if (required.empty() || required == supplied) return CheckResult::Accepted;
    return supplied.empty() ? CheckResult::Missing : CheckResult::Incorrect;
}

inline bool isRejection(const std::string& reason)
{
    return reason == MissingReason || reason == IncorrectReason || reason == InvalidReason;
}
}
