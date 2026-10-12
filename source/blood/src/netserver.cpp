// SPDX-License-Identifier: GPL-2.0-or-later
#include "netserver.h"
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "build.h"
#include "blood.h"
#include "common_game.h"
#include "db.h"
#include "globals.h"
#include "levels.h"
#include "mmulti.h"
#include "netmap.h"
#include "network.h"
#include "player.h"

namespace {
NetServerSettings settings;
bool hasDedicatedOptions = false, hasMaxPlayers = false, hasServerPlayers = false;
bool hasDifficulty = false, hasLoseKeys = false;
int serverPlayers = 8;
struct MapEntry { int episode, level; LEVELINFO metadata; };
std::vector<MapEntry> initialMaps;
std::vector<std::string> rotation;
int nextEpisode = 0, nextLevel = 0;
bool pendingAdvance = false, preserveInventory = false;

int number(const char* option, const char* text, int minimum, int maximum)
{
    if (!text || !*text)
        ThrowError("Missing argument for -%s", option);
    for (const char* p = text; *p; ++p)
        if (*p < '0' || *p > '9')
            ThrowError("Invalid -%s '%s': expected an integer from %d to %d", option, text, minimum, maximum);
    errno = 0;
    char* end = nullptr;
    long const result = std::strtol(text, &end, 10);
    if (errno || !end || *end || result < minimum || result > maximum)
        ThrowError("Invalid -%s '%s': expected an integer from %d to %d", option, text, minimum, maximum);
    return int(result);
}

bool toggle(const char* option, const char* text)
{
    if (text && !Bstrcasecmp(text, "on")) return true;
    if (text && !Bstrcasecmp(text, "off")) return false;
    ThrowError("Invalid -%s: expected on or off", option);
    return false;
}

std::string canonical(const char* name)
{
    std::string result(name);
    for (char& c : result)
    {
        if (c == '\\') c = '/';
        if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
    }
    if (result.size() >= 4 && result.compare(result.size() - 4, 4, ".MAP") == 0)
        result.resize(result.size() - 4);
    return result;
}

void checkMap(const char* name, const char* source)
{
    std::vector<uint8_t> bytes;
    std::string error;
    if (!netMapRead(name, bytes, error))
        ThrowError("Dedicated server %s: map '%s' is unavailable or invalid: %s", source, name, error.c_str());
}

void loadRotation()
{
    int const handle = kopen4loadfrommod(settings.mapList.c_str(), 0);
    if (handle < 0)
        ThrowError("Dedicated server -maplist: cannot open '%s'", settings.mapList.c_str());
    int const length = kfilelength(handle);
    if (length < 0 || length > 128 * 1024)
    {
        kclose(handle);
        ThrowError("Dedicated server -maplist '%s' exceeds 128 KiB", settings.mapList.c_str());
    }
    std::string text(size_t(length), '\0');
    int const bytesRead = length ? kread(handle, &text[0], length) : 0;
    kclose(handle);
    if (bytesRead != length || text.find('\0') != std::string::npos)
        ThrowError("Dedicated server -maplist '%s' is not a readable text file", settings.mapList.c_str());
    // Accept UTF-8 text files from Windows editors and either line ending.
    if (text.size() >= 3 && text.compare(0, 3, "\xef\xbb\xbf") == 0) text.erase(0, 3);
    size_t begin = 0;
    unsigned line = 0;
    while (begin < text.size())
    {
        ++line;
        size_t end = text.find('\n', begin);
        if (end == std::string::npos) end = text.size();
        std::string item = text.substr(begin, end - begin);
        begin = end + 1;
        size_t const comment = item.find_first_of("#;");
        if (comment != std::string::npos) item.resize(comment);
        size_t const first = item.find_first_not_of(" \t\r");
        if (first == std::string::npos) continue;
        item = item.substr(first, item.find_last_not_of(" \t\r") - first + 1);
        if (item.size() >= BMAX_PATH - 8 || item.find_first_of("\t\r\"<>") != std::string::npos)
            ThrowError("Dedicated server -maplist '%s' line %u: invalid map name", settings.mapList.c_str(), line);
        if (rotation.size() >= 256)
            ThrowError("Dedicated server -maplist '%s': maximum 256 maps", settings.mapList.c_str());
        checkMap(item.c_str(), settings.mapList.c_str());
        rotation.push_back(item);
    }
    if (rotation.empty())
        ThrowError("Dedicated server -maplist '%s' contains no maps", settings.mapList.c_str());
    settings.rotationCount = rotation.size();
}

void selectMap(const char* name)
{
    std::string const key = canonical(name);
    for (auto const& entry : initialMaps)
        if (canonical(entry.metadata.Filename) == key)
        {
            gEpisodeInfo[entry.episode].levelsInfo[entry.level] = entry.metadata;
            levelSetupOptions(entry.episode, entry.level);
            gGameOptions.szUserMap[0] = 0;
            return;
        }
    // Dedicated custom-map rotation reuses a metadata slot instead of growing
    // the finite legacy episode registry on every pass through the map list.
    // Original INI entries are retained above for later stock-map selection.
    char definition[BMAX_PATH];
    Bstrncpyz(definition, name, sizeof(definition));
    ChangeExtension(definition, ".DEF");
    IniFile metadata(definition);
    LEVELINFO map = {};
    Bstrncpyz(map.Filename, name, sizeof(map.Filename));
    levelLoadMapInfo(&metadata, &map, nullptr);
    gEpisodeInfo[0].levelsInfo[0] = map;
    gEpisodeInfo[0].nLevels = std::max(gEpisodeInfo[0].nLevels, 1);
    gEpisodeCount = std::max(gEpisodeCount, 1);
    levelSetupOptions(0, 0);
    Bstrncpyz(gGameOptions.szUserMap, name, sizeof(gGameOptions.szUserMap));
}

void queueAdvance(int exitType, const char* reason, bool limit = false)
{
    if (pendingAdvance || gStartNewGame) return;
    nextEpisode = gGameOptions.nEpisode;
    nextLevel = gGameOptions.nLevel;
    preserveInventory = rotation.empty() && settings.gameType == kGameTypeCoop && !gGameOptions.szUserMap[0]
        && (!limit || settings.episode != 0);
    if (preserveInventory)
    {
        int normal, secret;
        levelGetNextLevels(nextEpisode, nextLevel, &normal, &secret);
        nextLevel = exitType == kLevelExitSecret ? secret : normal;
        int const episode = settings.episode ? settings.episode - 1 : nextEpisode;
        if (nextLevel < 0 || nextLevel >= gEpisodeInfo[episode].nLevels)
        {
            nextEpisode = episode;
            nextLevel = 0;
            preserveInventory = false;
        }
    }
    pendingAdvance = true;
    gGameOptions.uGameFlags |= kGameFlagContinuing;
    LOG_F(INFO, "NET dedicated map finished: reason=%s map=%s elapsed_ticks=%d", reason, gGameOptions.zLevelName, gLevelTime);
}
}

