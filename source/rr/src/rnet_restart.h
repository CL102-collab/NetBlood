// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef REDNUKEM_RNET_RESTART_H
#define REDNUKEM_RNET_RESTART_H

#include <cstdint>
#include <string>
#include <vector>

namespace rnetaddon { struct Profile; }

// The private restart handoff is consumed before ordinary option processing.
// It never accepts command-line options supplied by a server: resourceArguments
// must be generated from the validated, typed addon profile.
bool rnetAddonRestartInitialize(int& argc, char const* const*& argv, std::string& error);
void rnetAddonRestartRememberStartup(int argc, char const* const* argv);
bool rnetAddonRestartRequest(std::vector<std::string> const& resourceArguments,
    std::string const& host, uint16_t port, std::string const& password,
    std::string const& profileId, rnetaddon::Profile const& profile,
    std::string const& directory, std::string& error);
bool rnetAddonRestartPending();
bool rnetAddonRestartReady();
// Resource archives activated by a server must never supply console scripts.
bool rnetAddonRestartActive();
// Call after normal shutdown has saved settings and disconnected the client.
bool rnetAddonRestartLaunch(std::string& error);

#endif
