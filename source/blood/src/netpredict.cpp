// Authoritative-network presentation and local movement prediction.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "compat.h"
#include "build.h"
#include "common_game.h"
#include "cosmeticrandom.h"
#include "db.h"
#include "globals.h"
#include "netdemo.h"
#include "netlag.h"
#include "netpredict.h"
#include "netsnapshot.h"
#include "mmulti.h"
#include "network.h"
#include "player.h"
#include "timer.h"
#include "view.h"
#include "weapon.h"

#include <algorithm>
#include <array>
#include <deque>
#include <limits>
#include <vector>

extern bool gPrediction;

namespace
{
struct Command { uint32_t sequence; GINPUT input; uint32_t cosmeticSeed; };
struct Entity
{
    int32_t x, y, z;
    int16_t angle, sector, status, type, owner, extra;
    uint32_t generation;
};
struct SectorPosition { int32_t ceiling, floor; int16_t ceilingSlope, floorSlope; };
struct WallPosition { int32_t x, y; };
std::deque<Command> commands;
struct PredictedSound { uint32_t sequence, time; int sprite, sound, variants, channel; };
std::deque<PredictedSound> sounds;
uint32_t predictingSequence = 0;
CosmeticRandomGenerator commandCosmetics;
std::array<Entity, kMaxSprites> previousEntities;
std::vector<SectorPosition> previousSectors, savedSectors;
std::vector<WallPosition> previousWalls, savedWalls;
std::vector<int> renderedSectors, renderedWalls;
NetPredictionStats stats;
bool initialized = false, beforeSnapshot = false, rendering = false, overflowed = false;
uint32_t snapshotTick = 0, snapshotTime = 0, commandTime = 0, lastCommand = 0;
uint32_t snapshotInterval = 34;
uint32_t previousSnapshotTick = 0;
NetLagCompensationTime renderedTime;
bool haveRenderedTime = false;
bool haveCommand = false;
int remoteFraction = 65536;

bool newer(uint32_t a, uint32_t b) { return int32_t(a - b) > 0; }
int lerp(int32_t a, int32_t b, int fraction)
{
    return int32_t(int64_t(a) + ((int64_t(b) - a) * fraction) / 65536);
}
Entity entity(spritetype const &s)
{
    return {s.x, s.y, s.z, s.ang, s.sectnum, s.statnum, s.type, s.owner, s.extra, netSnapshotSpriteGeneration(s.index)};
}
bool sameEntity(Entity const &a, spritetype const &b)
{
    // Reused slots, warps, sector teleports, and respawns must never streak
    // across the level. Sector changes snap too: Build's sprite sector lists
    // describe the newest snapshot, not an interpolated position's sector.
    return a.generation == netSnapshotSpriteGeneration(b.index) && a.status == b.statnum && a.type == b.type && a.owner == b.owner &&
        a.extra == b.extra && a.sector == b.sectnum &&
        klabs(int64_t(a.x) - b.x) < 8192 && klabs(int64_t(a.y) - b.y) < 8192 &&
        klabs(int64_t(a.z) - b.z) < 32768;
}
bool localPlayerValid()
{
    return gMe && gMe->pSprite && gMe->pXSprite &&
        gMe->pSprite->sectnum >= 0 && gMe->pSprite->sectnum < numsectors;
}
void captureWorld()
{
    for (int i = 0; i < kMaxSprites; ++i) previousEntities[i] = entity(sprite[i]);
    previousSectors.resize(numsectors);
    previousWalls.resize(numwalls);
    for (int i = 0; i < numsectors; ++i)
        previousSectors[i] = {sector[i].ceilingz, sector[i].floorz, sector[i].ceilingheinum, sector[i].floorheinum};
    for (int i = 0; i < numwalls; ++i) previousWalls[i] = {wall[i].x, wall[i].y};
    for (int i = connecthead; i >= 0; i = connectpoint2[i])
        if (gPlayer[i].pSprite && gPlayer[i].pXSprite) viewBackupView(i);
}
void predict(Command const &command, bool replay)
{
    if (gPrediction && localPlayerValid() && gMe->pXSprite->health > 0)
    {
        GINPUT input = command.input;
        viewUpdatePrediction(&input);
    }
    predictingSequence = command.sequence;
    // Recreate the same visual choices on every replay, independently of
    // one-shot sounds (which are deliberately not emitted during replay).
    commandCosmetics.seed(command.cosmeticSeed);
    WeaponPredictCommand(gMe, command.input, replay);
}
}

