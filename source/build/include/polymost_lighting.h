// Native OpenGL Polymost point lighting and world geometry shadow maps.
// Copyright (C) 2026 NBlood contributors. GPL-2.0-only.
#pragma once

#include "realtime_lighting.h"

bool polymostRealtimeLighting(const float projection[16], const float eyeToWorld[16], const int viewport[4],
                              const RealtimeLight *lights, unsigned lightCount,
                              const RealtimeShadowVertex *shadowVertices, unsigned shadowVertexCount,
                              bool shadows = true);
// Optimistic until the first desktop allocation attempt; false after an
// unsupported configuration or resource failure, and always false on GLES.
bool polymostRealtimeLightingAvailable();
// Call while the owning OpenGL context is current, before its destruction.
void polymostResetRealtimeLighting();