const NetServerSettings& netServerSettings() { return settings; }

void netServerParseOption(const char* name, const char* value)
{
    if (!std::strcmp(name, "port")) { gNetPort = number(name, value, 1, 65535); return; }
    if (!std::strcmp(name, "server"))
    {
        serverPlayers = number(name, value, 1, kMaxPlayers);
        hasServerPlayers = true;
        gNetPlayers = serverPlayers;
        return;
    }
    if (!std::strcmp(name, "s"))
    {
        settings.difficulty = number(name, value, 0, 4);
        hasDifficulty = true;
        return;
    }
    hasDedicatedOptions = true;
    if (!std::strcmp(name, "maxplayers")) { settings.maxPlayers = number(name, value, 1, kMaxPlayers); hasMaxPlayers = true; }
    else if (!std::strcmp(name, "timelimit")) settings.timeLimitMinutes = number(name, value, 0, 10080);
    else if (!std::strcmp(name, "scorelimit")) settings.scoreLimit = number(name, value, 0, INT_MAX);
    else if (!std::strcmp(name, "autoaim")) settings.autoAim = toggle(name, value);
    else if (!std::strcmp(name, "weaponsstay")) settings.weaponsStay = toggle(name, value);
    else if (!std::strcmp(name, "losekeys")) { settings.loseKeys = toggle(name, value); hasLoseKeys = true; }
    else if (!std::strcmp(name, "difficulty")) { settings.difficulty = number(name, value, 0, 4); hasDifficulty = true; }
    else if (!std::strcmp(name, "episode")) settings.episode = number(name, value, 1, kMaxEpisodes);
    else if (!std::strcmp(name, "maplist"))
    {
        if (!value || !*value || std::strpbrk(value, "/\\:") || std::strlen(value) >= BMAX_PATH
            || std::strlen(value) < 5 || Bstrcasecmp(value + std::strlen(value) - 4, ".txt"))
            ThrowError("Invalid -maplist: specify a .txt filename only, without a directory");
        settings.mapList = value;
    }
}

