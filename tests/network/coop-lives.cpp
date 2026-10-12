// SPDX-License-Identifier: GPL-2.0-or-later
// Production life accounting independent of engine state and transport.
#include "netcoop.h"
#include <cstdio>
#include <cstdlib>

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)

static void fourLives()
{
    netcoop::LifeState player;
    player.reset(4);
    CHECK(player.remaining() == 4 && !player.exhausted());
    unsigned respawns = 0;
    for (int death = 1; death <= 4; ++death)
    {
        CHECK(player.die());
        CHECK(player.remaining() == 4 - death);
        for (unsigned repeat = 0; repeat < 10; ++repeat) CHECK(!player.die());
        CHECK(player.remaining() == 4 - death);
        if (player.respawn()) ++respawns;
        CHECK(player.exhausted() == (death == 4));
    }
    CHECK(respawns == 3);
    CHECK(!player.respawn() && !player.die() && player.remaining() == 0);

    // A level reset replenishes lives and allows a fresh first death.
    player.reset(4);
    CHECK(player.remaining() == 4 && player.die() && player.remaining() == 3);
    player.reset(4);
    CHECK(player.die() && player.remaining() == 3);
}

static void independentPlayersAndReconnect()
{
    netcoop::LifeState first, second;
    first.reset(4); second.reset(4);
    CHECK(first.die() && first.remaining() == 3 && second.remaining() == 4);
    auto restored = first;
    CHECK(restored.remaining() == 3 && !restored.die());
    CHECK(restored.respawn() && restored.die() && restored.remaining() == 2);
    CHECK(first.remaining() == 3 && second.remaining() == 4);

    first.reset(1);
    CHECK(first.die() && first.exhausted());
    restored = first;
    CHECK(restored.exhausted() && !restored.respawn() && !restored.die());
    CHECK(!second.exhausted());
}

static void boundaries()
{
    netcoop::LifeState player;
    CHECK(player.remaining() == -1 && !player.exhausted());
    for (unsigned attempt = 0; attempt < 1024; ++attempt)
    {
        CHECK(player.die() && !player.die());
        CHECK(player.remaining() == -1 && !player.exhausted() && player.respawn());
    }
    player.reset(1);
    CHECK(player.remaining() == 1 && player.die() && player.remaining() == 0);
    CHECK(player.exhausted() && !player.die() && !player.respawn());
    player.reset(0);
    CHECK(player.remaining() == -1 && player.die() && player.respawn());
    player.reset(255);
    for (int remaining = 254; remaining >= 0; --remaining)
    {
        CHECK(player.die() && player.remaining() == remaining);
        CHECK(player.respawn() == (remaining != 0));
    }
    CHECK(player.exhausted() && !player.die());
}

int main()
{
    fourLives();
    independentPlayersAndReconnect();
    boundaries();
    std::printf("PASS: %u cooperative life checks (four deaths, three respawns, repeat guards, reset, unlimited and reconnect)\n", checks);
}
