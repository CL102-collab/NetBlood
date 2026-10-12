// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

// Dedicated rules live outside GAMEOPTIONS to preserve the legacy demo layout.
struct NetServerSettings
{
    int gameType = 1;
    int maxPlayers = 8;
    int autoAim = 1;
    bool weaponsStay = true;
    int timeLimitMinutes = 0;
    int scoreLimit = 0;
    int difficulty = 2;
    bool loseKeys = true;
    int episode = 0; // one-based; zero means no episode was selected explicitly
    size_t rotationIndex = 0;
    size_t rotationCount = 0;
    std::string mapList;
};

enum class NetServerLimit { NotReached, Time, Score };
// Gameplay ticks run at 30 Hz. Use wide arithmetic for long-running servers.
inline NetServerLimit netServerLimitReached(const NetServerSettings& settings, uint64_t elapsedTicks,
                                            const int* scores, size_t scoreCount)
{
    if (settings.timeLimitMinutes > 0 && elapsedTicks >= uint64_t(settings.timeLimitMinutes) * 60 * 30)
        return NetServerLimit::Time;
    if (settings.scoreLimit > 0 && settings.gameType >= 2)
        for (size_t i = 0; i < scoreCount; ++i)
            if (scores[i] >= settings.scoreLimit)
                return NetServerLimit::Score;
    return NetServerLimit::NotReached;
}

const NetServerSettings& netServerSettings();
void netServerParseOption(const char* name, const char* value);
void netServerValidateOptions(int gameType, bool hasUserMap);
void netServerInitializeSettings();
void netServerApplySettings();
void netServerPrepareUserMap(const char* name);
bool netServerPrepareEpisodeLevel(int episode, int level);
void netServerCheckLimits();
bool netServerHandleLevelExit(int exitType);
void netServerAdvanceMap();
