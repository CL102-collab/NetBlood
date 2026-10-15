// Render-only world lighting shared by the Build and Duke64 views.
// Copyright (C) 2026 Rednukem contributors. GPL-2.0-only.
#pragma once

#ifdef USE_OPENGL
#include "realtime_lighting.h"
#include "vec.h"

unsigned G_CollectRealtimeLights(RealtimeLight *lights, vec3_t const &viewPos, int32_t smoothRatio);
void G_RecordWeaponLight(int playerNum, int lightColor);
#endif
