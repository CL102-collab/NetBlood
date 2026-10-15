// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "player.h"

namespace rnet
{
// Predict camera movement only. After apply() resets to authoritative movement,
// replay the unacknowledged input queue in sequence using this same operation.
// Weapons, inventory, actors, RNG and world simulation remain server-owned.
void predict(const input_t& input);
}
