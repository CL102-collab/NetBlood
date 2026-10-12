// NBlood authoritative presentation events. SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

// Fixed resource IDs and sprite indices only: no pointers, paths to arbitrary
// disk files, executable callbacks, or client-originated world actions.
enum class NetSoundKind : uint8_t
{
    Position = 1, Sprite, SpriteCustom, KillSpriteSound, KillSpriteSounds,
    KillAll3D, Sample, NamedSample, Wave, KillAll2D, Message
};

void netSoundPosition(int x, int y, int z, int sound, int sector);
void netSoundSprite(NetSoundKind kind, int spriteIndex, int sound = -1,
                    int channel = -1, int flags = 0, int pitch = 0, int volume = 0);
void netSoundSample(NetSoundKind kind, int sound, const char *name,
                    int volume, int channel, bool loop = false);
void netSoundStopAll(bool spatial);
void netSoundForPlayer(int player, int sound, int volume, int channel, bool loop = false);
// Broadcast is a viewSetMessage hook: it emits only during a server tick and
// does not draw locally. The targeted wrapper handles local drawing itself.
void netMessageBroadcast(const char *text, int palette = 0, int priority = 0);
void netMessageForPlayer(int player, const char *text, int palette = 0, int priority = 0);
bool netPlayEvent(const uint8_t *data, size_t size);
void netEventsReset();
void netEventsOnSnapshot();
void netEventsBaseline(std::vector<std::vector<uint8_t>> &events);
