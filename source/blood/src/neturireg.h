// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef NETBLOOD_NETURIREG_H
#define NETBLOOD_NETURIREG_H

#include <string>

namespace neturi
{
// Associate netblood:// links with this executable for the current user.
bool registerHandler(std::string& error);

// Browsers do not provide the game directory as their working directory.
bool prepareLaunch(std::string& error);
}

#endif