void netServerValidateOptions(int gameType, bool hasUserMap)
{
    if (!gDedicated)
    {
        if (hasDedicatedOptions) ThrowError("Server rule options require -dedicated");
        return;
    }
    settings.gameType = gameType;
    if (hasMaxPlayers && hasServerPlayers && settings.maxPlayers != serverPlayers)
        ThrowError("Conflicting -maxplayers and -server player limits");
    if (!hasMaxPlayers && hasServerPlayers) settings.maxPlayers = serverPlayers;
    gNetPlayers = settings.maxPlayers;
    if ((settings.episode != 0) + !settings.mapList.empty() + hasUserMap > 1)
        ThrowError("Choose only one of -map, -maplist, or -episode");
    if (gameType != kGameTypeCoop && (settings.episode || hasDifficulty || hasLoseKeys))
        ThrowError("-episode, -difficulty/-s, and -losekeys require -gametype coop");
    if (gameType == kGameTypeCoop && settings.scoreLimit)
        ThrowError("-scorelimit requires -gametype bloodbath or teams; use -timelimit for coop");
}

void netServerInitializeSettings()
{
    if (!gDedicated) return;
    for (int e = 0; e < gEpisodeCount; ++e)
        for (int l = 0; l < gEpisodeInfo[e].nLevels; ++l)
            initialMaps.push_back({e, l, gEpisodeInfo[e].levelsInfo[l]});
    if (settings.episode)
    {
        int const episode = settings.episode - 1;
        if (episode >= gEpisodeCount || gEpisodeInfo[episode].nLevels < 1 || gEpisodeInfo[episode].bloodbath)
            ThrowError("Dedicated server -episode %d is unavailable for coop in '%s'", settings.episode, BloodIniFile);
        for (int l = 0; l < gEpisodeInfo[episode].nLevels; ++l)
        {
            checkMap(gEpisodeInfo[episode].levelsInfo[l].Filename, "-episode");
            int a, b;
            levelGetNextLevels(episode, l, &a, &b);
            if (a >= gEpisodeInfo[episode].nLevels || b >= gEpisodeInfo[episode].nLevels || a < -1 || b < -1)
                ThrowError("Dedicated server -episode %d map '%s' has an invalid level exit", settings.episode, gEpisodeInfo[episode].levelsInfo[l].Filename);
        }
        levelSetupOptions(episode, 0);
        gGameOptions.szUserMap[0] = 0;
    }
    else if (!settings.mapList.empty())
    {
        loadRotation();
        selectMap(rotation.front().c_str());
    }
    else if (gPacketStartGame.userMap)
    {
        checkMap(gPacketStartGame.userMapName, "-map");
        selectMap(gPacketStartGame.userMapName);
    }
    else
    {
        checkMap(gEpisodeInfo[gGameOptions.nEpisode].levelsInfo[gGameOptions.nLevel].Filename, "starting map");
        levelSetupOptions(gGameOptions.nEpisode, gGameOptions.nLevel);
    }
    netServerApplySettings();
    netPrepareCurrentMapSettings();
}

void netServerApplySettings()
{
    if (!gDedicated) return;
    gGameOptions.nGameType = settings.gameType;
    gGameOptions.nDifficulty = gGameOptions.nDifficultyQuantity = gGameOptions.nDifficultyHealth = settings.difficulty;
    gGameOptions.nWeaponSettings = settings.weaponsStay ? 1 : 2;
    gGameOptions.bPlayerKeys = settings.loseKeys ? LOSTONDEATH : KEPTONRESPAWN;
    if (settings.gameType != kGameTypeCoop) gGameOptions.nMonsterSettings = 0;
    gAutoAim = settings.autoAim;
    for (int p = 0; p < kMaxPlayers; ++p)
    {
        gProfile[p].nAutoAim = settings.autoAim;
        gProfile[p].skill = settings.gameType == kGameTypeCoop ? settings.difficulty : 2;
    }
}