bool netPredictActive()
{
    return initialized && !netIntermissionActive() && (netIsClient() || netDemoIsPlaying());
}

void netPredictReset()
{
    netPredictEndRender();
    commands.clear();
    sounds.clear();
    previousSectors.clear(); previousWalls.clear();
    stats = {};
    initialized = beforeSnapshot = overflowed = haveCommand = false;
    lastCommand = snapshotTick = snapshotTime = commandTime = 0;
    snapshotInterval = 34;
    previousSnapshotTick = 0;
    renderedTime = {};
    haveRenderedTime = false;
    WeaponPredictReset(nullptr);
}

NetPredictionStats netPredictGetStats() { return stats; }
NetLagCompensationTime netPredictViewTime()
{ return haveRenderedTime ? renderedTime : NetLagCompensationTime{snapshotTick, 0}; }

void netPredictCommand(uint32_t sequence, GINPUT const &input)
{
    if (!netPredictActive() || !localPlayerValid() || (haveCommand && !newer(sequence, lastCommand))) return;
    haveCommand = true;
    lastCommand = sequence;
    commandTime = timerGetTicks();
    ++stats.commands;
    if (commands.size() == 256)
    {
        // Never replay an incomplete suffix as if it followed the ACK. Stop
        // speculative movement until the server catches up with this history.
        commands.pop_front();
        overflowed = true;
        ++stats.historyOverflows;
    }
    commands.push_back({sequence, input, cosmeticRandomGenerator().next()});
    stats.pendingCommands = uint32_t(commands.size());
    if (!overflowed) predict(commands.back(), false);
}

void netPredictBeforeSnapshot()
{
    netPredictEndRender();
    if (initialized) captureWorld();
    beforeSnapshot = initialized;
}

void netPredictSnapshot(uint32_t acknowledgedCommand, uint32_t serverTick, bool discontinuity)
{
    if (netIntermissionActive())
    {
        // Inputs sent before the match ended can remain ahead of the final
        // server ACK. Discard them instead of moving/firing past that state.
        commands.clear();
        sounds.clear();
        overflowed = haveCommand = false;
        stats.pendingCommands = 0;
        discontinuity = true;
    }
    if (!localPlayerValid()) return;
    const bool first = !initialized;
    if (!first && (!beforeSnapshot || previousSectors.size() != size_t(numsectors) ||
        previousWalls.size() != size_t(numwalls))) discontinuity = true;
    if (!first && beforeSnapshot)
        discontinuity |= !sameEntity(previousEntities[gMe->pSprite->index], *gMe->pSprite);
    const uint32_t tickDelta = serverTick - snapshotTick;
    if (!first && (tickDelta == 0 || tickDelta > 30)) discontinuity = true;
    snapshotInterval = (!first && tickDelta > 0 && tickDelta <= 30) ? (tickDelta * 1000 + 29) / 30 : 34;
    previousSnapshotTick = first || discontinuity ? serverTick : snapshotTick;
    snapshotTick = serverTick;
    snapshotTime = timerGetTicks();
    initialized = true;
    if (first) commandTime = snapshotTime;
    if (first || discontinuity)
    {
        captureWorld();
        haveRenderedTime = false;
    }
    beforeSnapshot = false;
    while (!commands.empty() && !newer(commands.front().sequence, acknowledgedCommand)) commands.pop_front();
    if (overflowed && (commands.empty() || commands.front().sequence == acknowledgedCommand + 1)) overflowed = false;
    // An explicit map/session reset clears history through netPredictReset().
    // A teleport still replays inputs newer than the authoritative ACK.
    const vec3_t oldPosition = viewPredictionPosition();
    const vec2_t pendingLook = viewPredictionPendingLook();
    viewInitializePrediction();
    WeaponPredictReset(gMe);
    if (!overflowed)
        for (auto const &command : commands) { predict(command, true); ++stats.replayedCommands; }
    if (!first && !discontinuity)
        viewPredictionApplyPendingLook(pendingLook);
    else
        viewPredictionSnap();
    const vec3_t corrected = viewPredictionPosition();
    const int64_t correction = klabs(int64_t(corrected.x) - oldPosition.x) +
        klabs(int64_t(corrected.y) - oldPosition.y) + klabs(int64_t(corrected.z) - oldPosition.z) / 16;
    stats.lastCorrection = first ? 0 : int32_t(std::min<int64_t>(correction, std::numeric_limits<int32_t>::max()));
    stats.maxCorrection = std::max(stats.maxCorrection, stats.lastCorrection);
    ++stats.reconciliations;
    if (discontinuity) ++stats.discontinuities;
    stats.acknowledgedCommand = acknowledgedCommand;
    stats.pendingCommands = uint32_t(commands.size());
}

