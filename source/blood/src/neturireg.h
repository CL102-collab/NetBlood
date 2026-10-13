// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef NETBLOOD_NETURIREG_H
#define NETBLOOD_NETURIREG_H

#include <string>

namespace neturi
{
// Associate netblood:// links with this executable for the current user.
bool registerHandler(std::string& error);

// Register Discord's application launch scheme for the current user. Linux
// passes an ignored URI argument; Discord delivers its actual join secret by IPC.
bool registerDiscordHandler(std::string const& applicationId, std::string& error);

// Browsers do not provide the game directory as their working directory.
bool prepareLaunch(std::string& error);
}

#endif
