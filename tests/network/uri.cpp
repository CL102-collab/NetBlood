// SPDX-License-Identifier: GPL-2.0-or-later
#include "neturi.h"
#include <cstdio>
#include <cstdlib>

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)

static void valid(const std::string& uri, const std::string& host, unsigned port = 23513)
{
    neturi::Endpoint endpoint;
    std::string error = "previous failure";
    if (!neturi::parse(uri, endpoint, error))
    {
        std::fprintf(stderr, "Unexpected invalid URI: %s (%s)\n", uri.c_str(), error.c_str());
        std::exit(1);
    }
    CHECK(endpoint.host == host);
    CHECK(endpoint.port == port);
    CHECK(error.empty());
}

static void invalid(const std::string& uri)
{
    neturi::Endpoint endpoint;
    endpoint.host = "unchanged.example";
    endpoint.port = 4321;
    std::string error;
    if (neturi::parse(uri, endpoint, error))
    {
        std::fprintf(stderr, "Unexpected valid URI: %s\n", uri.c_str());
        std::exit(1);
    }
    CHECK(endpoint.host == "unchanged.example");
    CHECK(endpoint.port == 4321);
    CHECK(!error.empty());
}

int main()
{
    valid("netblood://server-ip.tld:23513", "server-ip.tld");
    valid("NetBlood://Server-1.Example/", "Server-1.Example");
    valid("NETBLOOD://localhost", "localhost");
    valid("netblood://server.example.:1/", "server.example.", 1);
    valid("netblood://127.0.0.1:65535", "127.0.0.1", 65535);
    valid("netblood://0.0.0.0", "0.0.0.0");
    valid("netblood://255.255.255.255", "255.255.255.255");
    valid("netblood://[::1]", "::1");
    valid("netblood://[::]:1/", "::", 1);
    valid("netblood://[2001:DB8::1]:65535", "2001:DB8::1", 65535);
    // Preserve complete IPv6 literals, including their leading zeroes.
    valid("netblood://[201:f044:ca34:9cc9:2946:ad06:6f2d:5c4e]:23513",
        "201:f044:ca34:9cc9:2946:ad06:6f2d:5c4e", 23513);
    valid("netblood://[ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff]:23513",
        "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff", 23513);
    valid("netblood://[0000:0000:0000:0000:0000:0000:0000:0001]:23513",
        "0000:0000:0000:0000:0000:0000:0000:0001", 23513);
    valid("netblood://[1:2:3:4:5:6:7:8]", "1:2:3:4:5:6:7:8");
    valid("netblood://[1:2:3:4:5:6:7::]", "1:2:3:4:5:6:7::");
    valid("netblood://[::ffff:192.0.2.1]:23513", "::ffff:192.0.2.1");
    valid("netblood://[1:2:3:4:5:6:192.0.2.1]", "1:2:3:4:5:6:192.0.2.1");
    valid("netblood://a:00001", "a", 1);
    valid("netblood://a1-b2", "a1-b2");
    const std::string longest = std::string(63, 'a') + "." + std::string(63, 'b')
        + "." + std::string(63, 'c') + "." + std::string(61, 'd');
    valid("netblood://" + longest, longest);
    invalid("netblood://" + longest + "e");
    invalid("netblood://" + std::string(64, 'a'));

    const char* malformed[] = {
        "", "netblood", "netblood:", "netblood:/host", "netblood://", "netblood:///",
        "netblood:////", "http://host", "netbloodx://host", "netblood://:23513",
        "netblood://host:", "netblood://host:0", "netblood://host:65536",
        "netblood://host:-1", "netblood://host:+1", "netblood://host:1.0",
        "netblood://host:0x50", "netblood://host:12345678901234567890",
        "netblood://host:1:2", "netblood://host:23513/path", "netblood://host//",
        "netblood://host?", "netblood://host#", "netblood://host/?x=1",
        "netblood://user@host", "netblood://user:pass@host", "netblood://a..b",
        "netblood://.host", "netblood://host..", "netblood://.", "netblood://-host",
        "netblood://host-", "netblood://ho_st", "netblood://a.-b", "netblood://a-.b",
        "netblood://256.0.0.1", "netblood://1.2.3.999", "netblood://127.00.0.1",
        "netblood://[]", "netblood://[::1", "netblood://::1", "netblood://[::1]]",
        "netblood://[::1]x", "netblood://[::1]:", "netblood://[::1]:0",
        "netblood://[::1]:65536", "netblood://[127.0.0.1]", "netblood://[example.com]",
        "netblood://[1:2:3:4:5:6:7]", "netblood://[1:2:3:4:5:6:7:8:9]",
        "netblood://[1:2:3:4:5:6:7:8::]", "netblood://[1:2:3:4:5:6:7:8:]",
        "netblood://[:1:2:3:4:5:6:7:8]", "netblood://[1::2::3]", "netblood://[:::1]",
        "netblood://[1:::]", "netblood://[10000::1]", "netblood://[gg::1]",
        "netblood://[::ffff:192.0.2.256]", "netblood://[::ffff:192.0.2.1:1]",
        "netblood://[192.0.2.1::]", "netblood://[fe80::1%eth0]",
        "netblood://[fe80::1%25eth0]", "netblood://%65xample.com",
        "netblood://host%3a23513", "netblood://host%00", "netblood://host%20-client",
        "netblood://host -server", "netblood://host\t-server", "netblood://host\n-server",
        " netblood://host", "netblood://host ", "netblood://host\\path",
        "netblood://host\" -server", "netblood://host'", "netblood://$(touch)",
        "netblood://`id`", "netblood://host;id", "netblood://host&calc", "netblood://host|id",
        "netblood://host>file", "netblood://host<file", "netblood://ho\xc3\xb6st",
    };
    for (const char* uri : malformed) invalid(uri);
    invalid("netblood://" + std::string(1100, 'x'));

    // Test every non-printing, non-ASCII or forbidden character in each input field.
    for (unsigned ch = 0; ch < 256; ++ch)
        if (ch <= 32 || ch >= 127 || ch == '%' || ch == '\\' || ch == '\'' || ch == '"')
        {
            const std::string byte(1, static_cast<char>(ch));
            invalid("netblood://ho" + byte + "st");
            invalid("netblood://host:12" + byte + "34");
            invalid("netblood://[::" + byte + "1]");
        }
    // Every usable UDP port, including both boundaries, is parsed without truncation.
    for (unsigned port = 1; port <= 65535; ++port)
        valid("netblood://host:" + std::to_string(port), "host", port);
    std::printf("PASS: %u connection URI checks (DNS/IPv4/IPv6, port range, rejection and unchanged failure outputs)\n", checks);
}
