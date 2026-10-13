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
bool hasDifficulty = false, hasLoseKeys = false, hasCoopLives = false;
int serverPlayers = 8;
struct MapEntry { int episode, level; LEVELINFO metadata; };
std::vector<MapEntry> initialMaps;
std::vector<std::string> rotation;
int nextEpisode = 0, nextLevel = 0;
bool pendingAdvance = false, preserveInventory = false;
NetServerLimit pendingLimit = NetServerLimit::NotReached;

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
            netQueueEpisodeOutro(gGameOptions.nEpisode, gGameOptions.nLevel);
            nextEpisode = episode;
            nextLevel = 0;
            preserveInventory = false;
        }
    }
    pendingAdvance = true;
    gGameOptions.uGameFlags |= kGameFlagContinuing;
    LOG_F(INFO, "NET dedicated map finished: reason=%s map=%s elapsed_ticks=%d", reason, gGameOptions.zLevelName, gLevelTime);
}

bool voteAllowed(std::string& error)
{
    if (!netIsServer())
        error = "Only a multiplayer server can run votes.";
    else if (!settings.mapVoteEnabled)
        error = "Voting is disabled on this server.";
    else if (!gGameStarted || !ready2send || pendingAdvance || gStartNewGame || netCinematicActive()
             || netIntermissionActive()
             || (gGameOptions.uGameFlags & (kGameFlagContinuing | kGameFlagEnding)))
        error = "A map change is already in progress.";
    else
        return true;
    return false;
}

struct VoteNextLevel
{
    int episode, level;
    bool preserveInventory;
};

bool nextVoteEpisode(VoteNextLevel& target, std::string& error)
{
    if (gGameOptions.szUserMap[0])
    {
        error = "This map has no next level; vote to restart it instead.";
        return false;
    }
    int episode = gGameOptions.nEpisode;
    int level = gGameOptions.nLevel;
    int const selectedEpisode = gDedicated && settings.episode ? settings.episode - 1 : episode;
    if (episode < 0 || episode >= gEpisodeCount || selectedEpisode < 0 || selectedEpisode >= gEpisodeCount
        || level < 0 || level >= gEpisodeInfo[episode].nLevels || gEpisodeInfo[selectedEpisode].nLevels < 1)
    {
        error = "The server has no valid next level.";
        return false;
    }
    int secret;
    levelGetNextLevels(episode, level, &level, &secret);
    bool preserve = true;
    if (level < 0 || level >= gEpisodeInfo[selectedEpisode].nLevels)
    {
        episode = selectedEpisode;
        level = 0;
        preserve = false;
    }
    if (level >= gEpisodeInfo[episode].nLevels)
    {
        error = "The server has no valid next level.";
        return false;
    }
    target = {episode, level, preserve};
    return true;
}

bool nextVoteMap(std::string& name, std::string& error)
{
    if (gDedicated && !rotation.empty())
    {
        name = rotation[(settings.rotationIndex + 1) % rotation.size()];
        return true;
    }
    VoteNextLevel target;
    if (!nextVoteEpisode(target, error)) return false;
    // Match netServerAdvanceMap's restored INI metadata after custom maps have
    // temporarily occupied a stock map's registry slot.
    if (gDedicated)
        for (auto const& entry : initialMaps)
            if (entry.episode == target.episode && entry.level == target.level)
            {
                name = entry.metadata.Filename;
                return true;
            }
    name = gEpisodeInfo[target.episode].levelsInfo[target.level].Filename;
    return true;
}

