// Real FinishHim callback and event delivery, with an inert audio sink.
// SPDX-License-Identifier: GPL-2.0-only
#include "build.h"
#include "common_game.h"
#include "mmulti.h"
#include "db.h"
#include "player.h"
#include "netevents.h"
#include "messages.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

void FinishHim(int nSprite);

spritetype sprite[MAXSPRITES];
XSPRITE xsprite[kMaxXSprites];
PLAYER gPlayer[kMaxPlayers];
PLAYER *gMe = nullptr, *gView = nullptr;
int myconnectindex = 0;
int connecthead = 0, connectpoint2[MAXMULTIPLAYERS];

namespace {
constexpr int kVictim = 2, kAttacker = 5;
constexpr int kVictimSprite = 41, kAttackerSprite = 88, kHostSprite = 10;
bool serverTick = false, kneeling = true;
unsigned tests = 0, sequenceChecks = 0;
struct Sample { unsigned id; int volume, channel; bool loop; };
std::vector<Sample> samples;
std::vector<std::vector<uint8_t>> sent;

void check(bool condition, const char* expression, int line)
{
    ++tests;
    if (!condition) {
        std::fprintf(stderr, "FinishHim test failed at %d: %s\n", line, expression);
        std::exit(1);
    }
}
#define CHECK(value) check(bool(value), #value, __LINE__)

void reset()
{
    netEventsReset();
    samples.clear();
    sent.clear();
    sequenceChecks = 0;
    serverTick = false;
    kneeling = true;
    myconnectindex = 0;
    gMe = gView = &gPlayer[0];
    for (auto& player : gPlayer) player.nSprite = -1;
    for (auto& next : connectpoint2) next = -1;
    connecthead = 0;
    connectpoint2[0] = kVictim;
    connectpoint2[kVictim] = kAttacker;
    gPlayer[0].nSprite = kHostSprite;
    gPlayer[kVictim].nSprite = kVictimSprite;
    gPlayer[kAttacker].nSprite = kAttackerSprite;
    sprite[kVictimSprite] = {};
    sprite[kVictimSprite].type = kDudePlayer1 + kVictim;
    sprite[kVictimSprite].extra = 1;
    xsprite[1] = {};
    xsprite[1].target = kAttackerSprite;
}

void checkSample()
{
    CHECK(samples.size() == 1);
    CHECK(samples[0].id == 3313);
    CHECK(samples[0].volume == -1);
    CHECK(samples[0].channel == 1);
    CHECK(!samples[0].loop);
}

void deliverTo(int player, bool expected)
{
    CHECK(sent.size() == 1);
    auto const event = sent[0];
    serverTick = false;
    myconnectindex = player;
    gMe = gView = &gPlayer[player];
    samples.clear();
    CHECK(netPlayEvent(event.data(), event.size()));
    CHECK(samples.size() == (expected ? 1u : 0u));
    if (expected) checkSample();
    CHECK(sent.size() == 1); // Playback must never rebroadcast the cue.
}

void remoteAttacker(bool dedicated)
{
    reset();
    serverTick = true;
    if (dedicated) {
        myconnectindex = -1;
        gMe = gView = nullptr;
    }
    FinishHim(kVictimSprite);
    CHECK(samples.empty()); // Dedicated mixer and listen host both stay silent.
    CHECK(sent.size() == 1);
    CHECK(sequenceChecks == 1);
    deliverTo(0, false);
    deliverTo(kVictim, false);
    deliverTo(kAttacker, true);
}

void localAttacker()
{
    reset();
    xsprite[1].target = kHostSprite;
    serverTick = true;
    FinishHim(kVictimSprite);
    checkSample();
    CHECK(sent.size() == 1); // Targeted event only; no extra global audio event.
    deliverTo(kVictim, false);
    deliverTo(kAttacker, false);

    reset();
    xsprite[1].target = kHostSprite;
    connectpoint2[0] = -1;
    FinishHim(kVictimSprite);
    checkSample();
    CHECK(sent.empty()); // Offline cue keeps its original local playback.
}

void silentCases()
{
    for (int reason = 0; reason < 4; ++reason) {
        reset();
        serverTick = true;
        gMe = gView = nullptr;
        if (reason == 0) kneeling = false;
        if (reason == 1) xsprite[1].target = 123;
        if (reason == 2) connectpoint2[kVictim] = -1; // Stale disconnected slot.
        if (reason == 3) sprite[kVictimSprite].type = kDudeCultistTommy;
        FinishHim(kVictimSprite);
        CHECK(samples.empty());
        CHECK(sent.empty());
        if (reason == 3) CHECK(sequenceChecks == 0);
    }

    reset();
    FinishHim(kVictimSprite);
    CHECK(samples.empty()); // Offline callback must not play another player's cue.
    CHECK(sent.empty());
}

void invalidReferences()
{
    for (int victim : {-1, kMaxSprites}) {
        reset();
        serverTick = true;
        FinishHim(victim);
        CHECK(sequenceChecks == 0);
        CHECK(samples.empty() && sent.empty());
    }
    for (int extra : {-1, 0, kMaxXSprites}) {
        reset();
        serverTick = true;
        sprite[kVictimSprite].extra = extra;
        FinishHim(kVictimSprite);
        CHECK(sequenceChecks == 0);
        CHECK(samples.empty() && sent.empty());
    }
    for (int target : {-1, kMaxSprites}) {
        reset();
        serverTick = true;
        // Match an unused player's default -1 as well as an invalid index.
        xsprite[1].target = target;
        FinishHim(kVictimSprite);
        CHECK(samples.empty() && sent.empty());
    }
}
} // namespace

char playerSeqPlaying(PLAYER* player, int sequence)
{
    ++sequenceChecks;
    CHECK(player == &gPlayer[kVictim]);
    CHECK(sequence == 16);
    return kneeling;
}
bool netInServerTick() { return serverTick; }
uint32_t netSnapshotSpriteGeneration(int) { return 0; }
void netBroadcastEvent(const uint8_t* bytes, size_t size) { sent.emplace_back(bytes, bytes + size); }
bool netPredictSuppressSound(int, int, uint32_t, int) { return false; }

void sndStartSample(unsigned id, int volume, int channel, bool loop)
{
    // Match the production audio hook: an untargeted direct call on the server
    // broadcasts. The targeted helper must suppress that duplicate/global event.
    netSoundSample(NetSoundKind::Sample, int(id), nullptr, volume, channel, loop);
    samples.push_back({id, volume, channel, loop});
}
void sfxPlay3DSound(int, int, int, int, int) { CHECK(false); }
void sfxPlay3DSound(spritetype*, int, int, int) { CHECK(false); }
void sfxPlay3DSoundCP(spritetype*, int, int, int, int, int) { CHECK(false); }
void sfxKill3DSound(spritetype*, int, int) { CHECK(false); }
void sfxKillSpriteSounds(spritetype*) { CHECK(false); }
void sfxKillAllSounds() { CHECK(false); }
void sndStartSample(const char*, int, int) { CHECK(false); }
void sndStartWavID(unsigned, int, int) { CHECK(false); }
void sndKillAllSounds() { CHECK(false); }
void viewSetMessage(const char*, int, MESSAGE_PRIORITY) { CHECK(false); }

int main()
{
    remoteAttacker(true);
    remoteAttacker(false);
    localAttacker();
    silentCases();
    invalidReferences();
    std::printf("FinishHim private sound delivery: %u checks passed\n", tests);
}
