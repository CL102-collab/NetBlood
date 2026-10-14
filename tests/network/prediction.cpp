// Tests the production ACK/replay and render interpolation implementation with
// a deterministic movement primitive. No game assets, simulation, or renderer.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "compat.h"
#include "build.h"
#include "common_game.h"
#include "cosmeticrandom.h"
#include "mmulti.h"
#include "netpredict.h"
#include "player.h"
#include "view.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <array>
#include <climits>
#include <vector>

int32_t randomseed = 0x12345678;
uint32_t wrandomseed = 0x23456789;
unsigned int randSeed = 0x34567890;

sectortype sector[MAXSECTORS + M32_FIXME_SECTORS];
walltype wall[MAXWALLS + M32_FIXME_WALLS];
spritetype sprite[MAXSPRITES];
uint32_t sectorchanged[MAXSECTORS + M32_FIXME_SECTORS];
uint32_t wallchanged[MAXWALLS + M32_FIXME_WALLS];
picanm_t picanm[MAXTILES];
int16_t numsectors = 1, numwalls = 1;
PLAYER gPlayer[kMaxPlayers];
PLAYER *gMe = &gPlayer[0], *gView = &gPlayer[0];
XSPRITE localXSprite;
int connecthead = 0, connectpoint2[MAXMULTIPLAYERS];
int32_t gViewInterpolate = 1;
bool gPrediction = true;
static bool client = true, playback = false, intermission = false;
static uint32_t clockMs = 0, generations[MAXSPRITES];
static vec3_t predicted;
static unsigned liveWeaponCalls = 0, replayWeaponCalls = 0;
static std::vector<std::array<int, 3>> liveWeaponChoices, replayWeaponChoices;
static std::vector<int> liveWeaponSounds;

bool netIsClient() { return client; }
bool netDemoIsPlaying() { return playback; }
bool netIntermissionActive() { return intermission; }
uint32_t timerGetTicks() { return clockMs; }
uint32_t netSnapshotSpriteGeneration(int n) { return generations[n]; }
void netLagFrame(int) {}
void viewBackupView(int) {}
void viewInitializePrediction() { predicted = { gMe->pSprite->x, gMe->pSprite->y, gMe->pSprite->z }; }
void viewUpdatePrediction(GINPUT *input) { predicted.x += input->forward; predicted.y += input->strafe; }
vec3_t viewPredictionPosition() { return predicted; }
vec2_t viewPredictionPendingLook() { return {0, 0}; }
void viewPredictionApplyPendingLook(vec2_t) {}
void viewPredictionSnap() {}
void WeaponPredictReset(PLAYER const *) {}
void WeaponPredictCommand(PLAYER *, GINPUT const &, bool replay)
{
    // Animation choices need to survive reconciliation; audio must only play
    // for live commands. Exercise the production helpers for both paths.
    std::array<int, 3> choices = {{netPredictCosmeticRandom(4), netPredictCosmeticRandom(17), netPredictCosmeticRandom(4)}};
    if (replay) { ++replayWeaponCalls; replayWeaponChoices.push_back(choices); }
    else
    {
        ++liveWeaponCalls;
        liveWeaponChoices.push_back(choices);
        liveWeaponSounds.push_back(netPredictLocalSound(123));
    }
}

