// SPDX-License-Identifier: GPL-2.0-or-later
// Include the implementation to construct native test worlds without exposing
// test-only schema types in the game API. Section GC discards game integration;
// every assertion below exercises the production schema/graph decoder.
#include "../../source/blood/src/netsnapshot.cpp"

#include <cstdio>
#include <cstdlib>

namespace
{
unsigned assertions = 0;
void require(bool condition, const char* message)
{
    ++assertions;
    if (!condition) { fprintf(stderr, "snapshot state test failed: %s\n", message); exit(1); }
}

std::unique_ptr<ClientWorld> initialWorld()
{
    auto w = std::unique_ptr<ClientWorld>(new ClientWorld());
    w->mask = 1;
    w->options.nGameType = kGameTypeCoop;
    w->options.nDifficulty = w->options.nDifficultyQuantity = w->options.nDifficultyHealth = 2;
    strcpy(w->options.zLevelName, "E1M1");
    w->options.uMapCRC = 0x12345678;
    w->frame = 420; w->clock = 1680;
    w->sectors.resize(1); w->walls.resize(4);
    w->sectors[0].value.wallptr = 0; w->sectors[0].value.wallnum = 4;
    w->sectors[0].value.floorz = 16384;
    w->sectors[0].head = 0;
    for (int i = 0; i < 4; ++i)
    {
        auto& a = w->walls[i].value;
        a.x = (i == 1 || i == 2) ? 4096 : 0;
        a.y = (i >= 2) ? 4096 : 0;
        a.point2 = (i+1) % 4; a.nextwall = a.nextsector = -1; a.extra = -1;
    }
    SpriteState s;
    s.value.index = 0; s.value.type = kDudePlayer1; s.value.extra = 1;
    s.value.statnum = kStatDude; s.value.sectnum = 0; s.value.owner = -1;
    s.value.xrepeat = s.value.yrepeat = 64;
    s.value.x = s.value.y = 2048;
    s.previousSector = s.previousStatus = 0;
    s.generation = 41;
    s.extra.reference = 0; s.extra.health = 1600;
    w->sprites.push_back(s);
    std::fill(std::begin(w->statusHeads), std::end(w->statusHeads), -1);
    w->statusHeads[kStatDude] = 0;
    auto& p = w->players[0];
    p.nPlayer = p.nSprite = 0;
    p.weaponQav = p.sceneQav = p.weaponAmmo = p.packItemId = -1;
    p.ammoCount[3] = 17;
    strcpy(w->profiles[0].name, "Test player");
#ifdef NOONE_EXTENSIONS
    std::fill(std::begin(w->qavControllers), std::end(w->qavControllers), -1);
#endif
    return w;
}

std::vector<uint8_t> encode(ClientWorld& world)
{
    std::vector<uint8_t> bytes;
    std::string error;
    Archive writer(bytes, error);
    require(stateFields(writer, world), error.c_str());
    return bytes;
}

void rejected(ClientWorld& world, const char* why)
{
    auto bytes = encode(world);
    std::string error;
    require(!netSnapshotValidate(bytes, error), why);
    require(!error.empty(), "rejection explains its cause");
}

void fieldRejected(ClientWorld& world)
{
    std::vector<uint8_t> bytes;
    std::string error;
    Archive writer(bytes, error);
    require(!stateFields(writer, world), "out-of-range native field rejected");
}
}

