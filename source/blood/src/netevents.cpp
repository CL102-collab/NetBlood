// NBlood authoritative presentation events. SPDX-License-Identifier: GPL-2.0-only
#include "netevents.h"
#include "netprotocol.h"
#include "network.h"
#include "netpredict.h"
#include "netsnapshot.h"
#include "mmulti.h"
#include "player.h"
#include "sfx.h"
#include "sound.h"
#include "view.h"

#include <cstring>
#include <algorithm>
#include <utility>

namespace
{
constexpr uint8_t kSoundVersion = 1;
bool gApplyingEvent = false;

struct SoundEvent
{
    NetSoundKind kind = NetSoundKind::Position;
    int32_t sound = -1, channel = -1, flags = 0, pitch = 0, volume = 0;
    int32_t x = 0, y = 0, z = 0, sector = -1;
    int32_t spriteIndex = -1, spriteType = 0, spriteTile = 0, spriteExtra = -1;
    uint32_t command = 0, spriteGeneration = 0;
    int32_t target = -1;
    bool loop = false;
    bool waitingForSprite = false;
    std::string name;
};

std::vector<SoundEvent> gLoops;

bool IsSpatial(const SoundEvent &e) { return e.kind <= NetSoundKind::KillAll3D; }

bool MatchesSprite(const SoundEvent &e)
{
    if (e.spriteIndex < 0) return false;
    auto const &spr = sprite[e.spriteIndex];
    return spr.statnum < kMaxStatus && spr.type == e.spriteType && spr.extra == e.spriteExtra
        && netSnapshotSpriteGeneration(e.spriteIndex) == e.spriteGeneration;
}

void Remember(const SoundEvent &e)
{
    if (e.kind == NetSoundKind::Message) return;
    gLoops.erase(std::remove_if(gLoops.begin(), gLoops.end(), [](const SoundEvent &old) {
        if (old.spriteIndex < 0) return false;
        uint32_t const current = netSnapshotSpriteGeneration(old.spriteIndex);
        return netwire::newer(current, old.spriteGeneration)
            || (current == old.spriteGeneration && sprite[old.spriteIndex].statnum >= kMaxStatus);
    }), gLoops.end());
    if (e.kind == NetSoundKind::KillAll3D || e.kind == NetSoundKind::KillAll2D)
    {
        gLoops.erase(std::remove_if(gLoops.begin(), gLoops.end(), [&](const SoundEvent &old) {
            return IsSpatial(e) == IsSpatial(old); }), gLoops.end());
        return;
    }
    if (e.kind == NetSoundKind::KillSpriteSound || e.kind == NetSoundKind::KillSpriteSounds)
    {
        gLoops.erase(std::remove_if(gLoops.begin(), gLoops.end(), [&](const SoundEvent &old) {
            return old.spriteIndex == e.spriteIndex && old.spriteGeneration == e.spriteGeneration
                && (e.kind == NetSoundKind::KillSpriteSounds
                    || ((e.channel < 0 || old.channel == e.channel) && (e.sound < 0 || old.sound == e.sound))); }), gLoops.end());
        return;
    }
    if (e.channel < 0) return;
    for (auto it = gLoops.begin(); it != gLoops.end(); ++it)
    {
        if (IsSpatial(*it) != IsSpatial(e) || it->channel != e.channel || it->target != e.target
            || (IsSpatial(e) && !(e.flags & 1) && (it->spriteIndex != e.spriteIndex || it->spriteGeneration != e.spriteGeneration))) continue;
        if (IsSpatial(e) && ((e.flags & 4) || ((e.flags & 2) && e.sound == it->sound))) return;
        gLoops.erase(it);
        break;
    }
    if (e.loop && gLoops.size() < 256) gLoops.push_back(e);
}

bool Valid(const SoundEvent &e)
{
    if (e.kind < NetSoundKind::Position || e.kind > NetSoundKind::Message
        || e.sound < -1 || e.sound > 1048575 || e.channel < -1 || e.channel > 255
        || e.flags < 0 || e.flags > 15 || e.pitch < 0 || e.pitch > 0x1000000
        || e.volume < (e.kind == NetSoundKind::Message ? -10 : -1) || e.volume > 255 || e.sector < -1 || e.sector >= kMaxSectors
        || e.spriteIndex < -1 || e.spriteIndex >= kMaxSprites
        || e.spriteType < 0 || e.spriteType > 32767 || e.spriteTile < 0 || e.spriteTile >= MAXTILES
        || e.spriteExtra < -1 || e.spriteExtra >= kMaxXSprites || e.name.size() > (e.kind == NetSoundKind::Message ? 240u : 63u)
        || e.target < -1 || e.target >= kMaxPlayers)
        return false;
    if (e.kind >= NetSoundKind::Sample && e.channel > 31) return false;
    if ((e.kind == NetSoundKind::Position || e.kind == NetSoundKind::Sprite
         || e.kind == NetSoundKind::SpriteCustom || e.kind == NetSoundKind::Sample
         || e.kind == NetSoundKind::Wave) && e.sound < 0) return false;
    if ((e.kind == NetSoundKind::Sprite || e.kind == NetSoundKind::SpriteCustom
         || e.kind == NetSoundKind::KillSpriteSound || e.kind == NetSoundKind::KillSpriteSounds)
        && e.spriteIndex < 0) return false;
    if (e.kind == NetSoundKind::NamedSample)
    {
        if (e.name.empty()) return false;
        for (unsigned char ch : e.name)
            if (!(ch >= 'a' && ch <= 'z') && !(ch >= 'A' && ch <= 'Z')
                && !(ch >= '0' && ch <= '9') && ch != '_' && ch != '-' && ch != '.') return false;
    }
    else if (e.kind == NetSoundKind::Message)
    {
        if (e.name.empty() || e.name.find('\0') != std::string::npos || e.volume > 100) return false;
        // Palette uses pitch and priority uses volume in the generic envelope.
        if (e.pitch > 255 || (e.volume != MESSAGE_PRIORITY_PICKUP && e.volume != MESSAGE_PRIORITY_NORMAL
            && e.volume != MESSAGE_PRIORITY_SECRET && e.volume != MESSAGE_PRIORITY_INI && e.volume != MESSAGE_PRIORITY_SYSTEM)) return false;
    }
    else if (!e.name.empty()) return false;
    return true;
}

std::vector<uint8_t> Encode(const SoundEvent &e)
{
    netwire::Writer w;
    w.u8(kSoundVersion); w.u8(uint8_t(e.kind)); w.u8(e.loop ? 1 : 0); w.u8(0);
    w.u32(uint32_t(e.sound)); w.u32(uint32_t(e.channel)); w.u32(uint32_t(e.flags));
    w.u32(uint32_t(e.pitch)); w.u32(uint32_t(e.volume));
    w.u32(uint32_t(e.x)); w.u32(uint32_t(e.y)); w.u32(uint32_t(e.z)); w.u32(uint32_t(e.sector));
    w.u32(uint32_t(e.spriteIndex)); w.u32(uint32_t(e.spriteType)); w.u32(uint32_t(e.spriteTile)); w.u32(uint32_t(e.spriteExtra));
    w.u32(e.command); w.u32(e.spriteGeneration); w.u32(uint32_t(e.target)); w.text(e.name, 240);
    return std::move(w.data);
}

void Send(const SoundEvent &e)
{
    if (gApplyingEvent || !netInServerTick() || !Valid(e)) return;
    Remember(e);
    auto const bytes = Encode(e);
    netBroadcastEvent(bytes.data(), bytes.size());
}

bool Read(const uint8_t *bytes, size_t size, SoundEvent &e)
{
    if (!bytes || size > 320) return false;
    netwire::Reader r(bytes, size);
    if (r.u8() != kSoundVersion) return false;
    e.kind = NetSoundKind(r.u8());
    unsigned const loop = r.u8(), reserved = r.u8();
    e.loop = loop != 0;
    e.sound = int32_t(r.u32()); e.channel = int32_t(r.u32()); e.flags = int32_t(r.u32());
    e.pitch = int32_t(r.u32()); e.volume = int32_t(r.u32());
    e.x = int32_t(r.u32()); e.y = int32_t(r.u32()); e.z = int32_t(r.u32()); e.sector = int32_t(r.u32());
    e.spriteIndex = int32_t(r.u32()); e.spriteType = int32_t(r.u32()); e.spriteTile = int32_t(r.u32()); e.spriteExtra = int32_t(r.u32());
    e.command = r.u32(); e.spriteGeneration = r.u32(); e.target = int32_t(r.u32()); e.name = r.text(240);
    return r.end() && loop <= 1 && reserved == 0 && Valid(e);
}
} // namespace

