// Native Metal submission for the shared Polymost scene frontend.
// Copyright (C) 2026 NBlood contributors. GPL-2.0-only.
#pragma once

#include "native_renderer.h"

struct SDL_Window;

#ifdef USE_METAL
bool metalCreate(SDL_Window *window, int width, int height, int vsync);
void metalDestroy();
bool metalActive();
bool metalSupported();
bool metalResize(int width, int height);
void metalSetVsync(int vsync);
bool metalPresent();
void metalFinish();
void metalFlush();
bool metalAmbientOcclusion(const float projection[16], const int viewport[4],
                           float radius, float strength, float bias);
void metalDraw(const NativeVertex *vertices, unsigned count, const NativeDrawState &state);
void metalClear(uint32_t mask, const float color[4], float depth, uint32_t stencil,
                const NativeDrawState &state);
void metalUploadTexture(uint32_t texture, int level, int width, int height, const uint8_t *rgba);
void metalUploadTextureRegion(uint32_t texture, int level, int x, int y, int width, int height, const uint8_t *rgba);
void metalDeleteTexture(uint32_t texture);
bool metalReadPixels(int x, int y, int width, int height, uint8_t *rgba);
const char *metalDeviceName();
int metalMaxTextureSize();
float metalMaxAnisotropy();
bool metalDepthClampSupported();
#else
static inline bool metalActive() { return false; }
static inline bool metalSupported() { return false; }
#endif