bool voteTarget(NetServerVoteAction action, size_t index, std::string& name, std::string& error)
{
    int const gameType = gDedicated ? settings.gameType : gGameOptions.nGameType;
    if (action == NetServerVoteAction::Shuffle)
    {
        if (gameType != kGameTypeTeams)
            error = "Team shuffle voting is only available in team games.";
        else if (netTeamShufflePlayerCount() < 2)
            error = "At least two players are required to shuffle teams.";
        else
        {
            name = gGameOptions.szUserMap[0] ? gGameOptions.szUserMap : gGameOptions.zLevelName;
            return true;
        }
        return false;
    }
    if (gameType != kGameTypeCoop && gameType != kGameTypeBloodBath && gameType != kGameTypeTeams)
    {
        error = "Map voting is unavailable for this game mode.";
        return false;
    }
    if (action == NetServerVoteAction::Restart)
    {
        name = gGameOptions.szUserMap[0] ? gGameOptions.szUserMap : gGameOptions.zLevelName;
        return true;
    }
    if (gameType == kGameTypeCoop)
    {
        if (action == NetServerVoteAction::Next)
            return nextVoteMap(name, error);
        error = "Co-op map votes can only restart the current level or advance to the next level.";
        return false;
    }
    if (!gDedicated || rotation.empty())
        error = "Map voting requires a server map rotation in Bloodbath or teams.";
    else if (action != NetServerVoteAction::Map)
        error = "Choose a map from the server's rotation.";
    else if (index >= rotation.size())
        error = "That map is not in the server's rotation.";
    else
    {
        name = rotation[index];
        return true;
    }
    return false;
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
    else if (!std::strcmp(name, "friendlyfire")) settings.friendlyFire = toggle(name, value);
    else if (!std::strcmp(name, "holsterweapon")) settings.holsterWeapon = toggle(name, value);
    else if (!std::strcmp(name, "chaseview")) settings.chaseView = toggle(name, value);
    else if (!std::strcmp(name, "mapvote")) settings.mapVoteEnabled = toggle(name, value);
    else if (!std::strcmp(name, "losekeys")) { settings.loseKeys = toggle(name, value); hasLoseKeys = true; }
    else if (!std::strcmp(name, "difficulty")) { settings.difficulty = number(name, value, 0, 4); hasDifficulty = true; }
    else if (!std::strcmp(name, "lives")) { settings.coopLives = number(name, value, 0, 255); hasCoopLives = true; }
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
    if (gameType != kGameTypeCoop && hasCoopLives)
        ThrowError("-lives requires -gametype coop");
    if (gameType == kGameTypeCoop && settings.scoreLimit)
        ThrowError("-scorelimit requires -gametype bloodbath or teams; use -timelimit for coop");
}

void netServerInitializeSettings()
{
    if (!gDedicated) return;
    netServerCancelIntermission();
    pendingAdvance = preserveInventory = false;
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
    gGameOptions.bFriendlyFire = settings.friendlyFire;
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
        || pendingLimit != NetServerLimit::NotReached || netIntermissionActive()
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
    {
        pendingLimit = limit;
        netBeginIntermission();
    }
}

void netServerUpdateIntermission()
{
    if (!gDedicated || !netIsServer() || pendingLimit == NetServerLimit::NotReached
        || gStartNewGame || !netIntermissionFinished()) return;
    queueAdvance(kLevelExitNormal, pendingLimit == NetServerLimit::Time ? "timelimit" : "scorelimit", true);
    netServerAdvanceMap();
    gStartNewGame = true;
}

void netServerCancelIntermission()
{
    pendingLimit = NetServerLimit::NotReached;
}

bool netServerHandleLevelExit(int exitType)
{
    if (!gDedicated || !netIsServer()) return false;
    // Consume exits during the scoreboard pause without advancing the map.
    if (netIntermissionActive()) return true;
    queueAdvance(exitType, exitType == kLevelExitSecret ? "secret-exit" : "normal-exit");
    return true;
}

