// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <vector>
namespace rnet { namespace audio {
constexpr uint8_t Event=12, Taunt=13, Baseline=14;
// An outer sound API call can delegate to other sound APIs. Only the outer
// call publishes an event; device-side replacement/cleanup is local.
struct Scope { Scope(); ~Scope(); };
void reset();
void beginTick();
void endTick();
void ready(int player);
void deleted(int emitter);
std::vector<uint8_t> demoState();
void snapshot();
bool handle(int sender,uint8_t type,const std::vector<uint8_t>& body);
void registerCommands();
int start(int number,int emitter); // -2: local only; -1: rejected; 1: logical voice
void stop(int number,int emitter);
void stopAll();
void pitch(int number,int emitter,int value);
bool authoritative();
bool playing(int number,int emitter); // -1 means any sound / any emitter
void taunt(int number);
} }
