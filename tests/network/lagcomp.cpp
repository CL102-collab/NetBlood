// Production player-history rewind and real Build collision in an asset-free map.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "compat.h"
#include "build.h"
#include "common_game.h"
#include "db.h"
#include "log.h"
#include "mmulti.h"
#include "netlagcomp.h"
#include "network.h"
#include "player.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

PLAYER gPlayer[kMaxPlayers];
int connecthead = 0, connectpoint2[MAXMULTIPLAYERS];
vec2_t hitscangoal = {1 << 27, 1 << 27};
#ifdef USE_STRUCT_TRACKERS
// The collision-engine objects use the same storage layout without hooks.
// ASan guards these exact-size arrays against hooks accidentally invoked on
// temporary/history sprites outside the authoritative sprite array.
static_assert(sizeof(spritetype) == sizeof(uspritetype), "tracked sprite storage must retain the engine ABI");
uint32_t sectorchanged[MAXSECTORS + M32_FIXME_SECTORS];
uint32_t wallchanged[MAXWALLS + M32_FIXME_WALLS];
uint32_t spritechanged[MAXSPRITES];
#endif
static XSPRITE playerX[kMaxPlayers];
static uint32_t generations[kMaxSprites];
static bool server = true, inTick = true, haveTime = true;
static NetLagCompensationTime shotTime;
static unsigned checks = 0;

bool netIsServer() { return server; }
bool netInServerTick() { return inTick; }
bool netGetLagCompensationTime(int, NetLagCompensationTime &time)
{
    time = shotTime;
    return haveTime;
}
uint32_t netSnapshotSpriteGeneration(int index) { return generations[index]; }
// The test uses opaque face sprites only. An unexpected asset request fails.
bool tileLoad(int16_t)
{
    std::fputs("lagcomp test unexpectedly requested game art\n", stderr);
    std::abort();
}
namespace loguru
{
Verbosity current_verbosity_cutoff() { return Verbosity_OFF; }
void log(Verbosity, const char *, unsigned, const char *, ...) {}
}

static void check(bool condition, const char *message)
{
    ++checks;
    if (!condition)
    {
        std::fprintf(stderr, "lagcomp test failed: %s\n", message);
        std::exit(1);
    }
}

// NBlood sector lists append to a tail stored in prevspritesect[head].
static void putInSector(int index, int sectorIndex)
{
    int head = headspritesect[sectorIndex];
    if (head < 0)
    {
        headspritesect[sectorIndex] = index;
        prevspritesect[index] = index;
    }
    else
    {
        int tail = prevspritesect[head];
        nextspritesect[tail] = index;
        prevspritesect[index] = tail;
        prevspritesect[head] = index;
    }
    nextspritesect[index] = -1;
    sprite[index].sectnum = sectorIndex;
}

static void rebuildLists()
{
    std::fill(std::begin(headspritesect), std::end(headspritesect), int16_t(-1));
    std::fill(std::begin(prevspritesect), std::end(prevspritesect), int16_t(-1));
    std::fill(std::begin(nextspritesect), std::end(nextspritesect), int16_t(-1));
    for (int p = connecthead; p >= 0; p = connectpoint2[p])
        putInSector(gPlayer[p].nSprite, sprite[gPlayer[p].nSprite].sectnum);
}

static void addPlayer(int player, int x, int y, int sectorIndex = 0)
{
    auto &s = sprite[player];
    s = {};
    s.index = player;
    s.x = x; s.y = y; s.z = 0;
    s.type = kDudePlayer1 + player;
    s.statnum = kStatDude;
    s.sectnum = sectorIndex;
    s.extra = player + 1;
    s.picnum = 1;
    s.cstat = 256;
    s.xrepeat = s.yrepeat = 64;
    s.clipdist = 32;
    generations[player] = 1;
    gPlayer[player] = {};
    gPlayer[player].nPlayer = player;
    gPlayer[player].nSprite = player;
    gPlayer[player].pSprite = &s;
    gPlayer[player].pXSprite = &playerX[player];
    playerX[player] = {};
    playerX[player].health = 100 << 4;
}