void netSoundPosition(int x, int y, int z, int sound, int sector)
{
    if (!netInServerTick() || gApplyingEvent) return;
    SoundEvent e;
    e.x = x; e.y = y; e.z = z; e.sound = sound; e.sector = sector;
    Send(e);
}

void netSoundSprite(NetSoundKind kind, int spriteIndex, int sound, int channel, int flags, int pitch, int volume)
{
    if (!netInServerTick() || gApplyingEvent || spriteIndex < 0 || spriteIndex >= kMaxSprites) return;
    auto const &spr = sprite[spriteIndex];
    SoundEvent e;
    e.kind = kind; e.spriteIndex = spriteIndex; e.sound = sound; e.channel = channel;
    e.flags = flags; e.pitch = pitch; e.volume = volume;
    e.x = spr.x; e.y = spr.y; e.z = spr.z; e.sector = spr.sectnum;
    e.spriteType = spr.type; e.spriteTile = spr.picnum; e.spriteExtra = spr.extra;
    e.spriteGeneration = netSnapshotSpriteGeneration(spriteIndex);
    if ((kind == NetSoundKind::Sprite || kind == NetSoundKind::SpriteCustom) && channel >= 0)
    {
        auto *resource = gSoundRes.Lookup(sound, "SFX");
        if (resource) e.loop = static_cast<SFX *>(gSoundRes.Load(resource))->loopStart >= 0;
    }
    for (int p = 0; p < kMaxPlayers; ++p)
        if (gPlayer[p].nSprite == spriteIndex) { e.command = netAppliedCommandSequence(p); break; }
    Send(e);
}

