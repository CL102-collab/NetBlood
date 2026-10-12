// SPDX-License-Identifier: GPL-2.0-or-later
// Run the real dedicated limit checker against controlled player/team scores.
// Section GC removes unrelated startup, filesystem and map-loading functions.
#include "../../source/blood/src/netserver.cpp"

#include <cstdio>
#include <cstdlib>
#include <limits>

bool gDedicated = true, gGameStarted = true, gStartNewGame = false;
int gLevelTime = 0;
int connecthead = -1, connectpoint2[MAXMULTIPLAYERS];
GAMEOPTIONS gGameOptions = {};
EPISODEINFO gEpisodeInfo[kMaxEpisodes] = {};
static bool isServer = true;
bool netIsServer() { return isServer; }
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
    rotation.clear();
    gDedicated = gGameStarted = isServer = true;
    gStartNewGame = false;
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
    netServerCheckLimits(); CHECK(pendingAdvance);
    CHECK((gGameOptions.uGameFlags & kGameFlagContinuing) != 0);
    CHECK(nextEpisode == 1 && nextLevel == 0);

    // Teams use team counters; individual frags cannot prematurely end a map.
    reset(); settings.gameType = kGameTypeTeams; settings.scoreLimit = 20;
    gPlayer[0].fragCount = 100;
    gPlayerScores[0] = 19; gPlayerScores[1] = -1;
    netServerCheckLimits(); CHECK(!pendingAdvance);
    gPlayerScores[1] = 20;
    netServerCheckLimits(); CHECK(pendingAdvance);

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
    netServerCheckLimits(); CHECK(pendingAdvance); // Empty servers still rotate.
    const int firstNextEpisode = nextEpisode;
    gGameOptions.nEpisode = 5;
    netServerCheckLimits(); CHECK(nextEpisode == firstNextEpisode); // Only once.
    for (int guard = 0; guard < 4; ++guard) {
        reset(); settings.timeLimitMinutes = 1; gLevelTime = 1800;
        if (guard == 0) gDedicated = false;
        if (guard == 1) isServer = false;
        if (guard == 2) gGameStarted = false;
        if (guard == 3) gStartNewGame = true;
        netServerCheckLimits(); CHECK(!pendingAdvance);
    }
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
    }
    reset(); settings.gameType = kGameTypeCoop; settings.episode = 2;
    gEpisodeInfo[1].nLevels = 3;
    gEpisodeInfo[1].levelsInfo[0].EndingA = -1;
    CHECK(netServerHandleLevelExit(kLevelExitNormal));
    CHECK(nextEpisode == 1 && nextLevel == 0); // Episode loops when complete.
    CHECK(!preserveInventory);
    reset(); settings.gameType = kGameTypeCoop; settings.timeLimitMinutes = 1;
    gEpisodeInfo[1].nLevels = 3;
    gEpisodeInfo[1].levelsInfo[0].EndingA = 1;
    gLevelTime = 1800;
    netServerCheckLimits();
    CHECK(pendingAdvance && nextLevel == 0 && !preserveInventory); // Single-map timer restarts.
    reset(); settings.gameType = kGameTypeCoop; settings.timeLimitMinutes = 1; settings.episode = 2;
    gLevelTime = 1800;
    netServerCheckLimits();
    CHECK(pendingAdvance && nextLevel == 1 && preserveInventory); // Episode timer advances.
    reset(); gDedicated = false;
    CHECK(!netServerHandleLevelExit(kLevelExitNormal) && !pendingAdvance);
    reset(); isServer = false;
    CHECK(!netServerHandleLevelExit(kLevelExitNormal) && !pendingAdvance);
}
}

int main()
{
    limitBoundaries(); authoritativeScores(); joiningPlayerScoreReset(); clockAndGuards(); coopRoutes();
    std::printf("PASS: dedicated limits, hot-join score retention, authoritative score sources, clock/role guards and coop exits (%u assertions)\n", assertions);
}
