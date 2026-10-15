// Shared native submission interface for Polymost scene preparation.
// Copyright (C) 2026 NBlood contributors. GPL-2.0-only.
#pragma once
#include <stdint.h>
#include "renderer_types.h"
#include "realtime_lighting.h"

struct SDL_Window;

struct NativeVertex
{
    float position[4];
    float color[4];
    float texcoord[3][4]; // Polymost texture units 0, 3 and 4.
};

// Float4-aligned layout shared by the Vulkan and Metal scene shaders.
struct NativeUniforms
{
    float modelView[16];
    float projection[16]; // Polymost/Duke64 modes 1/4: projection * modelView; otherwise projection.
    float textureMatrix[3][16];
    float rotation[16];
    float texturePosSize[4];
    float halfTexelPalswapPos[4];
    float palswapSizeClamp[4];
    float shadeNumVis[4];
    float fog[4]; // enabled, end, scale, alpha cutoff
    float fogColor[4];
    float effects[4]; // color only, palette, detail, glow; Duke64: combiner color 1
    float npot[4]; // Duke64: combiner color 2
    float correction[4];
    float misc[4]; // brightness (Duke64: combiner), shade interpolation, alpha test, program mode
    float fixed[4]; // texture enabled, fog enabled, alpha comparison, fog mode
};

struct NativeSamplerState
{
    uint32_t texture;
    uint32_t minFilter, magFilter, wrapS, wrapT;
    float anisotropy;
    float minLod, maxLod;
};

// State enumerants use the shared frontend's GL numeric constants, but are
// translated into native Vulkan/Metal state without calling an OpenGL driver.
struct NativeDrawState
{
    NativeUniforms uniforms;
    NativeSamplerState samplers[5];
    int viewport[4], scissor[4];
    uint32_t topology, depthFunc, blendSrc, blendDst, blendEquation;
    uint32_t cullFace, frontFace, polygonMode;
    uint32_t stencilFunc, stencilFail, stencilDepthFail, stencilPass;
    uint32_t stencilReference, stencilCompareMask, stencilWriteMask;
    float depthRange[2], depthBiasFactor, depthBiasUnits, lineWidth;
    bool depthTest, depthWrite, blend, cull, scissorTest, stencilTest, depthBias, depthClamp;
    bool colorWrite[4];
};

static_assert(sizeof(NativeVertex) == 80, "Native vertex shader ABI");
static_assert(sizeof(NativeUniforms) == 560, "Native uniform shader ABI");

#if defined(USE_VULKAN) || defined(USE_METAL)
bool nativeCreate(int renderer, SDL_Window *window, int width, int height, int vsync);
void nativeDestroy();
bool nativeActive();
int nativeRendererMode();
bool nativeSupported(int renderer);
bool nativeResize(int width, int height);
void nativeSetVsync(int vsync);
bool nativePresent();
void nativeFinish();
void nativeFlush();
// World-only AO uses the frontend's OpenGL projection and bottom-left viewport.
// Stencil bit 0x80 masks eligible world pixels; alpha/depth remain unchanged.
bool nativeAmbientOcclusion(const float projection[16], const int viewport[4],
                            float radius, float strength, float bias);
bool nativeRealtimeLighting(const float projection[16], const float eyeToWorld[16], const int viewport[4],
                            const RealtimeLight *lights, unsigned lightCount,
                            const RealtimeShadowVertex *shadowVertices, unsigned shadowVertexCount,
                            bool shadows = true);
void nativeDraw(const NativeVertex *vertices, unsigned count, const NativeDrawState &state);
void nativeClear(uint32_t mask, const float color[4], float depth, uint32_t stencil,
                 const NativeDrawState &state);
void nativeUploadTexture(uint32_t texture, int level, int width, int height, const uint8_t *rgba);
void nativeUploadTextureRegion(uint32_t texture, int level, int x, int y, int width, int height, const uint8_t *rgba);
void nativeDeleteTexture(uint32_t texture);
bool nativeReadPixels(int x, int y, int width, int height, uint8_t *rgba);
const char *nativeDeviceName();
int nativeMaxTextureSize();
float nativeMaxAnisotropy();
bool nativeDepthClampSupported();

// Frontend owns virtual texture/buffer IDs and material state without a GL context.
void nativeFrontendInstall(int width, int height);
void nativeFrontendUninstall();
void nativeFrontendResize(int width, int height);
#else
static inline bool nativeActive() { return false; }
static inline int nativeRendererMode() { return REND_CLASSIC; }
static inline bool nativeSupported(int) { return false; }
#endif