void netSoundSample(NetSoundKind kind, int sound, const char *name, int volume, int channel, bool loop)
{
    if (!netInServerTick() || gApplyingEvent) return;
    SoundEvent e;
    e.kind = kind; e.sound = sound; e.volume = volume; e.channel = channel; e.loop = loop;
    if (name) e.name = name;
    Send(e);
}

void netSoundStopAll(bool spatial)
{
    SoundEvent e;
    e.kind = spatial ? NetSoundKind::KillAll3D : NetSoundKind::KillAll2D;
    Send(e);
}

void netSoundForPlayer(int player, int sound, int volume, int channel, bool loop)
{
    if (netInServerTick())
    {
        SoundEvent e;
        e.kind = NetSoundKind::Sample; e.target = player; e.sound = sound;
        e.volume = volume; e.channel = channel; e.loop = loop;
        Send(e);
    }
    if (player != myconnectindex || !gMe) return;
    bool const saved = gApplyingEvent;
    gApplyingEvent = true;
    sndStartSample(unsigned(sound), volume, channel, loop);
    gApplyingEvent = saved;
}

void netMessageBroadcast(const char *text, int palette, int priority)
{
    if (!text || !netInServerTick() || gApplyingEvent) return;
    SoundEvent e;
    e.kind = NetSoundKind::Message; e.pitch = palette; e.volume = priority; e.name.assign(text, std::min<size_t>(240, std::strlen(text)));
    Send(e);
}

void netMessageForPlayer(int player, const char *text, int palette, int priority)
{
    if (!text) return;
    if (netInServerTick())
    {
        SoundEvent e;
        e.kind = NetSoundKind::Message; e.target = player; e.pitch = palette; e.volume = priority;
        e.name.assign(text, std::min<size_t>(240, std::strlen(text)));
        Send(e);
    }
    if (player != myconnectindex || !gMe) return;
    bool const saved = gApplyingEvent;
    gApplyingEvent = true;
    viewSetMessage(text, palette, MESSAGE_PRIORITY(priority));
    gApplyingEvent = saved;
}

