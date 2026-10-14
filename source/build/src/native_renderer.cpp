// Runtime routing for the shared Polymost native frontend.
// Copyright (C) 2026 NBlood contributors. GPL-2.0-only.
#include "native_renderer.h"
#if defined(USE_VULKAN) || defined(USE_METAL)
#include <SDL.h>
#ifdef USE_VULKAN
#include "vulkan_renderer.h"
#endif
#ifdef USE_METAL
#include "metal_renderer.h"
#endif

int nativeRendererMode()
{
#ifdef USE_METAL
    if (metalActive()) return REND_METAL;
#endif
#ifdef USE_VULKAN
    if (vulkanActive()) return REND_VULKAN;
#endif
    return REND_CLASSIC;
}

bool nativeActive() { return nativeRendererMode() != REND_CLASSIC; }

bool nativeSupported(int renderer)
{
    switch (renderer)
    {
#ifdef USE_METAL
        case REND_METAL: return metalSupported();
#endif
#ifdef USE_VULKAN
        case REND_VULKAN: return vulkanSupported();
#endif
        default: return false;
    }
}

void nativeDestroy()
{
#ifdef USE_METAL
    metalDestroy();
#endif
#ifdef USE_VULKAN
    vulkanDestroy();
#endif
}

bool nativeCreate(int renderer, SDL_Window *window, int width, int height, int vsync)
{
    nativeDestroy();
    switch (renderer)
    {
#ifdef USE_METAL
        case REND_METAL: return metalCreate(window, width, height, vsync);
#endif
#ifdef USE_VULKAN
        case REND_VULKAN: return vulkanCreate(window, width, height, vsync);
#endif
        default: SDL_SetError("The requested native renderer is unavailable in this build."); return false;
    }
}

bool nativeResize(int width, int height)
{
#ifdef USE_METAL
    if (metalActive()) return metalResize(width, height);
#endif
#ifdef USE_VULKAN
    if (vulkanActive()) return vulkanResize(width, height);
#endif
    return false;
}

void nativeSetVsync(int vsync)
{
#ifdef USE_METAL
    if (metalActive()) return metalSetVsync(vsync);
#endif
#ifdef USE_VULKAN
    if (vulkanActive()) return vulkanSetVsync(vsync);
#endif
}

bool nativePresent()
{
#ifdef USE_METAL
    if (metalActive()) return metalPresent();
#endif
#ifdef USE_VULKAN
    if (vulkanActive()) return vulkanPresent();
#endif
    return false;
}

void nativeFinish()
{
#ifdef USE_METAL
    if (metalActive()) return metalFinish();
#endif
#ifdef USE_VULKAN
    if (vulkanActive()) return vulkanFinish();
#endif
}

void nativeFlush()
{
#ifdef USE_METAL
    if (metalActive()) return metalFlush();
#endif
#ifdef USE_VULKAN
    if (vulkanActive()) return vulkanFlush();
#endif
}

bool nativeAmbientOcclusion(const float projection[16], const int viewport[4], float radius, float strength, float bias)
{
#ifdef USE_METAL
    if (metalActive()) return metalAmbientOcclusion(projection, viewport, radius, strength, bias);
#endif
#ifdef USE_VULKAN
    if (vulkanActive()) return vulkanAmbientOcclusion(projection, viewport, radius, strength, bias);
#endif
    return false;
}

bool nativeRealtimeLighting(const float projection[16], const float eyeToWorld[16], const int viewport[4],
                            const RealtimeLight *lights, unsigned lightCount,
                            const RealtimeShadowVertex *shadowVertices, unsigned shadowVertexCount, bool shadows)
{
#ifdef USE_VULKAN
    if (vulkanActive()) return vulkanRealtimeLighting(projection, eyeToWorld, viewport, lights, lightCount,
                                                     shadowVertices, shadowVertexCount, shadows);
#else
    (void)projection; (void)eyeToWorld; (void)viewport; (void)lights; (void)lightCount;
    (void)shadowVertices; (void)shadowVertexCount; (void)shadows;
#endif
    return false;
}

void nativeDraw(const NativeVertex *vertices, unsigned count, const NativeDrawState &state)
{
#ifdef USE_METAL
    if (metalActive()) return metalDraw(vertices, count, state);
#endif
#ifdef USE_VULKAN
    if (vulkanActive()) return vulkanDraw(vertices, count, state);
#endif
}

void nativeClear(uint32_t mask, const float color[4], float depth, uint32_t stencil, const NativeDrawState &state)
{
#ifdef USE_METAL
    if (metalActive()) return metalClear(mask, color, depth, stencil, state);
#endif
#ifdef USE_VULKAN
    if (vulkanActive()) return vulkanClear(mask, color, depth, stencil, state);
#endif
}

void nativeUploadTexture(uint32_t texture, int level, int width, int height, const uint8_t *rgba)
{
#ifdef USE_METAL
    if (metalActive()) return metalUploadTexture(texture, level, width, height, rgba);
#endif
#ifdef USE_VULKAN
    if (vulkanActive()) return vulkanUploadTexture(texture, level, width, height, rgba);
#endif
}

void nativeUploadTextureRegion(uint32_t texture, int level, int x, int y, int width, int height, const uint8_t *rgba)
{
#ifdef USE_METAL
    if (metalActive()) return metalUploadTextureRegion(texture, level, x, y, width, height, rgba);
#endif
#ifdef USE_VULKAN
    if (vulkanActive()) return vulkanUploadTextureRegion(texture, level, x, y, width, height, rgba);
#endif
}

void nativeDeleteTexture(uint32_t texture)
{
#ifdef USE_METAL
    if (metalActive()) return metalDeleteTexture(texture);
#endif
#ifdef USE_VULKAN
    if (vulkanActive()) return vulkanDeleteTexture(texture);
#endif
}

bool nativeReadPixels(int x, int y, int width, int height, uint8_t *rgba)
{
#ifdef USE_METAL
    if (metalActive()) return metalReadPixels(x, y, width, height, rgba);
#endif
#ifdef USE_VULKAN
    if (vulkanActive()) return vulkanReadPixels(x, y, width, height, rgba);
#endif
    return false;
}

const char * nativeDeviceName()
{
#ifdef USE_METAL
    if (metalActive()) return metalDeviceName();
#endif
#ifdef USE_VULKAN
    if (vulkanActive()) return vulkanDeviceName();
#endif
    return "No native renderer";
}

int nativeMaxTextureSize()
{
#ifdef USE_METAL
    if (metalActive()) return metalMaxTextureSize();
#endif
#ifdef USE_VULKAN
    if (vulkanActive()) return vulkanMaxTextureSize();
#endif
    return 0;
}

float nativeMaxAnisotropy()
{
#ifdef USE_METAL
    if (metalActive()) return metalMaxAnisotropy();
#endif
#ifdef USE_VULKAN
    if (vulkanActive()) return vulkanMaxAnisotropy();
#endif
    return 1.f;
}

bool nativeDepthClampSupported()
{
#ifdef USE_METAL
    if (metalActive()) return metalDepthClampSupported();
#endif
#ifdef USE_VULKAN
    if (vulkanActive()) return vulkanDepthClampSupported();
#endif
    return false;
}

#endif // USE_VULKAN || USE_METAL
