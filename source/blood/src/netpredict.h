// Authoritative-network presentation and local movement prediction.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <stdint.h>
#include "build.h"
#include "controls.h"

struct NetPredictionStats
{
    uint64_t commands = 0, replayedCommands = 0, reconciliations = 0;
    uint64_t historyOverflows = 0, discontinuities = 0;
    uint64_t predictedSounds = 0, suppressedSounds = 0;
    uint32_t pendingCommands = 0, acknowledgedCommand = 0;
    int32_t lastCorrection = 0, maxCorrection = 0;
};

// The transport calls Command only once for a new input sequence, never for a
// repeated UDP datagram. Snapshot hooks bracket a validated, complete install.
void netPredictReset();
void netPredictCommand(uint32_t sequence, GINPUT const &input);
void netPredictBeforeSnapshot();
void netPredictSnapshot(uint32_t acknowledgedCommand, uint32_t serverTick, bool discontinuity);
bool netPredictActive();
NetPredictionStats netPredictGetStats();

// Rendering only: never leave interpolated geometry in authoritative arrays.
int netPredictLocalFraction();
void netPredictBeginRender();
void netPredictEndRender();
void netPredictInterpolateSprite(int spriteIndex, tspritetype *renderSprite);

// Weapon presentation marks a sound before playing it. Authoritative sound
// events tagged with the generating command consume exactly one local match.
void netPredictLocalSound(int soundId, int variantCount = 1, int channel = -1);
bool netPredictSuppressSound(int spriteIndex, int soundId, uint32_t commandSequence, int channel = -1);
