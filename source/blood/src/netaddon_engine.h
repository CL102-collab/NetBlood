// SPDX-License-Identifier: GPL-2.0-or-later
// Adapter between the engine's startup filesystem and portable addon profiles.
#pragma once
#include "netaddon.h"

// Remember command-line archive order before G_LoadGroups consumes its list and
// before the engine changes its working directory.
void netAddonCaptureArguments(int argc, const char *const *argv);
// Restore the validated profile selected by the previous process. Its files
// are checked again after startup before the advertised identity is accepted.
bool netAddonActivateProfile(const netaddon::Profile &profile, const std::string &directory, std::string &error);
bool netAddonPreloadArchives(std::string &error);
bool netAddonFinalize(std::string &error);
const netaddon::Profile &netAddonProfile();
const std::string &netAddonActiveDirectory();
const std::string &netAddonSource(size_t index);
bool netAddonFindInstalled(const netaddon::Profile &profile, std::string &directory, std::string &error);
// Generates only resource options from typed, validated profile selections.
bool netAddonResourceArguments(const netaddon::Profile &profile, const std::string &directory,
                              std::vector<std::string> &arguments, std::string &error);
