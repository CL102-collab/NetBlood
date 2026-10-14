// Shared point lights and world geometry for Polymost and Vulkan shadows.
// Copyright (C) 2026 NBlood contributors. GPL-2.0-only.
#pragma once

#include <vector>

constexpr unsigned kMaxRealtimeLights = 8;

// Isotropic Build world coordinates: x, y, z / 16. RGB controls light color;
// color.w scales intensity without changing the light's range.
struct RealtimeLight
{
    float positionRadius[4];
    float color[4];
};

struct RealtimeShadowVertex
{
    float position[3];
};

#ifdef USE_OPENGL
// Positions and light radii use Build X/Y units, with Z divided by 16.
// Replaces vertices. Reads the current map without changing engine state.
void buildRealtimeShadowMesh(const RealtimeLight *lights, unsigned lightCount,
                             std::vector<RealtimeShadowVertex> &vertices);
#endif