static void check(bool value, const char *message)
{
    if (!value) { std::fprintf(stderr, "prediction test failed: %s\n", message); std::exit(1); }
}
static GINPUT input(int forward)
{
    GINPUT result = {};
    result.forward = int16_t(forward);
    result.buttonFlags.shoot = 1;
    return result;
}
static void reset(uint32_t ack = 0)
{
    netPredictReset();
    client = true; playback = false; intermission = false; clockMs = 0;
    for (auto &player : gPlayer) player = {};
    std::memset(sprite, 0, sizeof(sprite));
    std::memset(generations, 0, sizeof(generations));
    std::memset(&localXSprite, 0, sizeof(localXSprite));
    for (int &next : connectpoint2) next = -1;
    sprite[0].index = 0; sprite[0].sectnum = 0; sprite[0].statnum = 6;
    sprite[1].index = 1; sprite[1].sectnum = 0; sprite[1].statnum = 6;
    gMe = &gPlayer[0]; gMe->pSprite = &sprite[0]; gMe->pXSprite = &localXSprite;
    localXSprite.health = 100;
    predicted = {};
    liveWeaponCalls = replayWeaponCalls = 0;
    liveWeaponChoices.clear(); replayWeaponChoices.clear(); liveWeaponSounds.clear();
    netPredictSnapshot(ack, 1, true);
}
static void snapshot(uint32_t ack, uint32_t tick, int x)
{
    netPredictBeforeSnapshot();
    sprite[0].x = x;
    clockMs += 34;
    netPredictSnapshot(ack, tick, false);
}

static void checkCosmeticRandom()
{
    CosmeticRandomGenerator a(0x89abcdef), b(0x89abcdef);
    for (unsigned i = 0; i < 128; ++i)
        check(a.next() == b.next(), "private generators reproduce a seeded sequence independently");
    uint32_t saved = a.state();
    check(a.bounded(0) == 0 && a.bounded(1) == 0 && a.state() == saved,
          "empty and singleton ranges do not advance the cosmetic generator");
    uint32_t next = a.next();
    a.seed(saved);
    check(a.next() == next, "restoring cosmetic state reproduces the next draw for save-preview isolation");
    a.seed(0); b.seed(0);
    next = a.next();
    check(next != 0 && next == b.next(), "zero is a valid reproducible seed and does not lock the generator");
    unsigned seen = 0;
    a.seed(0x13579bdf);
    for (unsigned i = 0; i < 256; ++i)
    {
        uint32_t value = a.bounded(4);
        check(value < 4, "private variant selection stays within the sound range");
        seen |= 1u << value;
    }
    check(seen == 15, "private variant selection reaches all four alternatives");
    for (uint32_t bound : {2u, 3u, 17u, 32769u, 0x80000001u, 0xffffffffu})
        for (unsigned i = 0; i < 1024; ++i)
            check(a.bounded(bound) < bound, "private unsigned bounds include large and non-power-of-two ranges safely");

    auto &cosmetic = cosmeticRandomGenerator();
    saved = cosmetic.state();
    check(CosmeticRandom(INT_MIN) == 0 && CosmeticRandom(-1) == 0 && CosmeticRandom(0) == 0 && CosmeticRandom(1) == 0 &&
          CosmeticRandom2(INT_MIN) == 0 && CosmeticRandom2(-1) == 0 && CosmeticRandom2(0) == 0 && cosmetic.state() == saved,
          "empty signed cosmetic ranges do not advance state or overflow");
    cosmetic.seed(0x2468ace0);
    bool positive = false, negative = false;
    for (unsigned i = 0; i < 4096; ++i)
    {
        int value = CosmeticRandom(INT_MAX);
        check(value >= 0 && value < INT_MAX, "largest signed cosmetic range stays bounded");
        value = CosmeticRandom2(INT_MAX);
        check(int64_t(value) >= -int64_t(INT_MAX) && value < INT_MAX,
              "largest centered cosmetic range stays bounded without signed overflow");
        positive |= value > 0; negative |= value < 0;
        value = CosmeticRandom2(1);
        check(value == -1 || value == 0, "centered unit range preserves its exclusive upper bound");
    }
    check(positive && negative, "centered cosmetic choices vary on both sides of zero");
    check(randomseed == 0x12345678 && wrandomseed == 0x23456789 && randSeed == 0x34567890,
          "cosmetic draws leave all legacy gameplay and presentation RNG states untouched");
    cosmetic.seed(saved);
}

