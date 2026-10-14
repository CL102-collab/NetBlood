// Private randomness for local presentation; never use for game simulation.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "cosmeticrandom.h"

#include <chrono>

CosmeticRandomGenerator &cosmeticRandomGenerator()
{
    // Vary local presentation between launches without consulting or advancing
    // any simulation RNG. This is not a security or protocol random source.
    static CosmeticRandomGenerator generator([]() {
        uint64_t const ticks = uint64_t(std::chrono::high_resolution_clock::now().time_since_epoch().count());
        return uint32_t(ticks) ^ uint32_t(ticks >> 32);
    }());
    return generator;
}

int CosmeticRandom(int upperExclusive)
{
    if (upperExclusive <= 1) return 0;
    return int(cosmeticRandomGenerator().bounded(uint32_t(upperExclusive)));
}

int CosmeticRandom2(int magnitude)
{
    if (magnitude <= 0) return 0;
    return int(int64_t(cosmeticRandomGenerator().bounded(uint32_t(magnitude) * 2u)) - magnitude);
}
