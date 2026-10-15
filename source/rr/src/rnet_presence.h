// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace rnet { namespace presence {
// Returns true when the invocation is complete (registration or an error).
// A valid external launch consumes its arguments by setting argc to one.
bool earlyArguments(int& argc, const char* const* argv, int& exitCode);
bool parseArgument(int argc, const char* const* argv, int& index);
bool externalLaunch();
void registerCommands();
void startup();
void pump();
// Called only at the outer game-loop boundary (never during loading/cinematics).
void applyPendingJoin();
void shutdown();
// Must run before destroying the game ENet host.
void detachHost();
void mapChanged();
void connected(int player);
bool handle(int player, uint8_t type, const uint8_t* data, size_t size);
std::string joinUri();
} }