static void reset(int players = 2)
{
    netLagCompensationReset();
    server = inTick = haveTime = true;
    shotTime = {};
    std::memset(sprite, 0, sizeof(sprite));
    std::memset(sector, 0, sizeof(sector));
    std::memset(wall, 0, sizeof(wall));
    std::memset(picanm, 0, sizeof(picanm));
    std::memset(generations, 0, sizeof(generations));
    for (auto &player : gPlayer) player = {};
    for (int &next : connectpoint2) next = -1;
    connecthead = 0;
    for (int p = 0; p < players - 1; ++p) connectpoint2[p] = p + 1;
    numsectors = 2; numwalls = 8;
    for (int s = 0; s < 2; ++s)
    {
        sector[s].wallptr = s * 4;
        sector[s].wallnum = 4;
        sector[s].ceilingz = -16384;
        sector[s].floorz = 8192;
        int const left = -2048 + s * 4096, right = left + 4096;
        int const coords[4][2] = {{left, -2048}, {right, -2048}, {right, 2048}, {left, 2048}};
        for (int n = 0; n < 4; ++n)
        {
            int i = s * 4 + n;
            wall[i].x = coords[n][0]; wall[i].y = coords[n][1];
            wall[i].point2 = s * 4 + (n + 1) % 4;
            wall[i].nextsector = wall[i].nextwall = -1;
        }
    }
    wall[1].nextsector = 1; wall[1].nextwall = 7;
    wall[7].nextsector = 0; wall[7].nextwall = 1;
    tilesiz[1] = {32, 32};
    enginecompatibilitymode = ENGINE_EDUKE32;
    for (int p = 0; p < players; ++p) addPlayer(p, p ? 1024 + (p - 1) * 512 : -1024, 0);
    rebuildLists();
}

struct World
{
    std::array<spritetype, kMaxSprites> sprites;
    std::array<int16_t, MAXSECTORS + 1> heads;
    std::array<int16_t, MAXSPRITES> next, previous;
    World()
    {
        std::memcpy(sprites.data(), sprite, sizeof(sprite));
        std::memcpy(heads.data(), headspritesect, sizeof(headspritesect));
        std::memcpy(next.data(), nextspritesect, sizeof(nextspritesect));
        std::memcpy(previous.data(), prevspritesect, sizeof(prevspritesect));
    }
    void checkRestored() const
    {
        check(!std::memcmp(sprites.data(), sprite, sizeof(sprite)), "every sprite field restores exactly after trace");
        check(!std::memcmp(heads.data(), headspritesect, sizeof(headspritesect)), "sector list heads restore exactly");
        check(!std::memcmp(next.data(), nextspritesect, sizeof(nextspritesect)), "sector list forward order restores exactly");
        check(!std::memcmp(previous.data(), prevspritesect, sizeof(prevspritesect)), "sector list back links and tail sentinels restore exactly");
    }
};

static int trace()
{
    vec3_t source = {-1024, 0, -4096};
    hitdata_t hit = {};
    // VectorScan normally excludes the shooter before invoking hitscan.
    uint16_t const cstat = sprite[0].cstat;
    sprite[0].cstat &= ~256;
    hitscangoal = {1 << 27, 1 << 27};
    hitscan(&source, 0, 16384, 0, 0, &hit, CLIPMASK1);
    sprite[0].cstat = cstat;
    return hit.sprite;
}

static void hitHistory()
{
    reset(3);
    netLagCompensationCapture(0);
    sprite[0].x = -512;
    sprite[1].y = sprite[2].y = 1024;
    netLagCompensationCapture(1);
    check(trace() < 0, "present-time shot misses moving targets");
    World before;
    {
        NetLagCompensationScope scope(&sprite[0]);
        check(sprite[0].x == -512, "shooter is never rewound");
        check(sprite[1].y == 0 && sprite[2].y == 0, "all eligible target poses rewind together");
        check(trace() == 1, "real Build hitscan hits historical target nearest the shooter");
        NetLagCompensationScope nested(&sprite[0]);
        check(trace() == 1, "nested scope preserves the outer trace pose");
    }
    before.checkRestored();
    check(trace() < 0, "next trace sees present-time state after scope exits");
}

