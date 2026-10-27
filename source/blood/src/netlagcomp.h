// Server-authoritative player collision history.
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include "build.h"
#include "netcommandtime.h"

// Samples are simulation boundaries, independent of snapshot transmission.
// 31 boundaries cover the entire preceding second at Blood's 30 Hz tick rate.
void netLagCompensationReset();
void netLagCompensationCapture(uint32_t tick);
void netLagCompensationInvalidatePlayer(int player);

// Use only around read-only collision/aim queries. Restoration must finish
// before damage, triggers, spawning effects, or other gameplay callbacks run.
class NetLagCompensationScope
{
public:
    explicit NetLagCompensationScope(spritetype *shooter);
    ~NetLagCompensationScope();
    NetLagCompensationScope(NetLagCompensationScope const &) = delete;
    NetLagCompensationScope &operator=(NetLagCompensationScope const &) = delete;

private:
    struct LinkChange { int16_t *address; int16_t value; };
    // Local poses must be untracked: SpriteTracker assignments are valid only
    // for the global sprite array in OpenGL/Polymer builds.
    uspritetype saved_[8];
    int indices_[8] = {};
    unsigned count_ = 0;
    // Removing and appending a sprite changes at most six list entries.
    LinkChange links_[8 * 8];
    unsigned linkCount_ = 0;
    bool active_ = false;

    void setLink(int16_t &link, int16_t value);
    void moveSector(int index, int sector);
};