bool netPlayEvent(const uint8_t *data, size_t size)
{
    SoundEvent e;
    if (!Read(data, size, e)) return false;
    if (e.target >= 0 && e.target != myconnectindex) return true;
    // Do not attach a late sound to a different actor that reused this sprite
    // slot. Positional fallback retains one-shot sounds after actor deletion.
    spritetype *spr = e.spriteIndex >= 0 ? &sprite[e.spriteIndex] : nullptr;
    if (spr && !MatchesSprite(e)) spr = nullptr;
    e.waitingForSprite = e.loop && e.spriteIndex >= 0 && !spr;
    Remember(e);
    if (e.waitingForSprite) return true;
    if ((e.kind == NetSoundKind::Sprite || e.kind == NetSoundKind::SpriteCustom)
        && !e.loop && netPredictSuppressSound(e.spriteIndex, e.sound, e.command, e.channel)) return true;
    gApplyingEvent = true;
    switch (e.kind)
    {
    case NetSoundKind::Position:
        sfxPlay3DSound(e.x, e.y, e.z, e.sound, e.sector); break;
    case NetSoundKind::Sprite:
        if (spr) sfxPlay3DSound(spr, e.sound, e.channel, e.flags);
        else sfxPlay3DSound(e.x, e.y, e.z, e.sound, e.sector);
        break;
    case NetSoundKind::SpriteCustom:
        if (spr) sfxPlay3DSoundCP(spr, e.sound, e.channel, e.flags, e.pitch, e.volume);
        else sfxPlay3DSound(e.x, e.y, e.z, e.sound, e.sector);
        break;
    case NetSoundKind::KillSpriteSound:
        if (spr) sfxKill3DSound(spr, e.channel, e.sound);
        break;
    case NetSoundKind::KillSpriteSounds:
        if (spr) sfxKillSpriteSounds(spr);
        break;
    case NetSoundKind::KillAll3D: sfxKillAllSounds(); break;
    case NetSoundKind::Sample: sndStartSample(unsigned(e.sound), e.volume, e.channel, e.loop); break;
    case NetSoundKind::NamedSample: sndStartSample(e.name.c_str(), e.volume, e.channel); break;
    case NetSoundKind::Wave: sndStartWavID(unsigned(e.sound), e.volume, e.channel); break;
    case NetSoundKind::KillAll2D: sndKillAllSounds(); break;
    case NetSoundKind::Message: viewSetMessage(e.name.c_str(), e.pitch, MESSAGE_PRIORITY(e.volume)); break;
    }
    gApplyingEvent = false;
    return true;
}

void netEventsReset() { gLoops.clear(); }

void netEventsOnSnapshot()
{
    // Reliable sound events can arrive before the snapshot that creates their
    // actor. Defer attached loops until the matching entity generation exists.
    std::vector<SoundEvent> ready;
    for (auto it = gLoops.begin(); it != gLoops.end();)
    {
        if (it->spriteIndex >= 0 && (netwire::newer(netSnapshotSpriteGeneration(it->spriteIndex), it->spriteGeneration)
            || (netSnapshotSpriteGeneration(it->spriteIndex) == it->spriteGeneration && sprite[it->spriteIndex].statnum >= kMaxStatus)))
        {
            it = gLoops.erase(it);
            continue;
        }
        if (it->waitingForSprite && MatchesSprite(*it))
        {
            it->waitingForSprite = false;
            ready.push_back(*it);
        }
        ++it;
    }
    for (auto const &event : ready)
    {
        auto bytes = Encode(event);
        netPlayEvent(bytes.data(), bytes.size());
    }
}

void netEventsBaseline(std::vector<std::vector<uint8_t>> &events)
{
    events.clear();
    for (auto it = gLoops.begin(); it != gLoops.end();)
    {
        if (it->spriteIndex >= 0)
        {
            if (MatchesSprite(*it))
            {
                auto const &spr = sprite[it->spriteIndex];
                it->x = spr.x; it->y = spr.y; it->z = spr.z; it->sector = spr.sectnum;
            }
            else if (!it->waitingForSprite || netwire::newer(netSnapshotSpriteGeneration(it->spriteIndex), it->spriteGeneration))
            {
                it = gLoops.erase(it);
                continue;
            }
        }
        events.push_back(Encode(*it));
        ++it;
    }
}
