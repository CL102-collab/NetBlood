// SPDX-License-Identifier: GPL-2.0-or-later
// Exercise the production team transfer without assets or a running server.
#include "build.h"
#include "blood.h"
#include "dude.h"
#include "player.h"
#include "sound.h"
#include "view.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

GAMEOPTIONS gGameOptions = {};
static bool live = true, server = true;
bool netIsLive() { return live; }
bool netIsServer() { return server; }
int seqRegisterClient(void (*)(int, int)) { return 0; }
namespace loguru {
Verbosity current_verbosity_cutoff() { return Verbosity_OFF; }
void log(Verbosity, const char*, unsigned, const char*, ...) {}
}

namespace {
unsigned assertions = 0;
spritetype body = {}, drops[2] = {};
XSPRITE extended = {};
DUDEINFO dude = {};
int currentSequence = -1, sequenceStatus = -1;
int dropCount = 0, failedDrop = 0, notices = 0, sounds = 0;
int types[2] = {}, oldTeams[2] = {};
void check(bool condition, const char* expression, int line)
{
    ++assertions;
    if (!condition) { std::fprintf(stderr, "FAIL line %d: %s\n", line, expression); std::exit(1); }
}
#define CHECK(value) check(bool(value), #value, __LINE__)

PLAYER& reset()
{
    live = server = true;
    gGameOptions = {};
    gGameOptions.nGameType = kGameTypeTeams;
    PLAYER& player = gPlayer[3];
    player = {};
    body = {}; extended = {}; dude = {};
    player.nPlayer = 3; player.pSprite = &body; player.pXSprite = &extended;
    player.pDudeInfo = &dude; dude.seqStartID = 1024;
    player.teamId = 0; body.pal = 11; body.cstat = 257;
    player.fragCount = 17; player.fragInfo[0] = 9;
    player.curWeapon = kWeaponShotgun; player.ammoCount[2] = 42;
    player.armor[0] = 300; player.hasKey[2] = 1;
    player.pwUpTime[kPwUpShadowCloak] = 99;
    player.pwUpTime[kPwUpDoppleganger] = 123;
    player.used2[0] = 21; player.used2[1] = 32;
    extended.health = 0;
    currentSequence = sequenceStatus = -1;
    std::strcpy(gProfile[3].name, "newest");
    for (int i = 0; i < kMaxPlayers; ++i) {
        gPlayerScores[i] = 10 + i;
        gPlayerScoreTicks[i] = 100 + i;
    }
    dropCount = failedDrop = notices = sounds = 0;
    gBlueFlagDropped = gRedFlagDropped = false;
    return player;
}

void preservesState()
{
    // Transfer after either a running final-death animation or its completion.
    for (int deathSequence : {1, 2, 3, 17, 18, -1}) {
        PLAYER& player = reset();
        if (deathSequence >= 0) {
            currentSequence = dude.seqStartID + deathSequence;
            sequenceStatus = 0;
        }
        const XSPRITE beforeExtended = extended;
        PLAYER expected = player; expected.teamId = 1;
        spritetype expectedBody = body; expectedBody.pal = 12;
        CHECK(playerChangeTeam(&player, 1));
        CHECK(std::memcmp(&player, &expected, sizeof(player)) == 0);
        CHECK(std::memcmp(&body, &expectedBody, sizeof(body)) == 0);
        CHECK(std::memcmp(&extended, &beforeExtended, sizeof(extended)) == 0);
        CHECK(dropCount == 0 && notices == 0 && sounds == 0);
        for (int i = 0; i < kMaxPlayers; ++i) {
            CHECK(gPlayerScores[i] == 10 + i);
            CHECK(int(gPlayerScoreTicks[i]) == 100 + i);
        }
        CHECK(playerChangeTeam(&player, 0));
        CHECK(player.teamId == 0 && body.pal == 11);
    }
}

void waitsForDeath()
{
    for (int health : {1500, 16, 1}) {
        PLAYER& player = reset(); extended.health = health; player.hasFlag = 3;
        const PLAYER before = player;
        CHECK(!playerChangeTeam(&player, 1));
        CHECK(std::memcmp(&player, &before, sizeof(player)) == 0);
        CHECK(extended.health == health && body.pal == 11);
        CHECK(dropCount == 0 && notices == 0 && sounds == 0);
    }
    for (int revivableSequence : {16, 14}) {
        PLAYER& player = reset(); player.hasFlag = 3;
        currentSequence = dude.seqStartID + revivableSequence;
        sequenceStatus = 0;
        const PLAYER before = player;
        CHECK(!playerChangeTeam(&player, 1));
        CHECK(std::memcmp(&player, &before, sizeof(player)) == 0);
        CHECK(dropCount == 0 && notices == 0 && sounds == 0 && body.pal == 11);
        // Finishing the kneeling state with a lethal hit completes the death.
        currentSequence = dude.seqStartID + 18;
        CHECK(playerChangeTeam(&player, 1));
        CHECK(player.teamId == 1 && player.hasFlag == 0 && body.pal == 12);
    }
}

void dropsFlags()
{
    for (int mask = 1; mask <= 3; ++mask) {
        PLAYER& player = reset(); player.hasFlag = mask;
        PLAYER expected = player; expected.teamId = 1; expected.hasFlag = 0;
        if (mask & 1) expected.used2[0] = -1;
        if (mask & 2) expected.used2[1] = -1;
        CHECK(playerChangeTeam(&player, 1));
        CHECK(std::memcmp(&player, &expected, sizeof(player)) == 0);
        const int expectedDrops = mask == 3 ? 2 : 1;
        CHECK(dropCount == expectedDrops && notices == expectedDrops && sounds == expectedDrops);
        CHECK(gBlueFlagDropped == bool(mask & 1));
        CHECK(gRedFlagDropped == bool(mask & 2));
        for (int i = 0; i < expectedDrops; ++i) {
            const bool blue = mask == 1 || (mask == 3 && i == 0);
            CHECK(types[i] == (blue ? kItemFlagA : kItemFlagB));
            CHECK(drops[i].owner == (blue ? 21 : 32));
            CHECK(oldTeams[i] == 0); // Drop while still on the original team.
        }
    }
    PLAYER& player = reset(); player.hasFlag = 3; failedDrop = 1;
    const PLAYER before = player;
    CHECK(!playerChangeTeam(&player, 1));
    CHECK(std::memcmp(&player, &before, sizeof(player)) == 0);
    CHECK(body.pal == 11 && notices == 0 && sounds == 0);
    CHECK(!gBlueFlagDropped && !gRedFlagDropped);

    reset(); player.hasFlag = 3; failedDrop = 2;
    CHECK(!playerChangeTeam(&player, 1));
    CHECK(player.teamId == 0 && body.pal == 11 && player.hasFlag == 2);
    CHECK(player.used2[0] == -1 && player.used2[1] == 32);
    CHECK(gBlueFlagDropped && !gRedFlagDropped && notices == 1);
    dropCount = 0; failedDrop = 0;
    CHECK(playerChangeTeam(&player, 1));
    CHECK(player.teamId == 1 && player.hasFlag == 0 && gRedFlagDropped);
}

void guards()
{
    for (int guard = 0; guard < 8; ++guard) {
        PLAYER& player = reset(); player.hasFlag = 3;
        int target = 1;
        if (guard == 0) live = false;
        if (guard == 1) server = false;
        if (guard == 2) gGameOptions.nGameType = kGameTypeBloodBath;
        if (guard == 3) gGameOptions.nGameType = kGameTypeCoop;
        if (guard == 4) target = 0;
        if (guard == 5) target = -1;
        if (guard == 6) target = 2;
        if (guard == 7) player.nPlayer = kMaxPlayers;
        const PLAYER before = player;
        CHECK(!playerChangeTeam(&player, target));
        CHECK(std::memcmp(&player, &before, sizeof(player)) == 0);
        CHECK(dropCount == 0 && body.pal == 11);
    }
    reset(); CHECK(!playerChangeTeam(nullptr, 1));
    for (int pointer = 0; pointer < 3; ++pointer) {
        PLAYER& unspawned = reset();
        if (pointer == 0) unspawned.pSprite = nullptr;
        if (pointer == 1) unspawned.pXSprite = nullptr;
        if (pointer == 2) unspawned.pDudeInfo = nullptr;
        const PLAYER before = unspawned;
        CHECK(!playerChangeTeam(&unspawned, 1));
        CHECK(std::memcmp(&unspawned, &before, sizeof(unspawned)) == 0);
        CHECK(dropCount == 0 && notices == 0 && sounds == 0);
    }
}
}

spritetype *actDropObject(spritetype* source, int type)
{
    CHECK(source == &body);
    CHECK(dropCount < 2);
    const int index = dropCount++;
    types[index] = type; oldTeams[index] = gPlayer[3].teamId;
    if (failedDrop == dropCount) return nullptr;
    drops[index] = {};
    return &drops[index];
}
void sndStartSample(unsigned int, int, int, bool) { ++sounds; }
void viewSetMessage(const char*, int, MESSAGE_PRIORITY) { ++notices; }
int seqGetID(int, int) { return currentSequence; }
int seqGetStatus(int, int) { return sequenceStatus; }

int main()
{
    preservesState(); waitsForDeath(); dropsFlags(); guards();
    std::printf("player team transfer: %u assertions passed\n", assertions);
}