int netPredictLocalSound(int soundId, int variantCount, int channel)
{
    if (!netPredictActive() || !localPlayerValid()) return soundId;
    // Track the entire original range even when the local cosmetic choice is
    // different from the authoritative variant received a moment later.
    int const variants = int(std::min<int64_t>(std::max(1, variantCount),
        int64_t(std::numeric_limits<int>::max()) - soundId + 1));
    if (sounds.size() == 256) sounds.pop_front();
    sounds.push_back({predictingSequence, timerGetTicks(), gMe->pSprite->index, soundId, variants, channel});
    ++stats.predictedSounds;
    return soundId + CosmeticRandom(variants);
}

int netPredictCosmeticRandom(int upperExclusive)
{
    if (!netPredictActive() || !localPlayerValid() || !haveCommand || upperExclusive <= 1) return 0;
    return int(commandCosmetics.bounded(uint32_t(upperExclusive)));
}

bool netPredictSuppressSound(int spriteIndex, int soundId, uint32_t commandSequence, int channel)
{
    if (!netPredictActive() || !localPlayerValid()) return false;
    const uint32_t now = timerGetTicks();
    while (!sounds.empty() && now - sounds.front().time > 10000) sounds.pop_front();
    for (auto it = sounds.begin(); it != sounds.end(); ++it)
        if ((spriteIndex == it->sprite || spriteIndex == -1) && commandSequence == it->sequence && channel == it->channel &&
            soundId >= it->sound && int64_t(soundId) - it->sound < it->variants)
        {
            sounds.erase(it);
            ++stats.suppressedSounds;
            return true;
        }
    return false;
}

int netPredictLocalFraction()
{
    if (netIntermissionActive()) return 65536;
    return int(std::min<uint64_t>(65536, uint64_t(timerGetTicks() - commandTime) * 30 * 65536 / 1000));
}

