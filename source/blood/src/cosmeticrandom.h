// Private randomness for local presentation; never use for game simulation.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <stdint.h>

class CosmeticRandomGenerator
{
    uint32_t value;

public:
    explicit CosmeticRandomGenerator(uint32_t seed = 0x6d2b79f5u) : value(seed) {}
    uint32_t state() const { return value; }
    void seed(uint32_t seed) { value = seed; }

    uint32_t next()
    {
        // A Weyl sequence with a bijective 32-bit mixer. Zero is a valid seed;
        // all arithmetic wraps explicitly, including on old 32-bit targets.
        uint32_t result = (value += 0x9e3779b9u);
        result = (result ^ (result >> 16)) * 0x21f0aaadu;
        result = (result ^ (result >> 15)) * 0x735a2d97u;
        return result ^ (result >> 15);
    }

    uint32_t bounded(uint32_t upper)
    {
        if (upper <= 1) return 0;
        // Reject the short residue interval so every alternative has the same
        // probability, including ranges that are not powers of two.
        uint32_t const threshold = (0u - upper) % upper;
        uint32_t result;
        do result = next(); while (result < threshold);
        return result % upper;
    }
};

// This stream is independent of wrandomseed, randomseed and legacy qrand.
// Save-preview rendering preserves its state; prediction replay uses a separate
// generator seeded once per input command instead of consuming this stream.
CosmeticRandomGenerator &cosmeticRandomGenerator();
int CosmeticRandom(int upperExclusive);
int CosmeticRandom2(int magnitude); // [-magnitude, magnitude), or 0 if <= 0.
