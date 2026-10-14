// Build world geometry used by native Vulkan point-light shadow maps.
// Copyright (C) 2026 NBlood contributors. GPL-2.0-only.
#pragma once

#include "vulkan_renderer.h"

#ifdef USE_VULKAN
// Positions and light radii use Build X/Y units, with Z divided by 16.
// Replaces vertices. Reads the current map without changing engine state.
void vulkanBuildShadowMesh(const VulkanLight *lights, unsigned lightCount,
                           std::vector<VulkanShadowVertex> &vertices);
#endif