void netPredictBeginRender()
{
    if (!netPredictActive() || rendering) return;
    const uint32_t elapsed = timerGetTicks() - snapshotTime;
    remoteFraction = int(std::min<uint64_t>(65536, uint64_t(elapsed) * 65536 / snapshotInterval));
    renderedTime = gViewInterpolate ? netcommandtime::interpolate(previousSnapshotTick, snapshotTick, unsigned(remoteFraction))
                                    : NetLagCompensationTime{snapshotTick, 0};
    haveRenderedTime = true;
    netLagFrame(int(std::min<uint32_t>(elapsed, 10000)) - int(snapshotInterval));
    if (!gViewInterpolate || previousSectors.size() != size_t(numsectors) || previousWalls.size() != size_t(numwalls)) return;
    if (remoteFraction == 65536) return;
    savedSectors.resize(numsectors); savedWalls.resize(numwalls);
    renderedSectors.clear(); renderedWalls.clear();
    for (int i = 0; i < numsectors; ++i)
    {
        auto &s = sector[i]; auto const &p = previousSectors[i];
        // Large geometry changes (map effects/instant movers) snap. Assign
        // only changed fields: Build's tracked fields invalidate renderer
        // geometry caches on assignment, even if assigned their current value.
        if (klabs(int64_t(p.ceiling) - s.ceilingz) >= 65536 || klabs(int64_t(p.floor) - s.floorz) >= 65536) continue;
        SectorPosition const next = {lerp(p.ceiling, s.ceilingz, remoteFraction), lerp(p.floor, s.floorz, remoteFraction),
            int16_t(lerp(p.ceilingSlope, s.ceilingheinum, remoteFraction)), int16_t(lerp(p.floorSlope, s.floorheinum, remoteFraction))};
        if (next.ceiling == s.ceilingz && next.floor == s.floorz && next.ceilingSlope == s.ceilingheinum && next.floorSlope == s.floorheinum) continue;
        savedSectors[i] = {s.ceilingz, s.floorz, s.ceilingheinum, s.floorheinum};
        renderedSectors.push_back(i);
        if (next.ceiling != s.ceilingz) s.ceilingz = next.ceiling;
        if (next.floor != s.floorz) s.floorz = next.floor;
        if (next.ceilingSlope != s.ceilingheinum) s.ceilingheinum = next.ceilingSlope;
        if (next.floorSlope != s.floorheinum) s.floorheinum = next.floorSlope;
    }
    for (int i = 0; i < numwalls; ++i)
    {
        auto &w = wall[i]; auto const &p = previousWalls[i];
        if (klabs(int64_t(p.x) - w.x) >= 8192 || klabs(int64_t(p.y) - w.y) >= 8192) continue;
        WallPosition const next = {lerp(p.x, w.x, remoteFraction), lerp(p.y, w.y, remoteFraction)};
        if (next.x == w.x && next.y == w.y) continue;
        savedWalls[i] = {w.x, w.y};
        renderedWalls.push_back(i);
        if (next.x != w.x) w.x = next.x;
        if (next.y != w.y) w.y = next.y;
    }
    rendering = !renderedSectors.empty() || !renderedWalls.empty();
}

void netPredictEndRender()
{
    if (!rendering) return;
    for (int i : renderedSectors)
    {
        auto &s = sector[i]; auto const &saved = savedSectors[i];
        if (s.ceilingz != saved.ceiling) s.ceilingz = saved.ceiling;
        if (s.floorz != saved.floor) s.floorz = saved.floor;
        if (s.ceilingheinum != saved.ceilingSlope) s.ceilingheinum = saved.ceilingSlope;
        if (s.floorheinum != saved.floorSlope) s.floorheinum = saved.floorSlope;
    }
    for (int i : renderedWalls)
    {
        if (wall[i].x != savedWalls[i].x) wall[i].x = savedWalls[i].x;
        if (wall[i].y != savedWalls[i].y) wall[i].y = savedWalls[i].y;
    }
    rendering = false;
}

void netPredictInterpolateSprite(int index, tspritetype *s)
{
    if (!netPredictActive() || !gViewInterpolate || index < 0 || index >= kMaxSprites || (s->flags & 512)) return;
    auto const &p = previousEntities[index];
    if (!sameEntity(p, sprite[index])) return;
    s->x = lerp(p.x, s->x, remoteFraction); s->y = lerp(p.y, s->y, remoteFraction); s->z = lerp(p.z, s->z, remoteFraction);
    s->ang = int16_t((p.angle + (((s->ang - p.angle + 1024) & 2047) - 1024) * remoteFraction / 65536) & 2047);
}
