// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <utility>

namespace rnet { namespace social {
std::vector<std::pair<uint8_t, std::vector<uint8_t>>> demoState();
void init();
void tick();
void draw(bool scoreboard = false);
void join(int player);
void ready(int player);
void leave(int player);
bool handle(int sender, uint8_t type, const std::vector<uint8_t>& payload);
void registerCommands();
std::string normalizeName(int player, const std::string& requested);
void profile();
void sendChat(const char* text, bool team = false);
void chatInput();
void input(); // F1/F2 ballots, before the legacy local-key handlers.
void requestVote(const std::string& action, const std::string& map = {});
void castBallot(bool yes);
void snapshot(uint32_t sequence, unsigned pending, bool rateDelayed = false);
void frame(int timingOffsetMs);
void shuffle();
} }
