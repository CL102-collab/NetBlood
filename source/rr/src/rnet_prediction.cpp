// SPDX-License-Identifier: GPL-2.0-or-later
#include "duke3d.h"
#include "rnet.h"
#include "rnet_prediction.h"
#include "rnet_snapshot.h"
#include "rnet_rules.h"

namespace rnet
{
void predict(const input_t& input)
{
    if (!client() || playback() || !inGame() || rules::frozen() ||
        myconnectindex < 0 || myconnectindex >= MAXPLAYERS || !g_player[myconnectindex].ps)
        return;
    if (rules::downed(myconnectindex) || rules::spectating(myconnectindex)) return;
    auto const& p = *g_player[myconnectindex].ps;
    if (p.i < 0 || p.i >= MAXSPRITES || sprite[p.i].statnum >= MAXSTATUS ||
        p.cursectnum < 0 || p.cursectnum >= numsectors || p.newowner >= 0)
        return;
    // The legacy predictor reads its sector before testing it, so repair an
    // unsuccessful preceding collision query before entering it again.
    if (mycursectnum < 0 || mycursectnum >= numsectors) resetPrediction();
    if (mycursectnum < 0 || mycursectnum >= numsectors) return;

    int const oldIndex = predictfifoplc;
    int const ring = oldIndex & (MOVEFIFOSIZ-1);
    input_t const saved = inputfifo[ring][myconnectindex];
    inputfifo[ring][myconnectindex] = input;
    // Net_DoPrediction changes only my* movement/view fields and its history;
    // its temporary sprite collision-mask change is restored before return.
    // In particular this does not execute P_ProcessInput, CON, or G_MoveWorld.
    Net_DoPrediction();
    inputfifo[ring][myconnectindex] = saved;
    predictfifoplc = oldIndex;
    ototalclock = totalclock;
}
} // namespace rnet
