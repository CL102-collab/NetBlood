// SPDX-License-Identifier: GPL-2.0-or-later
#include "rnet_rules_model.h"
#include <cassert>
#include <cstdio>
using namespace rnet::rulemodel;

int main()
{
    Score score;
    for (int kills=1;kills<=700;++kills)
    {
        updateScore(score,uint8_t(kills),0);
        assert(score.total==kills);
        updateScore(score,uint8_t(kills),0); // Multiple roster reads do not count twice.
        assert(score.total==kills);
    }
    for (int deaths=1;deaths<=1000;++deaths)
    {
        updateScore(score,uint8_t(700),uint8_t(deaths));
        assert(score.total==700-deaths);
    }
    // Simultaneous frags and penalties both wrap within one simulation tick.
    score={1000,254,253}; updateScore(score,2,1);
    assert(score.total==1000);
    score=Score(); updateScore(score,1,0);
    assert(score.total==1); // A new map or occupant starts a fresh match score.
    score={std::numeric_limits<int32_t>::max(),255,0}; updateScore(score,0,0);
    assert(score.total==std::numeric_limits<int32_t>::max());
    score={std::numeric_limits<int32_t>::min(),0,255}; updateScore(score,0,0);
    assert(score.total==std::numeric_limits<int32_t>::min());

    Life player;
    assert(player.remaining == 4 && canRespawn(player));
    assert(knockDown(player, 36));
    for (int i=0;i<150;++i) assert(!giveUp(player,true,90));
    assert(player.remaining == 4 && !canRespawn(player));
    assert(!giveUp(player,false,90));
    for (int i=0;i<89;++i) assert(!giveUp(player,true,90));
    assert(player.hold == 89);
    assert(!giveUp(player,false,90));
    assert(player.hold == 0 && player.remaining == 4);
    assert(revive(player));
    assert(player.remaining == 4 && player.state == Alive);

    for (int lives=4;lives>0;--lives)
    {
        assert(knockDown(player,36));
        assert(!knockDown(player,36));
        assert(!giveUp(player,false,90));
        for (int i=0;i<89;++i) assert(!giveUp(player,true,90));
        assert(giveUp(player,true,90));
        assert(player.remaining == lives-1);
        assert(!canRespawn(player)); // Must release before attempting to respawn.
        assert(!revive(player));
        player.useWasDown=false;
        if (lives>1)
        {
            assert(player.state == Dead && canRespawn(player));
            player.state=Alive;
        }
        else
            assert(player.state == Out && !canRespawn(player));
    }
    assert(!knockDown(player,36));
    assert(!giveUp(player,true,90));
    assert(player.remaining == 0);
    std::puts("Rednukem co-op life and wide score rules passed");
}
