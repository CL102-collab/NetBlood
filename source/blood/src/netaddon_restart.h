// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef NETBLOOD_NETADDON_RESTART_H
#define NETBLOOD_NETADDON_RESTART_H

#include <cstdint>
#include <string>
#include <vector>

namespace netaddon { struct Profile; }

// The private restart handoff is consumed before ordinary option processing.
// It never accepts command-line options supplied by a server: resourceArguments
// must be generated from the validated, typed addon profile.
bool netAddonRestartInitialize(int& argc, char const* const*& argv, std::string& error);
void netAddonRestartRememberStartup(int argc, char const* const* argv);
bool netAddonRestartRequest(std::vector<std::string> const& resourceArguments,
    std::string const& host, uint16_t port, std::string const& password,
    std::string const& profileId, netaddon::Profile const& profile,
    std::string const& directory, std::string& error,
    std::string const& zeroTierNetwork = {});
bool netAddonRestartPending();
bool netAddonRestartReady();
// Resource archives activated by a server must never supply console scripts.
bool netAddonRestartActive();
// Call after normal shutdown has saved settings and disconnected the client.
bool netAddonRestartLaunch(std::string& error);

#endif
