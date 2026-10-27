// Server-authoritative player collision history.
// SPDX-License-Identifier: GPL-2.0-only
// This is an original Build-engine implementation of historical hit testing.
// Stock Quake III Arena has no player-rewind hitscan algorithm to adapt.
#include "compat.h"
#include "build.h"
#include "common_game.h"
#include "db.h"
#include "mmulti.h"
#include "netlagcomp.h"
#include "netsnapshot.h"
#include "network.h"
#include "player.h"

#include <array>
#include <cstdint>
#include <cstring>

namespace
{
constexpr unsigned HistorySize = kTicsPerSec + 1;
static_assert(kMaxPlayers == 8, "Update the rewind scope's fixed player storage");
struct Sample
{
    uspritetype value = {};
    uint32_t generation = 0, epoch = 0;
    bool valid = false;
};
struct Frame
{
    uint32_t tick = 0;
    std::array<Sample, kMaxPlayers> players;
};
std::array<Frame, HistorySize> history;
std::array<uint32_t, kMaxPlayers> epochs;
unsigned frameCount = 0, nextFrame = 0;
bool rewinding = false;

bool newer(uint32_t a, uint32_t b) { return int32_t(a - b) > 0; }
Frame const &frame(unsigned age) { return history[(nextFrame + HistorySize - 1 - age) % HistorySize]; }
bool playerValid(int player)
{
    auto const &p = gPlayer[player];
    if (unsigned(p.nSprite) >= kMaxSprites || p.pSprite != &sprite[p.nSprite] || !p.pXSprite || p.pXSprite->health <= 0)
        return false;
    auto const &s = *p.pSprite;
    return s.index == p.nSprite && s.type == kDudePlayer1 + player && s.statnum == kStatDude &&
        unsigned(s.sectnum) < unsigned(numsectors);
}
bool matches(Sample const &sample, int player)
{
    return sample.valid && sample.epoch == epochs[player] && sample.value.index == gPlayer[player].nSprite &&
        unsigned(sample.value.sectnum) < unsigned(numsectors) &&
        sample.generation == netSnapshotSpriteGeneration(gPlayer[player].nSprite);
}
bool continuous(uspritetype const &a, uspritetype const &b)
{
    // Match remote-player presentation: sector transitions and large warps
    // snap rather than drawing collision in the space between two rooms.
    return a.sectnum == b.sectnum && a.statnum == b.statnum && a.type == b.type && a.extra == b.extra &&
        klabs(int64_t(a.x) - b.x) < 8192 && klabs(int64_t(a.y) - b.y) < 8192 &&
        klabs(int64_t(a.z) - b.z) < 32768;
}
int32_t interpolate(int32_t a, int32_t b, uint64_t offset, uint64_t duration)
{
    return int32_t(int64_t(a) + (int64_t(b) - a) * int64_t(offset) / int64_t(duration));
}
bool historicalPlayer(int player, NetLagCompensationTime time, uspritetype &result)
{
    Frame const *lower = nullptr, *upper = nullptr;
    for (unsigned age = 0; age < frameCount; ++age)
    {
        auto const &candidate = frame(age);
        if (frame(0).tick - candidate.tick > kTicsPerSec) continue;
        if (!matches(candidate.players[player], player)) continue;
        if (newer(candidate.tick, time.tick)) upper = &candidate;
        else { lower = &candidate; break; }
    }
    // Never use a previous life, or invent a position before this player's
    // first sample after spawning/teleporting. Such targets stay current.
    if (!lower) return false;
    result = lower->players[player].value;
    uint64_t const offset = (uint64_t(time.tick - lower->tick) << 16) + time.fraction;
    if (upper && offset)
    {
        auto const &after = upper->players[player].value;
        // Shape and animation come from the newest endpoint, as on clients.
        result = after;
        if (continuous(lower->players[player].value, after))
        {
            auto const &before = lower->players[player].value;
            uint64_t const duration = uint64_t(upper->tick - lower->tick) << 16;
            result.x = interpolate(before.x, after.x, offset, duration);
            result.y = interpolate(before.y, after.y, offset, duration);
            result.z = interpolate(before.z, after.z, offset, duration);
            int const angleDelta = ((after.ang - before.ang + 1024) & 2047) - 1024;
            result.ang = int16_t((before.ang + int64_t(angleDelta) * int64_t(offset) / int64_t(duration)) & 2047);
        }
    }
    return true;
}
void collisionState(spritetype &destination, uspritetype const &source)
{
    destination.xyz = source.xyz;
    destination.ang = source.ang;
    destination.cstat = source.cstat;
    destination.picnum = source.picnum;
    destination.clipdist = source.clipdist;
    destination.xrepeat = source.xrepeat;
    destination.yrepeat = source.yrepeat;
    destination.xoffset = source.xoffset;
    destination.yoffset = source.yoffset;
}
} // namespace

