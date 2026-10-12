// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>

// Call after resources, base ART and definitions are loaded, before any map.
// The cached fingerprint covers effective game content, never installation paths.
uint32_t netContentCRC();