int main()
{
    auto w = initialWorld();
    auto good = encode(*w);
    std::string error;
    require(netSnapshotValidate(good, error), error.c_str());
    auto decoded = std::unique_ptr<ClientWorld>(new ClientWorld());
    require(decode(good, *decoded, error), error.c_str());
    require(encode(*decoded) == good, "field codec roundtrip is byte exact");
    GAMEOPTIONS options = {};
    uint8_t mask = 0;
    require(netSnapshotGetOptions(good, options, mask, error), error.c_str());
    require(mask == 1 && options.uMapCRC == 0x12345678 && !strcmp(options.zLevelName, "E1M1"), "bootstrap metadata");
    require(decoded->players[0].ammoCount[3] == 17 && decoded->sprites[0].generation == 41, "HUD and generation fields");

    w = initialWorld(); w->players[0].weaponAmmo = 12; rejected(*w, "HUD ammunition index bound");
    w = initialWorld(); w->players[0].packItemId = kPackMax; rejected(*w, "HUD inventory index bound");
    w = initialWorld(); w->players[0].posture = kPostureMax; rejected(*w, "prediction posture index bound");
    w = initialWorld(); w->players[0].lifeMode = kModeMax; rejected(*w, "prediction life mode index bound");
    w = initialWorld(); w->players[0].weaponQav = 126; rejected(*w, "weapon animation index bound");
    w = initialWorld(); w->players[0].nSprite = 2; rejected(*w, "player references absent sprite");
    w = initialWorld(); w->mask = 3; rejected(*w, "roster cannot fabricate player state");
    w = initialWorld(); w->sectors[0].value.wallnum = 5; rejected(*w, "sector range outside world");
    w = initialWorld(); w->walls[1].value.point2 = 1; rejected(*w, "merged/nonclosing wall loop");
    w = initialWorld(); w->walls[1].value.nextwall = 2; rejected(*w, "portal missing paired sector");
    w = initialWorld(); w->walls[1].value.nextwall = 2; w->walls[1].value.nextsector = 0;
    require(netSnapshotValidate(encode(*w), error), "bounded asymmetric legacy portal accepted");
    w->walls[1].value.nextwall = 4; rejected(*w, "portal wall index outside world");
    w = initialWorld(); w->walls[1].value.nextwall = 2; w->walls[1].value.nextsector = 1;
    rejected(*w, "portal sector index outside world");
    w = initialWorld(); w->sectors[0].value.wallnum = 2; w->walls.resize(2);
    w->walls[1].value.point2 = 0;
    require(netSnapshotValidate(encode(*w), error), "E6M7 two-wall sector accepted");
    w = initialWorld(); w->walls[0].value.overpicnum = -1;
    require(netSnapshotValidate(encode(*w), error), "unused legacy overlay sentinel accepted");
    w->walls[0].value.cstat = 16; rejected(*w, "visible negative overlay rejected");
    w = initialWorld(); w->sprites[0].extra.reference = 1; rejected(*w, "mismatched extended sprite reference");
    w = initialWorld(); w->sprites[0].nextSector = 0; rejected(*w, "cyclic sector sprite list");
    w = initialWorld(); w->sprites[0].nextStatus = 0; rejected(*w, "cyclic status sprite list");
    w = initialWorld(); w->sprites[0].previousStatus = -1; rejected(*w, "head must contain Blood tail backlink");
    w = initialWorld(); w->statusHeads[kStatDude] = -1; rejected(*w, "orphaned sprite");
    w = initialWorld(); w->sectors[0].upperLink = 2; rejected(*w, "warp link references absent sprite");
    w = initialWorld(); w->sprites[0].value.extra = -1; rejected(*w, "actor requires extended state");
    w = initialWorld(); w->sprites[0].attachedHand = true; w->sprites[0].extra.target = -1; rejected(*w, "attached hand requires target");
    w = initialWorld(); w->sprites.push_back(w->sprites[0]); rejected(*w, "duplicate sprite identity");
    w = initialWorld(); w->sprites[0].generation = 0; fieldRejected(*w);
    w = initialWorld(); w->sprites[0].extension.alpha = std::numeric_limits<float>::quiet_NaN(); fieldRejected(*w);
#ifdef NOONE_EXTENSIONS
    w = initialWorld(); w->modern = 1; require(netSnapshotValidate(encode(*w), error), "modern scalar state accepted");
    w->players[0].sceneQav = 300; rejected(*w, "QAV scene requires live controller");
    w = initialWorld(); w->modern = 1; LaserState laser; laser.owner = 1; w->lasers.push_back(laser);
    rejected(*w, "laser references absent owner");
    w = initialWorld(); w->modern = 1; laser.owner = 0; w->lasers.push_back(laser);
    rejected(*w, "laser cannot refer to ordinary player");
#endif
    // Prefixes of a real valid packet reach all variable-length/truncation paths.
    for (size_t length = 0; length < good.size(); ++length)
    {
        std::vector<uint8_t> prefix(good.begin(), good.begin()+length);
        require(!netSnapshotValidate(prefix, error), "truncated world must be rejected");
    }
    auto trailing = good; trailing.push_back(0);
    require(!netSnapshotValidate(trailing, error), "trailing data rejected");
    uint32_t random = 0x230451u;
    for (unsigned i = 0; i < 4000; ++i)
    {
        auto changed = good;
        random = random * 1664525u + 1013904223u;
        size_t const index = random % changed.size();
        random = random * 1664525u + 1013904223u;
        changed[index] ^= uint8_t(1u << (random & 7));
        // Mutating a legitimate scalar can produce another legitimate world.
        // Every accepted value must survive a second strict decode/encode.
        if (netSnapshotValidate(changed, error))
        {
            auto copy = std::unique_ptr<ClientWorld>(new ClientWorld());
            require(decode(changed, *copy, error), "accepted mutation decodes");
            require(encode(*copy) == changed, "accepted mutation is canonical");
        }
        else require(!error.empty(), "malformed mutation reports failure");
    }
    printf("snapshot state: %u assertions passed (%zu-byte canonical world)\n", assertions, good.size());
}