static void checkPredictedCosmetics()
{
    reset();
    auto &cosmetic = cosmeticRandomGenerator();
    cosmetic.seed(0x12345678);
    CosmeticRandomGenerator expected(0x12345678);
    for (uint32_t sequence = 1; sequence <= 32; ++sequence)
    {
        netPredictCommand(sequence, input(1));
        expected.next();
    }
    check(cosmetic.state() == expected.state(), "each accepted input command draws exactly one cosmetic seed");
    unsigned choices = 0;
    for (auto const &choice : liveWeaponChoices)
    {
        check(choice[0] >= 0 && choice[0] < 4 && choice[1] >= 0 && choice[1] < 17 && choice[2] >= 0 && choice[2] < 4,
              "command-specific animation choices stay within their ranges");
        choices |= 1u << choice[0];
    }
    check(choices == 15, "predicted animations use all four cosmetic alternatives");
    check(liveWeaponSounds.size() == 32 && liveWeaponSounds.front() == 123 && liveWeaponSounds.back() == 123,
          "single-variant prediction preserves the original sample ID");
    uint32_t saved = cosmetic.state();
    netPredictCommand(32, input(100)); netPredictCommand(31, input(100));
    check(cosmetic.state() == saved && liveWeaponChoices.size() == 32,
          "duplicate and stale commands neither reroll cosmetic choices nor emit audio");

    unsigned variants = 0;
    for (unsigned i = 0; i < 128; ++i)
    {
        int sound = netPredictLocalSound(200, 4, 18);
        check(sound >= 200 && sound < 204, "predicted sample uses a cosmetic variant inside the QAV range");
        variants |= 1u << (sound - 200);
        check(!netPredictSuppressSound(0, 200, 31, 18) && !netPredictSuppressSound(0, 200, 32, 17) &&
              !netPredictSuppressSound(0, 204, 32, 18), "random prediction does not suppress another command, channel, or range");
        int authoritative = 200 + (sound - 200 + 1) % 4;
        check(netPredictSuppressSound(0, authoritative, 32, 18) && !netPredictSuppressSound(0, sound, 32, 18),
              "a different authoritative variant consumes exactly one prediction using the original sound range");
    }
    check(variants == 15, "predicted weapon audio varies across all QAV alternatives");
    saved = cosmetic.state();
    check(netPredictLocalSound(321, 1) == 321 && netPredictLocalSound(322, 0) == 322 &&
          netPredictLocalSound(323, INT_MIN) == 323 && netPredictLocalSound(INT_MAX, 1000) == INT_MAX && cosmetic.state() == saved,
          "single-variant and absent-range sounds do not consume cosmetic randomness");
    for (unsigned i = 0; i < 16; ++i)
    {
        int sound = netPredictLocalSound(INT_MAX - 1, 1000, 19);
        check(sound == INT_MAX - 1 || sound == INT_MAX, "QAV variant count cannot overflow the returned sample ID");
        check(netPredictSuppressSound(0, INT_MAX, 32, 19), "bounded sound range still suppresses the final representable variant");
    }
    saved = cosmetic.state();
    snapshot(16, 2, 16);
    check(cosmetic.state() == saved && liveWeaponChoices.size() == 32 && liveWeaponSounds.size() == 32,
          "reconciliation does not consume the global cosmetic generator or replay sound feedback");
    check(replayWeaponChoices.size() == 16, "only pending commands replay their cosmetic animation choices");
    for (size_t i = 0; i < replayWeaponChoices.size(); ++i)
        check(replayWeaponChoices[i] == liveWeaponChoices[i + 16],
              "ACK replay reproduces every command-specific choice despite unrelated cosmetic sound draws");
    replayWeaponChoices.clear();
    snapshot(16, 3, 16);
    check(cosmetic.state() == saved && replayWeaponChoices.size() == 16,
          "repeated reconciliation reuses the same per-command streams without global draws");
    for (size_t i = 0; i < replayWeaponChoices.size(); ++i)
        check(replayWeaponChoices[i] == liveWeaponChoices[i + 16], "repeated replay keeps animation alternatives stable");

    intermission = true;
    netPredictCommand(33, input(100));
    check(netPredictLocalSound(200, 4, 18) == 200 && cosmetic.state() == saved,
          "intermission rejects new cosmetic predictions without consuming the stream");
    intermission = false; client = false;
    netPredictCommand(33, input(100));
    check(netPredictLocalSound(200, 4, 18) == 200 && cosmetic.state() == saved,
          "offline prediction does not consume cosmetic randomness");
    check(randomseed == 0x12345678 && wrandomseed == 0x23456789 && randSeed == 0x34567890,
          "live prediction and replay leave every legacy RNG state untouched");
}

