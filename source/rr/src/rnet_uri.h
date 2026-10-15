// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "../../blood/src/neturi.h"

// Keep the strictly validated endpoint grammar shared with NetBlood while
// using a separate scheme. Links never become console or shell commands.
namespace rnet { namespace uri {
using Endpoint = neturi::Endpoint;
inline bool parse(const std::string& value, Endpoint& endpoint, std::string& error)
{
    const char scheme[] = "rednukem://";
    error = "Expected a rednukem://host[:port] link.";
    if (value.size() < sizeof(scheme) - 1 || value.size() > 1024) return false;
    for (size_t i = 0; i < sizeof(scheme) - 1; ++i)
    {
        char ch = value[i];
        if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
        if (ch != scheme[i]) return false;
    }
    return neturi::parse("netblood://" + value.substr(sizeof(scheme) - 1), endpoint, error);
}
inline std::string address(const Endpoint& endpoint)
{
    return (endpoint.host.find(':') == std::string::npos ? endpoint.host : "[" + endpoint.host + "]")
        + ":" + std::to_string(endpoint.port);
}
inline std::string format(const Endpoint& endpoint) { return "rednukem://" + address(endpoint); }
bool registerHandler(std::string& error);
bool registerDiscordHandler(const std::string& applicationId, std::string& error);
bool prepareLaunch(std::string& error);
} }
