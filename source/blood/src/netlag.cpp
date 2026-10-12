// NBlood network diagnostics. Copyright (C) 2026 NBlood contributors.
// SPDX-License-Identifier: GPL-2.0-only
// Graph semantics follow Quake III Arena's CG_DrawLagometer (id Software,
// GPL-2.0): https://github.com/id-Software/Quake-III-Arena/blob/master/code/cgame/cg_draw.c
// This implementation uses Build's common HUD primitives for every renderer.
#include "netlag.h"

#include <algorithm>
#include <cstdio>

#ifndef NETLAG_NO_DRAW
#include "build.h"
#include "timer.h"
#endif

int32_t gNetLagometer = 1;

namespace
{
NetLagHistory gHistory;
constexpr unsigned kMask = kNetLagSamples - 1;

void AddSnapshot(int ping, bool delayed)
{
    auto &sample = gHistory.snapshots[gHistory.snapshotCount++ & kMask];
    sample.pingMs = ping;
    sample.rateDelayed = delayed;
}

#ifndef NETLAG_NO_DRAW
palette_t Color(int red, int green, int blue)
{
    // drawlinergb uses RGB on hardware renderers and p.f on the palette renderer.
    int best = 0, distance = 0x7fffffff;
    for (int index = 0; index < 256; ++index)
    {
        int const r = int(uint8_t(curpalette[index].r)) - red;
        int const g = int(uint8_t(curpalette[index].g)) - green;
        int const b = int(uint8_t(curpalette[index].b)) - blue;
        int const candidate = r * r + g * g + b * b;
        if (candidate < distance) distance = candidate, best = index;
    }
    return {char(red), char(green), char(blue), char(best)};
}

void Line(int x, int y1, int y2, palette_t color)
{
    // Build coordinates are 20.12 fixed point. A nonzero segment also survives
    // the GL/Vulkan line rasterizer when a sample is less than one pixel high.
    if (y2 > y1) drawlinergb(x << 12, y1 << 12, x << 12, y2 << 12, color);
}
#endif
} // namespace

void netLagReset() { gHistory = NetLagHistory{}; }

void netLagFrame(int timingOffsetMs)
{
    gHistory.frameMs[gHistory.frameCount++ & kMask] = std::max(-60000, std::min(60000, timingOffsetMs));
}

void netLagometerSnapshot(int pingMs, int dropped, bool rateDelayed)
{
    // A hostile or stale sequence gap cannot turn a diagnostics update into an
    // unbounded loop. Only the last history window can be drawn in any case.
    int const visibleDrops = std::max(0, std::min(int(kNetLagSamples), dropped));
    for (int i = 0; i < visibleDrops; ++i) AddSnapshot(-1, false);
    AddSnapshot(std::max(0, std::min(60000, pingMs)), rateDelayed);
    gHistory.active = true;
}

void netLagometerCommandAck(unsigned pending) { gHistory.pendingCommands = pending; }
void netLagometerDisconnect(bool interrupted) { gHistory.interrupted = interrupted; }
const NetLagHistory &netLagHistory() { return gHistory; }

void netLagDraw()
{
#ifndef NETLAG_NO_DRAW
    if (!gHistory.active || xdim < 160 || ydim < 100) return;
    if (gNetLagometer)
    {
        int const scale = std::max(1, ydim / 400);
        int const width = 64 * scale;
        int const height = 48 * scale;
        int const x = std::max(windowxy1.x + 2, windowxy2.x - width - 8 * scale);
        int const y = std::max(windowxy1.y + 10, windowxy2.y - height - 12 * scale);
        int const mid = y + height / 4;
        int const frameRange = height / 4;
        int const pingRange = height / 2;
        palette_t const background = Color(16, 16, 16);
        palette_t const blue = Color(64, 128, 255);
        palette_t const yellow = Color(255, 224, 32);
        palette_t const green = Color(32, 224, 64);
        palette_t const red = Color(255, 32, 32);
        uint64_t const frameCount = std::min<uint64_t>(gHistory.frameCount, kNetLagSamples);
        uint64_t const snapshotCount = std::min<uint64_t>(gHistory.snapshotCount, kNetLagSamples);
        for (int column = 0; column < width; ++column)
        {
            int const at = x + width - column - 1;
            unsigned const age = unsigned(column / scale);
            Line(at, y, y + height, background);
            if (age < frameCount)
            {
                int const sample = gHistory.frameMs[(gHistory.frameCount - 1 - age) & kMask];
                if (sample)
                {
                    int const magnitude = sample < 0 ? -sample : sample;
                    int const pixels = std::max(1, std::min(frameRange, magnitude * frameRange / 300));
                    if (sample > 0) Line(at, mid - pixels, mid, yellow);
                    else Line(at, mid, mid + pixels, blue);
                }
            }
            if (age < snapshotCount)
            {
                auto const &sample = gHistory.snapshots[(gHistory.snapshotCount - 1 - age) & kMask];
                int const pixels = sample.pingMs < 0 ? pingRange
                    : std::max(1, std::min(pingRange, sample.pingMs * pingRange / 900));
                Line(at, y + height - pixels, y + height,
                    sample.pingMs < 0 ? red : sample.rateDelayed ? yellow : green);
            }
        }
        int lost = 0, received = 0, ping = 0;
        for (uint64_t age = 0; age < snapshotCount; ++age)
        {
            auto const &sample = gHistory.snapshots[(gHistory.snapshotCount - 1 - age) & kMask];
            if (sample.pingMs < 0) ++lost;
            else if (!received++) ping = sample.pingMs;
        }
        char label[64];
        std::snprintf(label, sizeof(label), "%d ms %d%%", ping,
            snapshotCount ? lost * 100 / int(snapshotCount) : 0);
        printext256(x, y - 9, whitecol, blackcol, label, 1);
    }
    // Match Quake III's disconnect notice even when the graph is disabled.
    if (gHistory.interrupted && !(timerGetTicks() & 512))
    {
        const char *text = "CONNECTION INTERRUPTED";
        int const x = std::max(windowxy1.x, (xdim - 22 * 8) / 2);
        printext256(x, std::max(windowxy1.y + 8, ydim / 4), redcol, blackcol, text, 0);
    }
#endif
}