void netServerAdvanceMap()
{
    if (netIntermissionActive() && !netIntermissionFinished()) return;
    netServerCancelIntermission();
    if (!pendingAdvance)
    {
        // Modern maps can supply gNextLevel directly, bypassing levelEndLevel.
        // Honor that target within the selected episode; a rotation still wins.
        nextEpisode = settings.episode ? settings.episode - 1 : gGameOptions.nEpisode;
        nextLevel = (gGameOptions.uGameFlags & kGameFlagEnding) ? 0 : gNextLevel;
        if (rotation.empty() && settings.gameType == kGameTypeCoop && !gGameOptions.szUserMap[0]
            && (gGameOptions.uGameFlags & kGameFlagEnding))
            netQueueEpisodeOutro(gGameOptions.nEpisode, gGameOptions.nLevel);
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
    LOG_F(INFO, "NET dedicated next map: map=%s rotation_index=%u rotation_count=%u", gGameOptions.zLevelName,
          unsigned(settings.rotationIndex), unsigned(settings.rotationCount));
}

void netServerRestartLevel()
{
    if (!gDedicated || !netIsServer()) return;
    netServerCancelIntermission();
    pendingAdvance = preserveInventory = false;
    gGameOptions.uGameFlags &= ~(kGameFlagContinuing | kGameFlagEnding | kGameFlagPlayOutro);
    netPrepareCurrentMapSettings();
    gStartNewGame = true;
}

std::vector<std::string> netServerVoteMaps()
{
    if (gDedicated && settings.mapVoteEnabled
        && (settings.gameType == kGameTypeBloodBath || settings.gameType == kGameTypeTeams))
        return rotation;
    return {};
}

bool netServerResolveVote(NetServerVoteAction action, const std::string& map,
                          size_t& rotationIndex, std::string& error)
{
    error.clear();
    if (!voteAllowed(error)) return false;
    size_t index = 0;
    if (action == NetServerVoteAction::Map)
    {
        if (map.find('\0') != std::string::npos)
        {
            error = "Invalid map name.";
            return false;
        }
        std::string const key = canonical(map.c_str());
        for (; index < rotation.size(); ++index)
            if (canonical(rotation[index].c_str()) == key) break;
    }
    else if (!map.empty())
    {
        error = "Restart, next-level and team-shuffle votes do not accept a map name.";
        return false;
    }
    std::string name;
    if (!voteTarget(action, index, name, error)) return false;
    rotationIndex = index;
    return true;
}

bool netServerApplyVote(NetServerVoteAction action, size_t rotationIndex, std::string& error)
{
    error.clear();
    if (!voteAllowed(error)) return false;
    std::string name;
    if (!voteTarget(action, rotationIndex, name, error)) return false;
    // Rotation entries were checked at startup, but their files may have been
    // removed while a vote was running. Reject the vote without killing the
    // dedicated server or changing any game settings.
    std::vector<uint8_t> bytes;
    std::string mapError;
    if (!netMapRead(name.c_str(), bytes, mapError))
    {
        error = "Cannot load voted map '" + name + "': " + mapError;
        return false;
    }
    // Validate the map before changing authoritative team assignments. A player
    // may also have disconnected since the vote began, so recheck the roster.
    if (action == NetServerVoteAction::Shuffle && !netShuffleTeams(error))
        return false;
    if (action == NetServerVoteAction::Next)
    {
        if (gDedicated)
        {
            queueAdvance(kLevelExitNormal, "mapvote-next");
            netServerAdvanceMap();
        }
        else
        {
            VoteNextLevel target;
            if (!nextVoteEpisode(target, error)) return false;
            if (!target.preserveInventory)
                netQueueEpisodeOutro(gGameOptions.nEpisode, gGameOptions.nLevel);
            levelSetupOptions(target.episode, target.level);
            gGameOptions.uGameFlags &= ~(kGameFlagContinuing | kGameFlagEnding | kGameFlagPlayOutro);
            if (target.preserveInventory) gGameOptions.uGameFlags |= kGameFlagContinuing;
            netPrepareCurrentMapSettings();
        }
    }
    else
    {
        if (action == NetServerVoteAction::Map)
        {
            settings.rotationIndex = rotationIndex;
            selectMap(rotation[rotationIndex].c_str());
        }
        // Selecting a rotation map, restarting the map or shuffling teams starts
        // a fresh round; co-op Next retains the usual episode inventory behavior.
        gGameOptions.uGameFlags &= ~(kGameFlagContinuing | kGameFlagEnding | kGameFlagPlayOutro);
        netPrepareCurrentMapSettings();
        // A listen server has already registered this map. Reuse its slot on
        // restart instead of appending the same custom map with levelAddUserMap.
        // Keep szUserMap intact so it still has no implicit next episode level.
        if ((action == NetServerVoteAction::Restart || action == NetServerVoteAction::Shuffle) && !gDedicated)
            gPacketStartGame.userMap = 0;
        LOG_F(INFO, "NET map vote applied: map=%s rotation_index=%u rotation_count=%u",
              gGameOptions.zLevelName, unsigned(settings.rotationIndex), unsigned(settings.rotationCount));
    }
    gStartNewGame = true;
    return true;
}