void netServerPrepareUserMap(const char* name) { selectMap(name); }

bool netServerPrepareEpisodeLevel(int episode, int level)
{
    // Custom maps reuse a registry slot. Numeric administrator commands still
    // refer to the original INI, including a stock map that occupied that slot.
    for (auto const& entry : initialMaps)
        if (entry.episode == episode && entry.level == level)
        {
            gEpisodeInfo[episode].levelsInfo[level] = entry.metadata;
            levelSetupOptions(episode, level);
            gGameOptions.szUserMap[0] = 0;
            return true;
        }
    return false;
}

void netServerCheckLimits()
{
    if (!gDedicated || !netIsServer() || !gGameStarted || pendingAdvance || gStartNewGame
        || (gGameOptions.uGameFlags & kGameFlagContinuing)) return;
    int scores[kMaxPlayers] = {};
    size_t count = 0;
    if (settings.gameType == kGameTypeTeams)
    {
        scores[count++] = gPlayerScores[0];
        scores[count++] = gPlayerScores[1];
    }
    else
        for (int p = connecthead; p >= 0; p = connectpoint2[p]) scores[count++] = gPlayer[p].fragCount;
    NetServerLimit const limit = netServerLimitReached(settings, uint64_t(gLevelTime), scores, count);
    if (limit != NetServerLimit::NotReached)
        queueAdvance(kLevelExitNormal, limit == NetServerLimit::Time ? "timelimit" : "scorelimit", true);
}

bool netServerHandleLevelExit(int exitType)
{
    if (!gDedicated || !netIsServer()) return false;
    queueAdvance(exitType, exitType == kLevelExitSecret ? "secret-exit" : "normal-exit");
    return true;
}

void netServerAdvanceMap()
{
    if (!pendingAdvance)
    {
        // Modern maps can supply gNextLevel directly, bypassing levelEndLevel.
        // Honor that target within the selected episode; a rotation still wins.
        nextEpisode = settings.episode ? settings.episode - 1 : gGameOptions.nEpisode;
        nextLevel = (gGameOptions.uGameFlags & kGameFlagEnding) ? 0 : gNextLevel;
        preserveInventory = rotation.empty() && settings.gameType == kGameTypeCoop && !gGameOptions.szUserMap[0]
            && !(gGameOptions.uGameFlags & kGameFlagEnding);
        if (nextLevel < 0 || nextLevel >= gEpisodeInfo[nextEpisode].nLevels)
        {
            nextLevel = 0;
            preserveInventory = false;
        }
        LOG_F(INFO, "NET dedicated map finished: reason=custom-exit map=%s elapsed_ticks=%d", gGameOptions.zLevelName, gLevelTime);
    }
    pendingAdvance = false;
    if (!rotation.empty())
    {
        settings.rotationIndex = (settings.rotationIndex + 1) % rotation.size();
        selectMap(rotation[settings.rotationIndex].c_str());
    }
    else if (!gGameOptions.szUserMap[0])
    {
        for (auto const& entry : initialMaps)
            if (entry.episode == nextEpisode && entry.level == nextLevel)
                gEpisodeInfo[nextEpisode].levelsInfo[nextLevel] = entry.metadata;
        levelSetupOptions(nextEpisode, nextLevel);
    }
    // Only cooperative episode play carries inventory to the following level.
    if (!preserveInventory)
        gGameOptions.uGameFlags &= ~kGameFlagContinuing;
    gGameOptions.uGameFlags &= ~(kGameFlagEnding | kGameFlagPlayOutro);
    netServerApplySettings();
    netPrepareCurrentMapSettings();
    LOG_F(INFO, "NET dedicated next map: map=%s rotation_index=%zu rotation_count=%zu", gGameOptions.zLevelName,
          settings.rotationIndex, settings.rotationCount);
}