static void interpolation()
{
    reset();
    sprite[1].y = -512; sprite[1].ang = 2040;
    netLagCompensationCapture(10);
    sprite[1].y = 512; sprite[1].ang = 8;
    netLagCompensationCapture(11);
    shotTime = {10, 32768};
    World before;
    {
        NetLagCompensationScope scope(&sprite[0]);
        check(sprite[1].y == 0, "fractional command time interpolates between collision samples");
        check(sprite[1].ang == 0, "angle interpolation follows shortest path around wrap");
        check(trace() == 1, "half-tick historical position produces a real hit");
    }
    before.checkRestored();
}

static void sectorRewind()
{
    reset(3);
    netLagCompensationCapture(10);
    sprite[1].x = 3072; sprite[1].y = 1024; sprite[1].sectnum = 1;
    sprite[2].x = 4096; sprite[2].y = 1024; sprite[2].sectnum = 1;
    rebuildLists();
    netLagCompensationCapture(11);
    shotTime = {10, 0};
    World before;
    {
        NetLagCompensationScope scope(&sprite[0]);
        check(sprite[1].sectnum == 0 && sprite[2].sectnum == 0, "historical sector membership is installed for all targets");
        check(trace() == 1, "real hitscan finds target after sector list relinking");
    }
    before.checkRestored();
}

static void boundsAndReset()
{
    reset();
    for (uint32_t tick = 0; tick <= 60; ++tick)
    {
        sprite[1].x = 1024 + int(tick) * 8;
        netLagCompensationCapture(tick);
    }
    shotTime = {0, 0};
    {
        NetLagCompensationScope scope(&sprite[0]);
        check(sprite[1].x == 1024 + 30 * 8, "old requests are capped at one second of retained history");
    }
    shotTime = {60, 0};
    {
        NetLagCompensationScope scope(&sprite[0]);
        check(sprite[1].x == 1024 + 60 * 8, "current-time request does not rewind");
    }
    shotTime = {61, 65535};
    {
        NetLagCompensationScope scope(&sprite[0]);
        check(sprite[1].x == 1024 + 60 * 8, "future collision times cannot move targets ahead of current state");
    }
    netLagCompensationReset();
    shotTime = {30, 0};
    {
        NetLagCompensationScope scope(&sprite[0]);
        check(sprite[1].x == 1024 + 60 * 8, "map/session reset removes all old collision history");
    }
}

static void collisionShapeAndDiscontinuities()
{
    reset();
    netLagCompensationCapture(1);
    sprite[1].yrepeat = 16;
    sprite[1].xrepeat = 20;
    sprite[1].clipdist = 8;
    sprite[1].xoffset = 3;
    sprite[1].yoffset = -2;
    sprite[1].picnum = 2;
    tilesiz[2] = {32, 32};
    netLagCompensationCapture(2);
    shotTime = {1, 0};
    check(trace() < 0, "present crouched collision is below the test ray");
    World before;
    {
        NetLagCompensationScope scope(&sprite[0]);
        check(sprite[1].picnum == 1 && sprite[1].yrepeat == 64 && sprite[1].clipdist == 32,
              "historical art, stance, and collision dimensions are installed together");
        check(sprite[1].xoffset == 0 && sprite[1].yoffset == 0, "historical sprite offsets are restored for collision");
        check(trace() == 1, "standing historical collision registers a real hit while present crouch misses");
    }
    before.checkRestored();
    // Damage is deliberately applied after the read-only trace scope exits.
    playerX[1].health -= 100;
    check(playerX[1].health == (100 << 4) - 100 && sprite[1].yrepeat == 16,
          "authoritative damage applies to restored present state");

    reset();
    sprite[1].y = -512;
    netLagCompensationCapture(1);
    sprite[1].y = 8192;
    netLagCompensationCapture(2);
    shotTime = {1, 32768};
    { NetLagCompensationScope scope(&sprite[0]); check(sprite[1].y == 8192, "large warps never interpolate a collision pose through intervening space"); }

    reset();
    netLagCompensationCapture(1);
    sprite[1].x = 3072; sprite[1].sectnum = 1;
    rebuildLists();
    netLagCompensationCapture(2);
    shotTime = {1, 32768};
    World sectorBefore;
    {
        NetLagCompensationScope scope(&sprite[0]);
        check(sprite[1].x == 3072 && sprite[1].sectnum == 1,
              "fractional sector transitions use the same snapped endpoint as client presentation");
    }
    sectorBefore.checkRestored();
}

