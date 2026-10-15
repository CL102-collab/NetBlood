// SPDX-License-Identifier: GPL-2.0-or-later
#include "rnet_uri.h"
#include <cstdio>
#include <cstdlib>

#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #value); std::exit(1); } } while (0)
int main()
{
    rnet::uri::Endpoint endpoint;
    std::string error;
    for (const char* link : { "rednukem://localhost", "REDNUKEM://localhost", "Rednukem://localhost/" })
    {
        CHECK(rnet::uri::parse(link, endpoint, error));
        CHECK(endpoint.host == "localhost" && endpoint.port == 23513 && error.empty());
        CHECK(rnet::uri::format(endpoint) == "rednukem://localhost:23513");
    }
    CHECK(rnet::uri::parse("rednukem://[2001:db8::1]:65535/", endpoint, error));
    CHECK(rnet::uri::address(endpoint) == "[2001:db8::1]:65535");
    CHECK(rnet::uri::format(endpoint) == "rednukem://[2001:db8::1]:65535");
    for (const char* bad : {
        "netblood://localhost", "http://localhost", "rednukemx://host", "rednukem://", "rednukem://[1::2::3]",
        "rednukem://host:0", "rednukem://host:65536", "rednukem://host -server", "rednukem://host\" -x evil.con",
        "rednukem://host%22", "rednukem://host;id", "rednukem://user@host", "rednukem://host/map",
        "rednukem://host?q=1", "rednukem://host#x", "rednukem://host$(id)", "rednukem://host`id`" })
    {
        CHECK(!rnet::uri::parse(bad, endpoint, error));
        CHECK(endpoint.host == "2001:db8::1" && endpoint.port == 65535 && !error.empty());
    }
    for (unsigned port = 1; port <= 65535; ++port)
    {
        CHECK(rnet::uri::parse("rednukem://127.0.0.1:" + std::to_string(port), endpoint, error));
        CHECK(endpoint.port == port && error.empty());
    }
    for (unsigned byte = 0; byte < 256; ++byte)
        if (byte <= 32 || byte >= 127 || byte == '%' || byte == '\\' || byte == '\'' || byte == '"')
            CHECK(!rnet::uri::parse("rednukem://ho" + std::string(1, char(byte)) + "st", endpoint, error));
    std::puts("PASS: Rednukem URI branding, round trips, every UDP port, and malformed/injected link rejection");
}