void netLagCompensationReset()
{
    // Lifecycle resets run outside collision scopes on the simulation thread.
    frameCount = nextFrame = 0;
    epochs.fill(0);
}

void netLagCompensationInvalidatePlayer(int player)
{
    if (unsigned(player) >= kMaxPlayers) return;
    // Explicitly clear validity too, so even epoch wrap cannot revive a sample.
    ++epochs[player];
    for (auto &entry : history) entry.players[player].valid = false;
}

void netLagCompensationCapture(uint32_t tick)
{
    if (!netIsServer() || rewinding) return;
    Frame *entry;
    if (frameCount && frame(0).tick == tick)
        entry = &history[(nextFrame + HistorySize - 1) % HistorySize];
    else
    {
        // Map changes and clock resets must not join unrelated timelines.
        if (frameCount && !newer(tick, frame(0).tick)) netLagCompensationReset();
        entry = &history[nextFrame];
        nextFrame = (nextFrame + 1) % HistorySize;
        if (frameCount < HistorySize) ++frameCount;
    }
    *entry = Frame{};
    entry->tick = tick;
    for (int p = connecthead, visited = 0; unsigned(p) < kMaxPlayers && visited < kMaxPlayers; p = connectpoint2[p], ++visited)
    {
        if (!playerValid(p)) continue;
        auto &sample = entry->players[p];
        std::memcpy(&sample.value, &sprite[gPlayer[p].nSprite], sizeof(sample.value));
        sample.generation = netSnapshotSpriteGeneration(gPlayer[p].nSprite);
        sample.epoch = epochs[p];
        sample.valid = true;
    }
}

void NetLagCompensationScope::setLink(int16_t &link, int16_t value)
{
    links_[linkCount_++] = {&link, link};
    link = value;
}

void NetLagCompensationScope::moveSector(int index, int target)
{
    auto &s = sprite[index];
    if (s.sectnum == target) return;
    // NBlood's sector list appends at the tail. prev[head] holds that tail;
    // its last next entry is -1. Record every touched entry before changing it
    // and undo in reverse order to preserve the exact original list order.
    int const previousSector = s.sectnum;
    int other = nextspritesect[index];
    if (other < 0) other = headspritesect[previousSector];
    setLink(prevspritesect[other], prevspritesect[index]);
    if (headspritesect[previousSector] != index)
        setLink(nextspritesect[prevspritesect[index]], nextspritesect[index]);
    else
        setLink(headspritesect[previousSector], nextspritesect[index]);
    other = headspritesect[target];
    if (other >= 0)
    {
        setLink(prevspritesect[index], prevspritesect[other]);
        setLink(nextspritesect[index], -1);
        setLink(nextspritesect[prevspritesect[other]], int16_t(index));
        setLink(prevspritesect[other], int16_t(index));
    }
    else
    {
        setLink(prevspritesect[index], int16_t(index));
        setLink(nextspritesect[index], -1);
        setLink(headspritesect[target], int16_t(index));
    }
    s.sectnum = int16_t(target);
}

NetLagCompensationScope::NetLagCompensationScope(spritetype *shooter)
{
    if (!shooter || !netIsServer() || !netInServerTick() || rewinding || !frameCount) return;
    int const player = shooter->type - kDudePlayer1;
    if (unsigned(player) >= kMaxPlayers || !playerValid(player) || shooter != gPlayer[player].pSprite) return;
    NetLagCompensationTime time;
    if (!netGetLagCompensationTime(player, time)) return;
    // Defend the collision history independently of the wire time validation.
    uint32_t const latest = frame(0).tick, oldest = frame(frameCount - 1).tick;
    if (newer(time.tick, latest) || time.tick == latest) time = {latest, 0};
    uint32_t const minimum = newer(oldest, latest - kTicsPerSec) ? oldest : latest - kTicsPerSec;
    if (newer(minimum, time.tick)) time = {minimum, 0};
    rewinding = active_ = true;
    for (int p = connecthead, visited = 0; unsigned(p) < kMaxPlayers && visited < kMaxPlayers; p = connectpoint2[p], ++visited)
    {
        if (p == player || !playerValid(p)) continue;
        uspritetype desired;
        if (!historicalPlayer(p, time, desired)) continue;
        int const index = gPlayer[p].nSprite;
        std::memcpy(&saved_[count_], &sprite[index], sizeof(saved_[count_]));
        indices_[count_++] = index;
        moveSector(index, desired.sectnum);
        collisionState(sprite[index], desired);
    }
}

NetLagCompensationScope::~NetLagCompensationScope()
{
    if (!active_) return;
    while (linkCount_)
    {
        auto const &change = links_[--linkCount_];
        *change.address = change.value;
    }
    while (count_)
    {
        --count_;
        auto &current = sprite[indices_[count_]];
        collisionState(current, saved_[count_]);
        current.sectnum = saved_[count_].sectnum;
    }
    rewinding = false;
}
