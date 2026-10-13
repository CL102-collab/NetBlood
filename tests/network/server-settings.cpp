// SPDX-License-Identifier: GPL-2.0-or-later
// Run the real dedicated limit checker against controlled player/team scores.
// Section GC removes unrelated startup, filesystem and map-loading functions.
#include "../../source/blood/src/netserver.cpp"

#include <cstdio>
#include <cstdlib>
#include <limits>

bool gDedicated = true, gGameStarted = true, gStartNewGame = false;
bool ready2send = true;
int gLevelTime = 0;
int gNextLevel = 0;
int32_t gAutoAim = 1;
int connecthead = -1, connectpoint2[MAXMULTIPLAYERS];
GAMEOPTIONS gGameOptions = {};
PKT_STARTGAME gPacketStartGame = {};
EPISODEINFO gEpisodeInfo[kMaxEpisodes] = {};
int gEpisodeCount = kMaxEpisodes;
static bool isServer = true;
static bool cinematicActive = false;
static bool intermissionActive = false;
static uint32_t nowMilliseconds = 0, intermissionStarted = 0;
static unsigned intermissions = 0;
static int queuedOutros = 0, outroEpisode = -1, outroLevel = -1;
static int preparedMaps = 0;
static std::string readMapName;
bool netIsServer() { return isServer; }
bool netCinematicActive() { return cinematicActive; }
bool netIntermissionActive() { return intermissionActive; }
bool netIntermissionFinished() { return intermissionActive && nowMilliseconds - intermissionStarted >= 15000; }
void netBeginIntermission()
{
    if (intermissionActive) return;
    intermissionActive = true;
    intermissionStarted = nowMilliseconds;
    ++intermissions;
}
void netQueueEpisodeOutro(int episode, int level)
{
    ++queuedOutros;
    outroEpisode = episode;
    outroLevel = level;
}
void levelSetupOptions(int episode, int level)
{
    gGameOptions.nEpisode = episode;
    gGameOptions.nLevel = level;
    std::strcpy(gGameOptions.zLevelName, gEpisodeInfo[episode].levelsInfo[level].Filename);
}
// Vote application retains the custom-map loading branch at link time. These
// policy scenarios use stock metadata; unexpected filesystem use is a failure.
void ChangeExtension(char*, const char*) { std::abort(); }
IniFile::IniFile(const char*, char) { std::abort(); }
IniFile::~IniFile() {}
void levelLoadMapInfo(IniFile*, LEVELINFO*, const char*) { std::abort(); }
bool netMapRead(const char* name, std::vector<uint8_t>&, std::string&)
{
    readMapName = name;
    return true; // File I/O is outside this policy test.
}
void netPrepareCurrentMapSettings()
{
    ++preparedMaps;
    // The real network bootstrap marks named custom maps for registration.
    // Restart must suppress that registration while preserving the map's
    // standalone identity in GAMEOPTIONS for subsequent voting policy.
    gPacketStartGame.userMap = gGameOptions.szUserMap[0] != 0;
    gPacketStartGame.episodeId = gGameOptions.nEpisode;
    gPacketStartGame.levelId = gGameOptions.nLevel;
}
int seqRegisterClient(void (*)(int, int)) { return 0; }
void levelGetNextLevels(int episode, int level, int* normal, int* secret)
{
    *normal = gEpisodeInfo[episode].levelsInfo[level].EndingA;
    *secret = gEpisodeInfo[episode].levelsInfo[level].EndingB;
}
namespace loguru {
Verbosity current_verbosity_cutoff() { return Verbosity_OFF; }
void log(Verbosity, const char*, unsigned, const char*, ...) {}
}

