// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef BLOOD_NETURI_H
#define BLOOD_NETURI_H

#include <cstdint>
#include <string>

// URI parsing is deliberately independent of the shell, DNS and platform APIs.
namespace neturi
{
struct Endpoint
{
    std::string host;
    uint16_t port = 23513;
};

namespace detail
{
inline bool digit(char ch) { return ch >= '0' && ch <= '9'; }
inline bool alpha(char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'); }
inline bool hex(char ch) { return digit(ch) || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'); }

inline bool ipv4(const std::string& host)
{
    size_t start = 0;
    unsigned parts = 0;
    while (start < host.size())
    {
        size_t end = host.find('.', start);
        if (end == std::string::npos) end = host.size();
        const size_t length = end - start;
        if (!length || length > 3 || (length > 1 && host[start] == '0')) return false;
        unsigned value = 0;
        for (size_t i = start; i < end; ++i)
        {
            if (!digit(host[i])) return false;
            value = value * 10 + unsigned(host[i] - '0');
        }
        if (value > 255 || ++parts > 4) return false;
        if (end == host.size()) return parts == 4;
        start = end + 1;
    }
    return false;
}

inline bool ipv6Parts(const std::string& part, bool allowIpv4, unsigned& groups)
{
    if (part.empty()) return true;
    size_t start = 0;
    while (start < part.size())
    {
        size_t end = part.find(':', start);
        if (end == std::string::npos) end = part.size();
        const std::string group = part.substr(start, end - start);
        if (group.empty()) return false;
        if (group.find('.') != std::string::npos)
        {
            if (!allowIpv4 || end != part.size() || !ipv4(group)) return false;
            groups += 2;
        }
        else
        {
            if (group.size() > 4) return false;
            for (char ch : group) if (!hex(ch)) return false;
            ++groups;
        }
        if (groups > 8) return false;
        if (end == part.size()) return true;
        start = end + 1;
    }
    return false; // A single trailing colon is not compression.
}

inline bool ipv6(const std::string& host)
{
    const size_t compression = host.find("::");
    unsigned groups = 0;
    if (compression == std::string::npos)
        return ipv6Parts(host, true, groups) && groups == 8;
    if (host.find("::", compression + 2) != std::string::npos) return false;
    // A dotted IPv4 tail must be the final 32 bits, after any compression.
    return ipv6Parts(host.substr(0, compression), false, groups)
        && ipv6Parts(host.substr(compression + 2), true, groups) && groups < 8;
}

inline bool hostname(const std::string& host)
{
    if (host.empty() || host.size() > 253) return false;
    bool numeric = true;
    unsigned dots = 0;
    for (char ch : host)
    {
        if (ch == '.') ++dots;
        else if (!digit(ch)) numeric = false;
    }
    // Do not let an invalid dotted IPv4 address become a DNS name.
    if (numeric && dots == 3) return ipv4(host);
    const size_t length = host.size() - (host.back() == '.' ? 1 : 0);
    if (!length) return false;
    size_t start = 0;
    while (start < length)
    {
        size_t end = host.find('.', start);
        if (end == std::string::npos || end > length) end = length;
        if (end == start || end - start > 63 || host[start] == '-' || host[end - 1] == '-') return false;
        for (size_t i = start; i < end; ++i)
            if (!alpha(host[i]) && !digit(host[i]) && host[i] != '-') return false;
        if (end == length) return true;
        start = end + 1;
    }
    return false;
}
} // namespace detail

// Failure preserves endpoint and returns a displayable explanation in error.
inline bool parse(const std::string& uri, Endpoint& endpoint, std::string& error)
{
    const char scheme[] = "netblood://";
    const size_t schemeLength = sizeof(scheme) - 1;
    error = "Expected a netblood://host[:port] link.";
    if (uri.size() < schemeLength || uri.size() > 1024) return false;
    for (size_t i = 0; i < schemeLength; ++i)
    {
        char ch = uri[i];
        if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
        if (ch != scheme[i]) return false;
    }
    for (unsigned char ch : uri)
        if (ch <= 32 || ch >= 127 || ch == '%' || ch == '\\' || ch == '\'' || ch == '"')
        {
            error = "Connection links cannot contain whitespace, encoding, quotes or non-ASCII characters.";
            return false;
        }
    std::string authority = uri.substr(schemeLength);
    if (!authority.empty() && authority.back() == '/') authority.pop_back();
    if (authority.find_first_of("/@?#") != std::string::npos)
    {
        error = "Connection links cannot contain credentials, paths, queries or fragments.";
        return false;
    }

    Endpoint parsed;
    std::string port;
    bool hasPort = false;
    if (!authority.empty() && authority.front() == '[')
    {
        const size_t closing = authority.find(']');
        if (closing == std::string::npos)
        {
            error = "An IPv6 address must be enclosed in square brackets.";
            return false;
        }
        parsed.host = authority.substr(1, closing - 1);
        if (!detail::ipv6(parsed.host))
        {
            error = "The connection link contains an invalid IPv6 address.";
            return false;
        }
        if (closing + 1 < authority.size())
        {
            if (authority[closing + 1] != ':')
            {
                error = "Only a port may follow a bracketed IPv6 address.";
                return false;
            }
            hasPort = true;
            port = authority.substr(closing + 2);
        }
    }
    else
    {
        const size_t colon = authority.find(':');
        parsed.host = authority.substr(0, colon);
        if (!detail::hostname(parsed.host))
        {
            error = "The connection link contains an invalid or empty server hostname.";
            return false;
        }
        if (colon != std::string::npos)
        {
            hasPort = true;
            port = authority.substr(colon + 1);
        }
    }
    if (hasPort)
    {
        error = "The connection port must be a decimal number from 1 to 65535; IPv6 addresses require brackets.";
        if (port.empty() || port.size() > 5) return false;
        unsigned value = 0;
        for (char ch : port)
        {
            if (!detail::digit(ch)) return false;
            value = value * 10 + unsigned(ch - '0');
        }
        if (!value || value > 65535) return false;
        parsed.port = static_cast<uint16_t>(value);
    }
    endpoint = parsed;
    error.clear();
    return true;
}
} // namespace neturi
#endif
