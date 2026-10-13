// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef BLOOD_DISCORD_PRESENCE_H
#define BLOOD_DISCORD_PRESENCE_H

#include <string>

extern int gDiscordEnabled;
extern char gDiscordApplicationId[21];
extern char gDiscordServerAddress[300];

bool discordPresenceValidateSetting(const char* name, const char* value, std::string& error);
bool discordPresenceSetLaunchApplication(const char* applicationId, std::string& error);
void discordPresenceInitialize();
void discordPresenceShutdown();
// Polling may run during loading and cinematics. Joins only change game state
// after the outer game loop calls ApplyPendingJoin.
void discordPresencePump();
bool discordPresenceHasPendingJoin();
void discordPresenceApplyPendingJoin();
void discordPresenceRegister();
void discordPresencePrintStatus();

#endif
