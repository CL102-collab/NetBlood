// Exercise real VIEW endpoints instead of a stub viewInitializePrediction.
// Including production view.cpp lets the regression inspect private state;
// section GC removes renderer and movement code outside this camera boundary.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "../../source/blood/src/view.cpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

PLAYER gPlayer[kMaxPlayers];
PLAYER *gMe = &gPlayer[0], *gView = &gPlayer[0];
int numplayers = 1;
int xvel[kMaxSprites], yvel[kMaxSprites], zvel[kMaxSprites];
SPRITEHIT gSpriteHit[kMaxXSprites];
fix16_t gViewAngle = 0, gViewLook = 0;
// Unrelated view.cpp startup metadata; camera initialization uses neither.
POWERUPINFO gPowerUpInfo[kMaxPowerUps];
CGameMessageMgr::CGameMessageMgr() {}
static spritetype localSprite;
static XSPRITE localXSprite;
bool netPredictActive() { return true; }

namespace
{
unsigned checks = 0;
void check(bool condition, const char *message)
{
    ++checks;
    if (!condition)
    {
        std::fprintf(stderr, "camera test failed: %s\n", message);
        std::exit(1);
    }
}

// These are the real VIEW fields and the production interpolation helpers
// consumed by viewDrawScreen's predicted-camera branch (before render effects).
std::array<int32_t, 12> camera(int fraction)
{
    return {{
        interpolate(predictOld.at50, predict.at50, fraction),
        interpolate(predictOld.at54, predict.at54, fraction),
        interpolate(predictOld.at38, predict.at38, fraction),
        interpolate(predictOld.at34, predict.at34, fraction),
        interpolateangfix16(predictOld.at30, predict.at30, fraction),
        interpolate(predictOld.at24, predict.at24, fraction),
        interpolate(predictOld.at28, predict.at28, fraction),
        interpolate(predictOld.atc, predict.atc, fraction),
        interpolate(predictOld.at8, predict.at8, fraction),
        interpolate(predictOld.at1c, predict.at1c, fraction),
        interpolate(predictOld.at18, predict.at18, fraction),
        interpolate(predictOld.at58, predict.at58, fraction)
    }};
}

void playerPose(int step)
{
    localSprite.index = 0;
    localSprite.extra = 1;
    localSprite.sectnum = step > 1 ? 1 : 0;
    localSprite.x = 1000 + step * 200;
    localSprite.y = 2000 + step * 100;
    localSprite.z = 3000 + step * 80;
    gMe->pSprite = &localSprite;
    gMe->pXSprite = &localXSprite;
    gMe->q16ang = fix16_from_int(step ? 8 : 2040);
    gMe->q16look = fix16_from_int(step ? 3 : -2);
    gMe->q16horiz = fix16_from_int(90 + step * 10);
    gMe->q16slopehoriz = fix16_from_int(step * 4);
    gMe->slope = step * 15;
    gMe->zView = 1500 + step * 100;
    gMe->zViewVel = step * 30;
    gMe->zWeapon = 3500 + step * 200;
    gMe->zWeaponVel = step * 70;
    gMe->bobHeight = step * 60;
    gMe->bobWidth = step * 40;
    gMe->swayHeight = step * 50;
    gMe->swayWidth = step * 30;
    xvel[0] = step * 500;
    yvel[0] = step * 250;
    zvel[0] = step * 75;
    localXSprite.health = 1600;
    localXSprite.height = step * 10;
}

void zeroErrorAcknowledgement()
{
    playerPose(0);
    viewInitializePrediction();
    VIEW const previousCommand = predict;
    playerPose(1);
    // Establish the same real VIEW pair as a completed local command. Movement
    // itself is outside this test: keep its known prior endpoint explicitly so
    // the regression does not depend on the new reconciliation policy in setup.
    viewInitializePrediction();
    predictOld = previousCommand;
    check(!std::memcmp(&predictOld, &previousCommand, sizeof(VIEW)), "a fresh command interval keeps its previous camera endpoint");
    VIEW const commandEnd = predict;
    int const fraction = 65536 * 3 / 10;
    auto const halfway = camera(fraction);
    check(halfway[0] > previousCommand.at50 && halfway[0] < commandEnd.at50,
          "camera at fraction 0.3 is between local command endpoints");
    check(halfway[2] > previousCommand.at38 && halfway[2] < commandEnd.at38,
          "eye height is still interpolating when the snapshot ACK arrives");

    // Install a snapshot exactly matching the last local command, whose ACK
    // leaves no pending command to call viewUpdatePrediction again.
    viewInitializePrediction(false);
    check(!std::memcmp(&predictOld, &previousCommand, sizeof(VIEW)),
          "zero-error ACK preserves the actual previous VIEW when there is no replay");
    check(!std::memcmp(&predict, &commandEnd, sizeof(VIEW)), "zero-error ACK preserves the current predicted endpoint");
    check(camera(fraction) == halfway,
          "zero-error halfway ACK cannot jump position, angle, horizon, weapon, bob or sway to the command endpoint");
    for (int sample : {65536 / 20, fraction, 65536 * 3 / 5, 65536 * 9 / 10})
    {
        auto const before = camera(sample);
        viewInitializePrediction(false);
        check(camera(sample) == before, "repeated ordinary ACKs leave every fractional camera sample unchanged");
    }
}

void mouseLookAndDiscontinuity()
{
    vec2_t const pending = {fix16_from_int(7), fix16_from_int(2)};
    viewPredictionApplyPendingLook(pending);
    fix16_t const angle = gViewAngle, look = gViewLook;
    vec2_t const saved = viewPredictionPendingLook();
    viewInitializePrediction(false);
    viewPredictionApplyPendingLook(saved);
    check(gViewAngle == angle && gViewLook == look,
          "ordinary reconciliation can retain mouse look sampled since the last command");

    playerPose(3);
    viewInitializePrediction(true);
    check(!std::memcmp(&predictOld, &predict, sizeof(VIEW)), "teleport or first snapshot resets both real camera endpoints");
    check(camera(65536 * 3 / 10) == camera(65536), "discontinuity snaps the rendered camera at every interpolation fraction");

    playerPose(4);
    viewInitializePrediction(false);
    check(std::memcmp(&predictOld, &predict, sizeof(VIEW)) != 0, "ordinary movement creates distinct endpoints again after a discontinuity");
    viewPredictionSnap();
    check(!std::memcmp(&predictOld, &predict, sizeof(VIEW)), "explicit presentation snap still resets the real VIEW pair");
    check(camera(65536 * 3 / 10) == camera(65536), "explicit snap does not interpolate through an obsolete camera pose");
}
}

int main()
{
    zeroErrorAcknowledgement();
    mouseLookAndDiscontinuity();
    std::printf("camera prediction: %u checks passed (production VIEW initialization and interpolation)\n", checks);
}