static void identityAndEligibility()
{
    reset();
    netLagCompensationCapture(1);
    sprite[1].y = 1024;
    netLagCompensationCapture(2);
    shotTime = {1, 0};
    ++generations[1];
    {
        NetLagCompensationScope scope(&sprite[0]);
        check(sprite[1].y == 1024, "sprite reuse cannot rewind a new entity into its predecessor's history");
    }
    --generations[1];
    playerX[1].health = 0;
    {
        NetLagCompensationScope scope(&sprite[0]);
        check(sprite[1].y == 1024, "dead targets are not resurrected for collision");
    }
    playerX[1].health = 100 << 4;
    netLagCompensationInvalidatePlayer(1);
    {
        NetLagCompensationScope scope(&sprite[0]);
        check(sprite[1].y == 1024, "spawn and teleport invalidation removes the old life or location");
    }
    for (int mode = 0; mode < 3; ++mode)
    {
        reset();
        netLagCompensationCapture(1);
        sprite[1].y = 1024;
        netLagCompensationCapture(2);
        shotTime = {1, 0};
        if (mode == 0) server = false;
        if (mode == 1) inTick = false;
        if (mode == 2) haveTime = false;
        World before;
        { NetLagCompensationScope scope(&sprite[0]); check(sprite[1].y == 1024, "offline, client, out-of-tick and untimed shots preserve current poses"); }
        before.checkRestored();
    }
}

static void clockWrap()
{
    reset();
    uint32_t const last = std::numeric_limits<uint32_t>::max();
    sprite[1].y = 0;
    netLagCompensationCapture(last);
    sprite[1].y = 1024;
    netLagCompensationCapture(0);
    shotTime = {last, 0};
    World before;
    { NetLagCompensationScope scope(&sprite[0]); check(trace() == 1, "history selection survives server tick wrap"); }
    before.checkRestored();
}

static void fullRosterSectorRestore()
{
    reset(kMaxPlayers);
    for (int p = 1; p < kMaxPlayers; ++p)
    {
        sprite[p].x = 512 + p * 64;
        sprite[p].y = p * 16;
    }
    netLagCompensationCapture(5);
    for (int p = 1; p < kMaxPlayers; ++p)
    {
        sprite[p].x += 4096;
        sprite[p].sectnum = 1;
    }
    rebuildLists();
    netLagCompensationCapture(6);
    shotTime = {5, 0};
    World before;
    {
        NetLagCompensationScope scope(&sprite[0]);
        for (int p = 1; p < kMaxPlayers; ++p)
            check(sprite[p].sectnum == 0 && sprite[p].x == 512 + p * 64,
                  "full-roster rewind restores every target while emptying its current sector");
    }
    before.checkRestored();
}

int main()
{
    initdivtables();
    hitHistory();
    interpolation();
    sectorRewind();
    boundsAndReset();
    collisionShapeAndDiscontinuities();
    identityAndEligibility();
    clockWrap();
    fullRosterSectorRestore();
#ifdef USE_STRUCT_TRACKERS
    check(spritechanged[1] > 0, "production sprite tracker hooks execute on the authoritative array");
    std::printf("lag compensation: %u checks passed (tracked sprites and Build hitscan)\n", checks);
#else
    std::printf("lag compensation: %u checks passed (production rewind and Build hitscan)\n", checks);
#endif
}
