// Tests the production ACK/replay and render interpolation implementation with
// a deterministic movement primitive. No game assets, simulation, or renderer.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "compat.h"
#include "build.h"
#include "common_game.h"
#include "mmulti.h"
#include "netpredict.h"
#include "player.h"
#include "view.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

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
static bool client = true, playback = false;
static uint32_t clockMs = 0, generations[MAXSPRITES];
static vec3_t predicted;
static unsigned liveWeaponCalls = 0, replayWeaponCalls = 0;

bool netIsClient() { return client; }
bool netDemoIsPlaying() { return playback; }
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
{ if (replay) ++replayWeaponCalls; else { ++liveWeaponCalls; netPredictLocalSound(123); } }

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
    client = true; playback = false; clockMs = 0;
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
    netPredictSnapshot(ack, 1, true);
}
static void snapshot(uint32_t ack, uint32_t tick, int x)
{
    netPredictBeforeSnapshot();
    sprite[0].x = x;
    clockMs += 34;
    netPredictSnapshot(ack, tick, false);
}

int main()
{
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
    std::puts("prediction: ACK/replay, wrap, correction, overflow recovery, feedback dedup, interpolation, generation, teleport passed");
}
