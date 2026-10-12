// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct GAMEOPTIONS;

// A canonical snapshot is an explicitly encoded client world, not a savegame
// or an image of native memory. Transport owns sequence/tick/command ACKs and
// identifies the exact acknowledged baseline used by a delta.
constexpr size_t kNetSnapshotMaxBytes = 16u * 1024u * 1024u;
constexpr uint16_t kNetSnapshotVersion = 1;

bool netSnapshotCapture(std::vector<uint8_t>& state, uint8_t playerMask, std::string& error);
bool netSnapshotValidate(const std::vector<uint8_t>& state, std::string& error);
bool netSnapshotApply(const std::vector<uint8_t>& state, int localSlot, std::string& error);
bool netSnapshotGetOptions(const std::vector<uint8_t>& state, GAMEOPTIONS& options,
                           uint8_t& playerMask, std::string& error);

// The full and delta forms both carry an output checksum. Deltas additionally
// verify baseline length/checksum and cannot silently use a different baseline.
// Encode chooses a delta only if it is smaller than the independently decodable
// full form. Passing nullptr always produces/accepts an independent full form.
bool netSnapshotPack(const std::vector<uint8_t>& state, const std::vector<uint8_t>* baseline,
                     std::vector<uint8_t>& wire, bool& usedDelta, std::string& error);
bool netSnapshotUnpack(const std::vector<uint8_t>& wire, const std::vector<uint8_t>* baseline,
                       std::vector<uint8_t>& state, std::string& error);
uint32_t netSnapshotHash(const std::vector<uint8_t>& state);

// Stable across snapshots, incremented for every insertion including reuse
// between two consecutive captures. Client render interpolation compares these.
void netSnapshotSpriteCreated(int index);
uint32_t netSnapshotSpriteGeneration(int index);
void netSnapshotResetGenerations();
