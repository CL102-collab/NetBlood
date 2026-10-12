// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <vector>
#include "netassets.h"

// Call after resources, base ART and definitions are loaded, before any map.
// The cached fingerprint covers effective game content, never installation paths.
uint32_t netContentCRC();

// Names, byte sizes and MD5s of the selected content behind the fingerprint.
// Semantic keys identify archive/resource slots independently of install paths.
const std::vector<netassets::Asset>& netContentManifest();
std::vector<netassets::Issue> netContentDifferences(const std::vector<netassets::Asset>& required);

// Register additional files successfully loaded by ART/DEF initialization.
// Tracking ends when netContentCRC freezes the effective startup content.
void netContentTrackFile(const char* path, bool searchMod = false);
void netContentTrackDefinition(const char* path);
