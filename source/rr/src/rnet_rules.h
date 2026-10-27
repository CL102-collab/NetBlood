// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rnet { namespace rules {
void startup();
void shutdown();
void applySettings();
void useMenuSettings();
void mapChanging(bool restart);
void mapStart();
void playerJoined(int player);
void playerLeft(int player);
void tick();
void draw();
void registerCommands();
bool parseArgument(int argc, const char* const* argv, int& index);
bool message(int player, uint8_t type, const uint8_t* data, size_t size);
// State body for a packet-demo bootstrap; does not send a network message.
std::vector<uint8_t> demoState();
bool coop();
bool teams();
bool frozen();
bool intermission();
unsigned remainingSeconds();
int lives(int player); // -1 means unlimited.
int32_t score(int player); // Server score; client scoreboards receive roster scores.
bool downed(int player);
bool spectating(int player);
unsigned giveUpProgress(int player); // percent
bool death(int player); // true: handled by the co-op life rules.
bool allowRespawn(int player);
void respawned(int player);
bool damageAllowed(int victim, int attacker);
bool canHolster();
bool canChase();
bool keepKeys();
bool respawnSprite(int spriteNum);
bool respawnMarker(int spriteNum);
void status();
void beginIntermission(bool restart = false);
void levelEnded();
void nextMap();
void shuffleRotation();
const std::vector<std::string>& rotation();
bool validMap(const std::string& map);
void cutsceneBegin();
void cutsceneDone();
bool cutsceneWaiting();
bool cinematicPlaying();
// Implemented beside the legacy cinematic code, called only from graphical clients.
void playCutscene();
} }