namespace {
unsigned assertions = 0;
void check(bool condition, const char* expression, int line)
{
    ++assertions;
    if (!condition) { std::fprintf(stderr, "FAIL line %d: %s\n", line, expression); std::exit(1); }
}
#define CHECK(value) check(bool(value), #value, __LINE__)

void reset()
{
    settings = NetServerSettings();
    settings.gameType = kGameTypeBloodBath;
    pendingAdvance = false;
    pendingLimit = NetServerLimit::NotReached;
    rotation.clear();
    initialMaps.clear();
    gDedicated = gGameStarted = isServer = true;
    gStartNewGame = false;
    cinematicActive = false;
    intermissionActive = false;
    intermissions = 0;
    nowMilliseconds = intermissionStarted = 0;
    preparedMaps = 0;
    queuedOutros = 0;
    outroEpisode = outroLevel = -1;
    ready2send = 1;
    gLevelTime = 0;
    gGameOptions = {};
    gGameOptions.nEpisode = 1;
    gGameOptions.nLevel = 0;
    nextEpisode = nextLevel = -9;
    connecthead = 0;
    std::fill(std::begin(connectpoint2), std::end(connectpoint2), -1);
    for (auto& player : gPlayer) player.fragCount = 0;
    std::fill(std::begin(gPlayerScores), std::end(gPlayerScores), 0);
}

void limitBoundaries()
{
    NetServerSettings rule;
    int scores[] = {-100, 0, 4, 9};
    CHECK(netServerLimitReached(rule, UINT64_MAX, scores, 4) == NetServerLimit::NotReached);
    rule.gameType = kGameTypeBloodBath;
    rule.timeLimitMinutes = 1;
    CHECK(netServerLimitReached(rule, 1799, nullptr, 0) == NetServerLimit::NotReached);
    CHECK(netServerLimitReached(rule, 1800, nullptr, 0) == NetServerLimit::Time);
    CHECK(netServerLimitReached(rule, UINT64_MAX, nullptr, 0) == NetServerLimit::Time);
    rule.timeLimitMinutes = 10080;
    CHECK(netServerLimitReached(rule, 18143999, nullptr, 0) == NetServerLimit::NotReached);
    CHECK(netServerLimitReached(rule, 18144000, nullptr, 0) == NetServerLimit::Time);
    rule.timeLimitMinutes = 0;
    rule.scoreLimit = 10;
    CHECK(netServerLimitReached(rule, UINT64_MAX, scores, 4) == NetServerLimit::NotReached);
    scores[3] = 10;
    CHECK(netServerLimitReached(rule, 0, scores, 4) == NetServerLimit::Score);
    CHECK(netServerLimitReached(rule, 0, scores, 3) == NetServerLimit::NotReached);
    rule.gameType = kGameTypeTeams;
    CHECK(netServerLimitReached(rule, 0, scores, 4) == NetServerLimit::Score);
    rule.gameType = kGameTypeCoop;
    CHECK(netServerLimitReached(rule, UINT64_MAX, scores, 4) == NetServerLimit::NotReached);
    rule.gameType = kGameTypeBloodBath;
    rule.scoreLimit = INT_MAX;
    scores[3] = INT_MAX - 1;
    CHECK(netServerLimitReached(rule, 0, scores, 4) == NetServerLimit::NotReached);
    scores[3] = INT_MAX;
    CHECK(netServerLimitReached(rule, 0, scores, 4) == NetServerLimit::Score);
    rule.timeLimitMinutes = 1;
    CHECK(netServerLimitReached(rule, 1800, scores, 4) == NetServerLimit::Time);
}

void authoritativeScores()
{
    // The real checker must read active individual frag counts for Bloodbath.
    reset(); settings.scoreLimit = 5;
    connectpoint2[0] = 3;
    gPlayer[1].fragCount = 100; // Disconnected stale slot must not finish a map.
    gPlayerScores[0] = 100; // Team scores do not count in Bloodbath.
    gPlayer[3].fragCount = 4;
    netServerCheckLimits(); CHECK(!pendingAdvance);
    gPlayer[3].fragCount = 5;
    netServerCheckLimits(); CHECK(intermissionActive && pendingLimit == NetServerLimit::Score);
    CHECK(!pendingAdvance && !gStartNewGame);
    CHECK((gGameOptions.uGameFlags & kGameFlagContinuing) == 0);
    CHECK(nextEpisode == -9 && nextLevel == -9);
    nowMilliseconds = 14999;
    netServerUpdateIntermission(); CHECK(preparedMaps == 0 && !gStartNewGame);
    nowMilliseconds = 15000;
    netServerUpdateIntermission(); CHECK(preparedMaps == 1 && gStartNewGame);
    CHECK(nextEpisode == 1 && nextLevel == 0 && pendingLimit == NetServerLimit::NotReached);

    // Teams use team counters; individual frags cannot prematurely end a map.
    reset(); settings.gameType = kGameTypeTeams; settings.scoreLimit = 20;
    gPlayer[0].fragCount = 100;
    gPlayerScores[0] = 19; gPlayerScores[1] = -1;
    netServerCheckLimits(); CHECK(!pendingAdvance);
    gPlayerScores[1] = 20;
    netServerCheckLimits(); CHECK(intermissionActive && pendingLimit == NetServerLimit::Score);
    CHECK(!pendingAdvance && !gStartNewGame);

    reset(); settings.scoreLimit = 1; connecthead = -1;
    gPlayer[0].fragCount = 100;
    netServerCheckLimits(); CHECK(!pendingAdvance);
}

void joiningPlayerScoreReset()
{
    reset();
    gPlayer[0].fragCount = 8;
    gPlayer[3].fragCount = 17;
    std::fill(std::begin(gPlayer[3].fragInfo), std::end(gPlayer[3].fragInfo), 9);
    for (int i = 0; i < kMaxPlayers; ++i) {
        gPlayerScores[i] = 10 + i;
        gPlayerScoreTicks[i] = 100 + i;
    }
    // Hot-join uses this mode: initialize the joining player's personal score
    // while retaining an ongoing team's progress toward the configured limit.
    playerResetScores(3, false);
    CHECK(gPlayer[3].fragCount == 0 && gPlayer[0].fragCount == 8);
    for (int i = 0; i < kMaxPlayers; ++i) {
        CHECK(gPlayer[3].fragInfo[i] == 0);
        CHECK(gPlayerScores[i] == 10 + i);
        CHECK(int(gPlayerScoreTicks[i]) == 100 + i);
    }
    // The existing default remains appropriate for beginning a fresh match.
    playerResetScores(3);
    for (int i = 0; i < kMaxPlayers; ++i) {
        CHECK(gPlayerScores[i] == 0);
        CHECK(int(gPlayerScoreTicks[i]) == 0);
    }
}

void clockAndGuards()
{
    reset(); settings.timeLimitMinutes = 1; connecthead = -1;
    gLevelTime = 1799;
    netServerCheckLimits(); CHECK(!pendingAdvance);
    gLevelTime = 1800;
    netServerCheckLimits(); CHECK(intermissionActive); // Empty servers still rotate after the pause.
    netServerCheckLimits(); CHECK(intermissions == 1 && !pendingAdvance);
    CHECK(pendingLimit == NetServerLimit::Time);
    nowMilliseconds = 14999;
    netServerUpdateIntermission(); CHECK(preparedMaps == 0 && !gStartNewGame);
    nowMilliseconds = 15000;
    netServerUpdateIntermission(); CHECK(preparedMaps == 1 && gStartNewGame);
    netServerUpdateIntermission(); CHECK(preparedMaps == 1); // Only once, even before the new map loads.
    for (int guard = 0; guard < 4; ++guard) {
        reset(); settings.timeLimitMinutes = 1; gLevelTime = 1800;
        if (guard == 0) gDedicated = false;
        if (guard == 1) isServer = false;
        if (guard == 2) gGameStarted = false;
        if (guard == 3) gStartNewGame = true;
        netServerCheckLimits(); CHECK(!pendingAdvance && !intermissionActive);
    }
}

void intermissionRotation()
{
    for (bool timeLimit : {false, true})
    {
        reset();
        rotation = {"E2M1", "E2M2"};
        settings.rotationCount = rotation.size();
        settings.rotationIndex = 1; // Verify wrapping to the first map after the pause.
        for (int level = 0; level < 2; ++level)
        {
            MapEntry entry = {};
            entry.episode = 1;
            entry.level = level;
            std::strcpy(entry.metadata.Filename, rotation[level].c_str());
            initialMaps.push_back(entry);
        }
        gGameOptions.nLevel = 1;
        std::strcpy(gGameOptions.zLevelName, "E2M2");
        if (timeLimit) { settings.timeLimitMinutes = 1; gLevelTime = 1800; }
        else { settings.scoreLimit = 5; gPlayer[0].fragCount = 5; }
        nowMilliseconds = 20000;
        netServerCheckLimits();
        CHECK(intermissions == 1 && !pendingAdvance && !gStartNewGame);
        CHECK(settings.rotationIndex == 1 && gGameOptions.nLevel == 1);
        CHECK(!(gGameOptions.uGameFlags & kGameFlagContinuing));

        // Exits, resolved votes, and an already-passed vote must not skip the pause.
        std::string error;
        size_t index = 99;
        CHECK(netServerHandleLevelExit(kLevelExitNormal) && !pendingAdvance);
        CHECK(!netServerResolveVote(NetServerVoteAction::Map, "E2M1", index, error));
        CHECK(!netServerApplyVote(NetServerVoteAction::Map, 0, error));
        CHECK(index == 99 && !error.empty());
        netServerAdvanceMap();
        CHECK(settings.rotationIndex == 1 && preparedMaps == 0 && !gStartNewGame);

        for (uint32_t elapsed : {0u, 1000u, 14999u})
        {
            nowMilliseconds = 20000 + elapsed;
            netServerCheckLimits();
            netServerUpdateIntermission();
            CHECK(intermissions == 1 && !gStartNewGame && preparedMaps == 0);
            CHECK(settings.rotationIndex == 1 && gGameOptions.nLevel == 1);
            CHECK(gLevelTime == (timeLimit ? 1800 : 0));
        }
        nowMilliseconds = 35000;
        netServerUpdateIntermission();
        CHECK(gStartNewGame && preparedMaps == 1);
        CHECK(settings.rotationIndex == 0 && gGameOptions.nLevel == 0);
        CHECK(!pendingAdvance && pendingLimit == NetServerLimit::NotReached);
        nowMilliseconds = 50000;
        netServerUpdateIntermission();
        CHECK(preparedMaps == 1 && settings.rotationIndex == 0);
    }

    // An explicit administrator restart cancels the pending limit transition.
    reset(); settings.scoreLimit = 1; gPlayer[0].fragCount = 1;
    netServerCheckLimits(); CHECK(intermissionActive);
    netServerRestartLevel();
    CHECK(gStartNewGame && pendingLimit == NetServerLimit::NotReached);
    nowMilliseconds = 15000;
    netServerUpdateIntermission(); CHECK(preparedMaps == 1);

    // Console map/changelevel and disconnection use the network lifecycle reset,
    // bypassing AdvanceMap/RestartLevel. Their old deferred limit must not stop
    // the next match from recognizing a newly reached limit.
    reset(); settings.scoreLimit = 1; gPlayer[0].fragCount = 1;
    netServerCheckLimits(); CHECK(intermissionActive && intermissions == 1);
    netServerCancelIntermission();
    CHECK(pendingLimit == NetServerLimit::NotReached);
    intermissionActive = false; // The network lifecycle also clears its presentation state.
    gPlayer[0].fragCount = 0;
    nowMilliseconds = 15000;
    netServerUpdateIntermission(); CHECK(preparedMaps == 0 && !gStartNewGame);
    netServerCheckLimits(); CHECK(!intermissionActive);
    gPlayer[0].fragCount = 1;
    netServerCheckLimits(); CHECK(intermissionActive && intermissions == 2);
    nowMilliseconds = 29999;
    netServerUpdateIntermission(); CHECK(preparedMaps == 0 && !gStartNewGame);
    nowMilliseconds = 30000;
    netServerUpdateIntermission(); CHECK(preparedMaps == 1 && gStartNewGame);
}

void coopRoutes()
{
    for (int exitType : {kLevelExitNormal, kLevelExitSecret}) {
        reset(); settings.gameType = kGameTypeCoop; settings.episode = 2;
        gEpisodeInfo[1].nLevels = 3;
        gEpisodeInfo[1].levelsInfo[0].EndingA = 1;
        gEpisodeInfo[1].levelsInfo[0].EndingB = 2;
        CHECK(netServerHandleLevelExit(exitType));
        CHECK(pendingAdvance && nextEpisode == 1);
        CHECK(nextLevel == (exitType == kLevelExitNormal ? 1 : 2));
        CHECK(preserveInventory);
        CHECK((gGameOptions.uGameFlags & kGameFlagContinuing) != 0);
        CHECK(queuedOutros == 0); // Moving within an episode has no episode outro.
    }
    reset(); settings.gameType = kGameTypeCoop; settings.episode = 2;
    gEpisodeInfo[1].nLevels = 3;
    gEpisodeInfo[1].levelsInfo[0].EndingA = -1;
    CHECK(netServerHandleLevelExit(kLevelExitNormal));
    CHECK(nextEpisode == 1 && nextLevel == 0); // Episode loops when complete.
    CHECK(!preserveInventory);
    CHECK(queuedOutros == 1 && outroEpisode == 1 && outroLevel == 0);
    CHECK(netServerHandleLevelExit(kLevelExitNormal));
    CHECK(queuedOutros == 1); // Repeated exits cannot queue the same outro twice.
    reset(); settings.gameType = kGameTypeCoop; settings.timeLimitMinutes = 1;
    gEpisodeInfo[1].nLevels = 3;
    gEpisodeInfo[1].levelsInfo[0].EndingA = 1;
    gLevelTime = 1800;
    netServerCheckLimits();
    CHECK(intermissionActive && !pendingAdvance && !gStartNewGame);
    nowMilliseconds = 15000;
    netServerUpdateIntermission();
    CHECK(gStartNewGame && nextLevel == 0 && !preserveInventory); // Single-map timer restarts.
    CHECK(queuedOutros == 0);
    reset(); settings.gameType = kGameTypeCoop; settings.timeLimitMinutes = 1; settings.episode = 2;
    gLevelTime = 1800;
    netServerCheckLimits();
    CHECK(intermissionActive && !pendingAdvance && !gStartNewGame);
    nowMilliseconds = 15000;
    netServerUpdateIntermission();
    CHECK(gStartNewGame && nextLevel == 1 && preserveInventory); // Episode timer advances.
    CHECK(queuedOutros == 0);
    reset(); settings.gameType = kGameTypeCoop; settings.episode = 2;
    gGameOptions.nLevel = 2;
    gEpisodeInfo[1].levelsInfo[2].EndingA = -1;
    CHECK(netServerHandleLevelExit(kLevelExitNormal));
    CHECK(queuedOutros == 1 && outroEpisode == 1 && outroLevel == 2);
    CHECK(nextLevel == 0 && !preserveInventory);
    reset(); settings.gameType = kGameTypeCoop; settings.episode = 2;
    rotation = {"E2M1", "E2M2"};
    gEpisodeInfo[1].levelsInfo[0].EndingA = -1;
    CHECK(netServerHandleLevelExit(kLevelExitNormal));
    CHECK(queuedOutros == 0); // A rotation does not complete the selected episode.
    reset(); gDedicated = false;
    CHECK(!netServerHandleLevelExit(kLevelExitNormal) && !pendingAdvance);
    reset(); isServer = false;
    CHECK(!netServerHandleLevelExit(kLevelExitNormal) && !pendingAdvance);
}

void mapVotePolicy()
{
    std::string error, target;
    size_t index = 77;
    reset();
    CHECK(settings.mapVoteEnabled);
    CHECK(netServerVoteMaps().empty());
    CHECK(!netServerResolveVote(NetServerVoteAction::Map, "E1M2", index, error));
    CHECK(error.find("rotation") != std::string::npos && index == 77);
    rotation = {"E1M1", "E1M2.MAP", "maps/custom.map"};
    settings.rotationCount = rotation.size();
    CHECK(netServerVoteMaps() == rotation);
    const GAMEOPTIONS original = gGameOptions;
    for (const auto& name : {"E1M2", "e1m2", "E1M2.MAP", "e1m2.map"}) {
        CHECK(netServerResolveVote(NetServerVoteAction::Map, name, index, error));
        CHECK(index == 1 && error.empty());
        CHECK(std::memcmp(&gGameOptions, &original, sizeof(original)) == 0);
    }
    CHECK(netServerResolveVote(NetServerVoteAction::Map, "MAPS\\CUSTOM.MAP", index, error) && index == 2);
    for (const auto& name : {"E1M3", "E1M", "../E1M2", "E1M2.MAP extra", ""}) {
        index = 77;
        CHECK(!netServerResolveVote(NetServerVoteAction::Map, name, index, error));
        CHECK(index == 77 && !error.empty());
    }
    for (auto action : {NetServerVoteAction::Restart, NetServerVoteAction::Next})
        CHECK(!netServerResolveVote(action, "", index, error));
    settings.gameType = kGameTypeTeams;
    CHECK(netServerResolveVote(NetServerVoteAction::Map, "E1M1", index, error) && index == 0);
    CHECK(netServerVoteMaps() == rotation);
    settings.mapVoteEnabled = false;
    CHECK(netServerVoteMaps().empty());
    CHECK(!netServerResolveVote(NetServerVoteAction::Map, "E1M1", index, error));
    CHECK(error.find("disabled") != std::string::npos);

    for (int guard = 0; guard < 8; ++guard) {
        reset(); rotation = {"E1M1"};
        if (guard == 0) isServer = false;
        if (guard == 1) gGameStarted = false;
        if (guard == 2) gStartNewGame = true;
        if (guard == 3) pendingAdvance = true;
        if (guard == 4) gGameOptions.uGameFlags = kGameFlagContinuing;
        if (guard == 5) ready2send = 0;
        if (guard == 6) cinematicActive = true;
        if (guard == 7) intermissionActive = true;
        CHECK(!netServerResolveVote(NetServerVoteAction::Map, "E1M1", index, error));
        CHECK(!error.empty());
    }

    reset(); settings.gameType = kGameTypeCoop; settings.episode = 2;
    gEpisodeInfo[1].nLevels = 3;
    gEpisodeInfo[1].levelsInfo[0].EndingA = 1;
    gEpisodeInfo[1].levelsInfo[0].EndingB = 2;
    std::strcpy(gEpisodeInfo[1].levelsInfo[0].Filename, "E2M1");
    std::strcpy(gEpisodeInfo[1].levelsInfo[1].Filename, "E2M2");
    std::strcpy(gEpisodeInfo[1].levelsInfo[2].Filename, "E2M3");
    std::strcpy(gGameOptions.zLevelName, "E2M1");
    CHECK(netServerVoteMaps().empty());
    CHECK(!netServerResolveVote(NetServerVoteAction::Map, "E2M2", index, error));
    CHECK(netServerResolveVote(NetServerVoteAction::Restart, "", index, error));
    CHECK(netServerResolveVote(NetServerVoteAction::Next, "", index, error));
    cinematicActive = true;
    CHECK(!netServerResolveVote(NetServerVoteAction::Restart, "", index, error));
    CHECK(!netServerResolveVote(NetServerVoteAction::Next, "", index, error));
    CHECK(error.find("in progress") != std::string::npos);
    cinematicActive = false;
    CHECK(nextVoteMap(target, error) && target == "E2M2");
    VoteNextLevel next;
    CHECK(nextVoteEpisode(next, error) && next.episode == 1 && next.level == 1 && next.preserveInventory);
    CHECK(!netServerResolveVote(NetServerVoteAction::Restart, "E2M1", index, error));
    CHECK(!netServerResolveVote(NetServerVoteAction::Next, "E2M2", index, error));
    gEpisodeInfo[1].levelsInfo[0].EndingA = -1;
    CHECK(nextVoteMap(target, error) && target == "E2M1");
    CHECK(nextVoteEpisode(next, error) && next.episode == 1 && next.level == 0 && !next.preserveInventory);
    rotation = {"E1M1", "E1M2"}; settings.rotationIndex = 1;
    CHECK(nextVoteMap(target, error) && target == "E1M1");
    rotation.clear();
    std::strcpy(gGameOptions.szUserMap, "custom.map");
    CHECK(netServerResolveVote(NetServerVoteAction::Restart, "", index, error));
    CHECK(!netServerResolveVote(NetServerVoteAction::Next, "", index, error));
    gGameOptions.szUserMap[0] = 0;

    // Listen servers use their actual game mode, never stale dedicated rules.
    gDedicated = false; gGameOptions.nGameType = kGameTypeCoop;
    settings.gameType = kGameTypeBloodBath;
    CHECK(netServerResolveVote(NetServerVoteAction::Restart, "", index, error));
    CHECK(netServerResolveVote(NetServerVoteAction::Next, "", index, error));
    CHECK(nextVoteMap(target, error) && target == "E2M1");
    rotation = {"E1M1"}; settings.gameType = kGameTypeCoop;
    gGameOptions.nGameType = kGameTypeBloodBath;
    CHECK(netServerVoteMaps().empty());
    CHECK(!netServerResolveVote(NetServerVoteAction::Map, "E1M1", index, error));
    CHECK(!netServerResolveVote(NetServerVoteAction::Restart, "", index, error));
}

void listenCustomRestart()
{
    reset();
    gDedicated = false;
    gGameOptions.nGameType = kGameTypeCoop;
    gGameOptions.nEpisode = 0;
    gGameOptions.nLevel = 9;
    // GUI hosting reaches this path with an already registered map slot and
    // szUserMap populated by netBroadcastNewGame, unlike CLI -map startup.
    std::strcpy(gGameOptions.szUserMap, "custom.map");
    std::strcpy(gGameOptions.zLevelName, "custom");
    gPacketStartGame = {};
    preparedMaps = 0;
    const GAMEOPTIONS original = gGameOptions;
    std::string error;
    CHECK(netServerApplyVote(NetServerVoteAction::Restart, 0, error));
    CHECK(error.empty() && readMapName == "custom.map" && preparedMaps == 1);
    CHECK(gStartNewGame && gPacketStartGame.userMap == 0);
    CHECK(gPacketStartGame.episodeId == 0 && gPacketStartGame.levelId == 9);
    CHECK(std::memcmp(&original, &gGameOptions, sizeof(original)) == 0);
    CHECK(queuedOutros == 0);
}

void listenCoopNextCinematic()
{
    for (bool finalLevel : {false, true})
    {
        reset();
        gDedicated = false;
        gGameOptions.nGameType = kGameTypeCoop;
        gGameOptions.nLevel = finalLevel ? 2 : 0;
        gEpisodeInfo[1].nLevels = 3;
        gEpisodeInfo[1].levelsInfo[0].EndingA = 1;
        gEpisodeInfo[1].levelsInfo[2].EndingA = -1;
        std::strcpy(gEpisodeInfo[1].levelsInfo[0].Filename, "E2M1");
        std::strcpy(gEpisodeInfo[1].levelsInfo[1].Filename, "E2M2");
        std::strcpy(gEpisodeInfo[1].levelsInfo[2].Filename, "E2M3");
        std::string error;
        CHECK(netServerApplyVote(NetServerVoteAction::Next, 0, error));
        CHECK(error.empty() && gStartNewGame);
        CHECK(gGameOptions.nEpisode == 1 && gGameOptions.nLevel == (finalLevel ? 0 : 1));
        CHECK(queuedOutros == (finalLevel ? 1 : 0));
        if (finalLevel)
            CHECK(outroEpisode == 1 && outroLevel == 2); // Capture the outgoing map before selecting the next one.
        else
            CHECK((gGameOptions.uGameFlags & kGameFlagContinuing) != 0);
    }
}
}

int main()
{
    limitBoundaries(); authoritativeScores(); joiningPlayerScoreReset(); clockAndGuards(); intermissionRotation(); coopRoutes(); mapVotePolicy(); listenCustomRestart(); listenCoopNextCinematic();
    std::printf("PASS: dedicated limits, timed intermissions, hot-join scores, map vote policy, cinematic guards and coop exits (%u assertions)\n", assertions);
}
