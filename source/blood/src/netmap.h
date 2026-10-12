// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace netmap { constexpr size_t MaxBytes = 16u * 1024u * 1024u; }

// These helpers never call the fatal legacy map parser on unverified bytes.
// Read is for the server's configured map (paths are permitted). Client
// descriptors must use Prepare, which never probes absolute/traversing paths.
bool netMapRead(const char* name, std::vector<uint8_t>& bytes, std::string& error);
bool netMapValidate(const std::vector<uint8_t>& bytes, uint32_t expectedCRC, std::string& error);
bool netMapPrepare(const char* name, uint32_t expectedCRC, std::string& error);
bool netMapInstall(const char* name, uint32_t expectedCRC, const std::vector<uint8_t>& bytes, std::string& error);
void netMapClear();
// Alias only the currently selected network map, leaving canonical game/demo
// names and ordinary resource search paths untouched. Unmatched paths pass through.
const char* netMapResolve(const char* path);
