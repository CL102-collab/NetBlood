// SPDX-License-Identifier: GPL-2.0-only
#include "render_bounds.h"
#include <climits>
#include <cstdio>
#include <cstdlib>

namespace {
unsigned checks;
void require(bool condition)
{
    ++checks;
    if (!condition) { std::fprintf(stderr, "render bounds check %u failed\n", checks); std::exit(1); }
}
struct TileSize { int16_t x, y; };
TileSize tiles[64];

int reference(int base, int angle, int64_t start, int views, int frame)
{
    if (views < 0) return -1;
    __int128 tile = __int128(base) + angle + start + __int128(views) * frame;
    if (tile < 0 || tile >= 64) return -1;
    if (views)
        while (tile > 0 && tiles[int(tile)].x == 0)
        {
            tile -= views;
            if (tile < 0) return -1;
        }
    return int(tile);
}
void action(int base, int angle, int64_t start, int views, int frame)
{
    require(renderbounds::actionTile(base, angle, start, views, frame, tiles) ==
            reference(base, angle, start, views, frame));
}

template <unsigned N> void frames()
{
    int table[N];
    for (unsigned i = 0; i < N; ++i) table[i] = int(i);
    // Full serialized kickback range, including transformed sine-table indexes.
    for (int frame = INT16_MIN; frame <= INT16_MAX; ++frame)
    {
        bool const valid = renderbounds::frameIndex(table, frame);
        require(valid == (frame >= 0 && frame < int(N)));
        if (valid) require(table[frame] == frame);
        int64_t const sineFrame = int64_t(frame) * 128;
        bool const sineValid = renderbounds::frameIndex(table, sineFrame);
        require(sineValid == (sineFrame >= 0 && sineFrame < N));
        if (sineValid) require(table[sineFrame] == sineFrame);
    }
    require(!renderbounds::frameIndex(table, INT64_MIN));
    require(!renderbounds::frameIndex(table, INT64_MAX));
}
uint32_t randomValue()
{
    static uint32_t state = 0xcff30b11;
    state ^= state << 13; state ^= state >> 17; state ^= state << 5;
    return state;
}
}
int main()
{
    for (unsigned i = 0; i < 64; ++i) tiles[i] = {int16_t(i % 3 ? 16 : 0), 16};
    require(renderbounds::actionTile(12, 1, 3, 5, 2, tiles) == 26);
    require(renderbounds::actionTile(3, 0, 0, 5, 0, tiles) == -1); // fallback would become -2
    require(renderbounds::actionTile(0, 0, 0, 5, 0, tiles) == 0); // missing tile zero is still bounded
    require(renderbounds::actionTile(32760, 0, 8, 0, 0, tiles) == -1); // do not narrow to int16_t
    int const extremes[] = {INT_MIN, INT_MIN + 1, -32768, -1, 0, 1, 32767, INT_MAX};
    for (int base : extremes) for (int angle : extremes) for (int views : extremes) for (int frame : extremes)
    {
        action(base, angle, INT64_MIN, views, frame);
        action(base, angle, INT64_MAX, views, frame);
        action(base, angle, INT_MIN, views, frame);
        action(base, angle, INT_MAX, views, frame);
    }
    for (int frame = -8; frame < 80; ++frame)
        for (int views = 0; views < 9; ++views)
            for (int start = -12; start < 12; ++start)
                action(12, 3, start, views, frame);
    for (unsigned i = 0; i < 100000; ++i)
        action(int32_t(randomValue()), int32_t(randomValue()), int32_t(randomValue()),
               int32_t(randomValue()), int32_t(randomValue()));
    frames<3>(); frames<16>(); frames<23>(); frames<39>(); frames<2048>();
    std::printf("PASS: %u renderer bounds checks\n", checks);
}
