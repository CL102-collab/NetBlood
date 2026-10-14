// Production QAV playback with an inert audio sink; no game assets required.
// SPDX-License-Identifier: GPL-2.0-only
#include "build.h"
#include "qav.h"
#include "cosmeticrandom.h"

#include <cstdio>
#include <cstdlib>
#include <set>
#include <vector>

spritetype sprite[MAXSPRITES];
uint32_t wrandomseed = 1;
int32_t randomseed = 1;
bool bVanilla = false;

namespace {
unsigned tests = 0, callbacks = 0;
int lastCallback = 0;
void* lastCallbackData = nullptr;
std::vector<int> sounds;

void check(bool condition, const char* expression, int line)
{
    ++tests;
    if (!condition) {
        std::fprintf(stderr, "QAV cosmetic test failed at %d: %s\n", line, expression);
        std::exit(1);
    }
}
#define CHECK(value) check(bool(value), #value, __LINE__)

void callback(int trigger, void* data)
{
    ++callbacks;
    lastCallback = trigger;
    lastCallbackData = data;
}

QAV animation()
{
    QAV qav{};
    qav.nFrames = 1;
    qav.ticksPerFrame = 4;
    qav.at10 = 4;
    qav.nSprite = -1;
    qav.frames[0].sound.sound = 100;
    qav.frames[0].sound.sndRange = 5;
    qav.frames[0].nCallbackId = 7;
    return qav;
}

void reset()
{
    sounds.clear();
    callbacks = 0;
    lastCallback = 0;
    lastCallbackData = nullptr;
    bVanilla = false;
    wrandomseed = 0x12345678u;
    randomseed = 0x12345678;
    cosmeticRandomGenerator().seed(0x12345678u);
}

void menuSounds()
{
    for (unsigned range : {1u, 2u, 5u, 255u}) {
        reset();
        QAV qav = animation();
        qav.frames[0].sound.sndRange = range;
        unsigned const variants = range == 1 ? 2 : range;
        std::set<int> seen;
        for (int i = 0; i < 256; ++i) {
            qav.Play(-1, 0, -1, nullptr);
            CHECK(sounds.size() == size_t(i + 1));
            CHECK(sounds.back() >= 100 && unsigned(sounds.back() - 100) < variants);
            seen.insert(sounds.back());
        }
        CHECK(seen.size() > 1);
        CHECK(wrandomseed == 0x12345678u);
        CHECK(randomseed == 0x12345678);
        CHECK(cosmeticRandomGenerator().state() != 0x12345678u);
        CHECK(callbacks == 0);
    }
}

void gameplaySounds(int registeredCallback)
{
    int playerMarker = 123;
    for (int form = 0; form < 3; ++form) {
        reset();
        QAV qav = animation();
        // Active weapon, idle weapon, and callback without player data must
        // all retain the simulation draw. Only menu/choke has neither.
        int const callbackIndex = form == 1 ? -1 : registeredCallback;
        void* const data = form == 2 ? nullptr : &playerMarker;
        for (int i = 0; i < 64; ++i) {
            uint32_t const before = wrandomseed;
            int const expectedSound = 100 + Random(5);
            uint32_t const expectedSeed = wrandomseed;
            wrandomseed = before;
            qav.Play(-1, 0, callbackIndex, data);
            CHECK(sounds.back() == expectedSound);
            CHECK(wrandomseed == expectedSeed);
            CHECK(cosmeticRandomGenerator().state() == 0x12345678u);
        }
        CHECK(callbacks == (form == 1 ? 0u : 64u));
        if (form != 1) {
            CHECK(lastCallback == 7);
            CHECK(lastCallbackData == data);
        }
    }
}

void unvariedSounds(int registeredCallback)
{
    int playerMarker = 123;
    for (bool gameplay : {false, true}) {
        for (int reason = 0; reason < 3; ++reason) {
            reset();
            QAV qav = animation();
            if (reason == 0) bVanilla = true;
            if (reason == 1) qav.frames[0].sound.sndRange = 0;
            if (reason == 2) qav.frames[0].sound.sound = 0;
            qav.Play(-1, 0, gameplay ? registeredCallback : -1, gameplay ? &playerMarker : nullptr);
            CHECK(wrandomseed == 0x12345678u);
            CHECK(cosmeticRandomGenerator().state() == 0x12345678u);
            CHECK(sounds.size() == (reason == 2 ? 0u : 1u));
            if (reason != 2) CHECK(sounds[0] == 100);
            CHECK(callbacks == (gameplay ? 1u : 0u));
        }
    }
}
}

// The production Play implementation and actual RNGs are linked below. These
// replace only audio output; rendering and resource loading are section-GCed.
void QAV::PlaySound(int sound) { sounds.push_back(sound); }
void QAV::PlaySound3D(spritetype*, int sound, int, int) { sounds.push_back(sound); }
void sfxKill3DSound(spritetype*, int, int) {}
void sndKillAllSounds() {}
void __dassert(const char* expression, const char*, int line) { check(false, expression, line); }

int main()
{
    int const registeredCallback = qavRegisterClient(callback);
    menuSounds();
    gameplaySounds(registeredCallback);
    unvariedSounds(registeredCallback);
    std::printf("QAV cosmetic RNG isolation: %u checks passed\n", tests);
}
