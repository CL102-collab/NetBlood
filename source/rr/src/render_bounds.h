// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

namespace renderbounds {

template <typename T, std::size_t N>
inline bool frameIndex(T const (&)[N], int64_t index)
{
    return index >= 0 && uint64_t(index) < N;
}

// Return -1 rather than narrowing an invalid action frame into a different tile.
// The table access and missing-art fallback both stay within the tile array.
template <typename TileSize, std::size_t N>
inline int actionTile(int base, int angleFrame, int64_t startFrame, int views, int frame,
                      TileSize const (&sizes)[N])
{
    if (views < 0) return -1;
    int64_t const offset = int64_t(base) + angleFrame + int64_t(views) * frame;
    // Check before adding a script-sized value, which can itself be int64_t.
    if (startFrame < -offset || startFrame >= int64_t(N) - offset) return -1;
    int64_t tile = offset + startFrame;
    if (views > 0)
        while (tile > 0 && sizes[tile].x == 0)
        {
            if (tile < views) return -1;
            tile -= views;
        }
    return int(tile);
}

} // namespace renderbounds
