// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace rnet
{
// Canonical presentation state. Never a savegame, native structure dump, or CON
// program. A client must load matching content before applying a snapshot.
bool capture(std::vector<uint8_t>& bytes, std::string& error);
bool validate(const std::vector<uint8_t>& bytes, std::string& error);
bool apply(const std::vector<uint8_t>& bytes, int localSlot, std::string& error);
void resetPrediction();
}