int main()
{
    checkCosmeticRandom();
    checkPredictedCosmetics();
    picanm[100].num = 3; picanm[104].num = 2;
    check(viewAnimatedTile(100, 2) == 107, "ordinary chained sprite animation preserves artwork");
    check(viewAnimatedTile(kMaxTiles-1, 0) == kMaxTiles-1 && viewAnimatedTile(kMaxTiles-1, 1) == -1,
          "animation at final tile cannot walk beyond array");
    picanm[kMaxTiles-2].num = 255;
    check(viewAnimatedTile(kMaxTiles-2, 1) == -1 && viewAnimatedTile(0, 0x7fffffff) == -1 &&
          viewAnimatedTile(-1, 0) == -1, "untrusted animation counts/asset steps bounded before indexing");
    reset();
    netPredictCommand(1, input(10)); netPredictCommand(2, input(10)); netPredictCommand(3, input(10));
    check(predicted.x == 30 && sprite[0].x == 0, "movement is immediate and authoritative state untouched");
    netPredictCommand(3, input(500)); netPredictCommand(2, input(500));
    check(predicted.x == 30 && liveWeaponCalls == 3, "duplicate/out-of-order command has no movement or feedback");
    check(!netPredictSuppressSound(1, 123, 1), "another player's sound never suppressed");
    check(netPredictSuppressSound(0, 123, 1) && !netPredictSuppressSound(0, 123, 1), "one authoritative sound echo consumes exactly one prediction");
    check(!netPredictSuppressSound(0, 124, 2) && !netPredictSuppressSound(0, 123, 99), "unpredicted sound/command remains audible");
    netPredictLocalSound(200, 4, 18);
    check(!netPredictSuppressSound(0, 202, 3, -1) && !netPredictSuppressSound(0, 204, 3, 18),
          "predicted variants never suppress unrelated channels or sound IDs");
    check(netPredictSuppressSound(0, 203, 3, 18) && !netPredictSuppressSound(0, 200, 3, 18),
          "authoritative random QAV variant consumes one equivalent prediction");
    snapshot(1, 2, 10);
    check(predicted.x == 30 && netPredictGetStats().pendingCommands == 2, "ACK removes only executed commands then replays suffix");
    check(replayWeaponCalls == 2 && liveWeaponCalls == 3 && netPredictGetStats().predictedSounds == 4, "reconciliation feedback is marked replay");
    snapshot(1, 3, 4);
    check(predicted.x == 24 && netPredictGetStats().lastCorrection == 6, "authoritative correction plus pending movement");
    snapshot(3, 4, 18);
    check(predicted.x == 18 && netPredictGetStats().pendingCommands == 0, "fully acknowledged history retires");
    reset(0xfffffffdu);
    for (uint32_t seq : {0xfffffffeu, 0xffffffffu, 0u, 1u}) netPredictCommand(seq, input(10));
    snapshot(0xffffffffu, 2, 20);
    check(predicted.x == 40 && netPredictGetStats().pendingCommands == 2, "sequence wrap keeps newer inputs 0 and 1");
    reset();
    for (uint32_t seq = 1; seq <= 257; ++seq) netPredictCommand(seq, input(10));
    check(netPredictGetStats().historyOverflows == 1 && netPredictGetStats().pendingCommands == 256 && predicted.x == 2560,
          "bounded overflow stops speculative replay of incomplete history");
    snapshot(1, 2, 10);
    check(predicted.x == 2570 && netPredictGetStats().pendingCommands == 256, "prediction recovers once ACK closes history gap");
    reset();
    netPredictCommand(1, input(10)); netPredictCommand(2, input(10));
    intermission = true;
    check(!netPredictActive(), "intermission disables predicted movement and weapon presentation");
    netPredictCommand(3, input(100));
    netPredictLocalSound(124);
    check(predicted.x == 20 && liveWeaponCalls == 2 && netPredictGetStats().predictedSounds == 2,
          "held movement/fire and direct sound feedback cannot predict during intermission");
    snapshot(1, 2, 10);
    check(predicted.x == 10 && replayWeaponCalls == 0 && netPredictGetStats().pendingCommands == 0,
          "final snapshot discards unacknowledged commands instead of replaying past match end");
    netPredictCommand(4, input(100));
    snapshot(1, 3, 10);
    check(predicted.x == 10 && replayWeaponCalls == 0 && liveWeaponCalls == 2 && netPredictLocalFraction() == 65536,
          "repeated intermission snapshots remain at final authoritative position");
    intermission = false;
    check(!netPredictSuppressSound(0, 123, 2), "intermission discards predicted sound suppression history");
    reset();
    netPredictCommand(1, input(10));
    check(netPredictActive() && predicted.x == 10 && liveWeaponCalls == 1,
          "new map reset resumes prediction with fresh command sequence");
    reset();
    sprite[1].x = 100; wall[0].x = 100; sector[0].floorz = 1000;
    netPredictBeforeSnapshot();
    sprite[1].x = 200; wall[0].x = 200; sector[0].floorz = 2000;
    clockMs = 34; netPredictSnapshot(0, 2, false); clockMs += 17;
    netPredictBeginRender();
    tspritetype render = {}; render.x = 200; render.sectnum = 0;
    netPredictInterpolateSprite(1, &render);
    check(render.x == 150 && sprite[1].x == 200 && wall[0].x == 150 && sector[0].floorz == 1500,
          "remote entity/world render at interpolated time without mutating authoritative entity");
    netPredictEndRender();
    check(wall[0].x == 200 && sector[0].floorz == 2000, "world geometry restored after render");
    intermission = true; render.x = 200;
    netPredictBeginRender(); netPredictInterpolateSprite(1, &render); netPredictEndRender();
    check(render.x == 200 && wall[0].x == 200 && sector[0].floorz == 2000,
          "intermission renders final authoritative entities and geometry without interpolation");
    intermission = false;
    ++generations[1]; render.x = 200;
    netPredictBeginRender(); netPredictInterpolateSprite(1, &render); netPredictEndRender();
    check(render.x == 200, "reused entity slot snaps across generation change");
    netPredictBeforeSnapshot(); sprite[1].x = 20000; netPredictSnapshot(0, 3, false);
    render.x = 20000; netPredictBeginRender(); netPredictInterpolateSprite(1, &render); netPredictEndRender();
    check(render.x == 20000, "teleport never streaks across map");
    client = false; playback = true;
    check(netPredictActive(), "packet demo uses snapshot presentation");
    playback = false;
    check(!netPredictActive(), "offline and legacy demo paths stay independent");
    netPredictReset(); check(!netPredictActive() && netPredictGetStats().commands == 0, "session reset clears prediction history");
    std::puts("prediction: cosmetic RNG isolation/variation, stable animation replay, ACK/replay, wrap, correction, overflow recovery, feedback dedup, intermission freeze, interpolation, generation, teleport passed");
}
