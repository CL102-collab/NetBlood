// Rednukem packet demos. Copyright (C) 2026 NBlood contributors.
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstdint>
#include <string>
#include <vector>

// A packet demo is a stream of accepted server messages, not UDP datagrams.
// The transport supplies Welcome followed by self-contained full snapshots.
// Recording duplicates, fragments, unvalidated packets, or client commands here
// would make playback depend on the network conditions of the original session.
struct NetDemoInfo
{
    uint32_t protocolVersion = 1;
    uint32_t mapCrc = 0;
    uint32_t contentCrc = 0;
    uint32_t tickRate = 30;
    int32_t localSlot = -1;
    std::string mapName;
};

constexpr uint32_t kNetDemoFormatVersion = 1;
constexpr uint32_t kNetDemoMaxPacketBytes = 16u * 1024u * 1024u + 4096u;

bool netDemoStartRecording(const char *path, const NetDemoInfo &info, std::string &error);
// resetClock marks a map-generation Welcome whose tick counter restarts. Its
// presentation time follows the preceding map without rewinding the demo clock.
bool netDemoRecordPacket(uint32_t serverTick, const std::vector<uint8_t> &packet, bool resetClock = false);
void netDemoStopRecording();
bool netDemoIsRecording();

// elapsedMs is a caller-owned monotonic playback clock, starting at zero.
// A false NextPacket result can mean "not due yet", clean EOF, or failure;
// IsPlaying and LastError distinguish these cases. Packets use the ordinary
// client message/snapshot apply path; playback must not simulate the world.
bool netDemoStartPlayback(const char *path, NetDemoInfo &info, std::string &error);
bool netDemoNextPacket(uint64_t elapsedMs, std::vector<uint8_t> &packet, uint32_t &serverTick);
void netDemoStopPlayback();
bool netDemoIsPlaying();
const std::string &netDemoLastError();
