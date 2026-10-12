// NBlood network diagnostics. Copyright (C) 2026 NBlood contributors.
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstdint>

extern int32_t gNetLagometer;

constexpr unsigned kNetLagSamples = 128;
struct NetLagSample
{
    int pingMs = 0; // -1 is a lost snapshot
    bool rateDelayed = false;
};

struct NetLagHistory
{
    int frameMs[kNetLagSamples] = {};
    NetLagSample snapshots[kNetLagSamples] = {};
    uint64_t frameCount = 0;
    uint64_t snapshotCount = 0;
    unsigned pendingCommands = 0;
    bool active = false;
    bool interrupted = false;
};

void netLagReset();
// Render time minus latest authoritative snapshot time: negative values are
// interpolation (blue); positive values are extrapolation/stale state (yellow).
void netLagFrame(int timingOffsetMs);
void netLagometerSnapshot(int pingMs, int dropped, bool rateDelayed = false);
void netLagometerCommandAck(unsigned pending);
void netLagometerDisconnect(bool interrupted);
void netLagDraw();
const NetLagHistory &netLagHistory();
