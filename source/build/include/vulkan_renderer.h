// Native Vulkan submission for the shared Polymost scene frontend.
// Copyright (C) 2026 NBlood contributors. GPL-2.0-only.
#pragma once

#include <stdint.h>
#include <vector>

struct SDL_Window;

struct VulkanVertex
{
    float position[4];
    float color[4];
    float texcoord[3][4]; // Polymost texture units 0, 3 and 4.
};

// vec4-aligned layout shared with vulkan_polymost.vert/.frag (std140).
struct VulkanUniforms
{
    float modelView[16];
    float projection[16]; // Polymost mode 1: precomposed projection * modelView; otherwise projection.
    float textureMatrix[3][16];
    float rotation[16];
    float texturePosSize[4];
    float halfTexelPalswapPos[4];
    float palswapSizeClamp[4];
    float shadeNumVis[4];
    float fog[4]; // enabled, end, scale, alpha cutoff
    float fogColor[4];
    float effects[4]; // color only, palette, detail, glow
    float npot[4];
    float correction[4];
    float misc[4]; // brightness, shade interpolation, alpha test, program mode
    float fixed[4]; // texture enabled, fog enabled, alpha comparison, fog mode
};

struct VulkanSamplerState
{
    uint32_t texture;
    uint32_t minFilter, magFilter, wrapS, wrapT;
    float anisotropy;
    float minLod, maxLod;
};

// State enumerants use the shared frontend's GL numeric constants, but are
// translated into native VkPipeline state without calling an OpenGL driver.
struct VulkanDrawState
{
    VulkanUniforms uniforms;
    VulkanSamplerState samplers[5];
    int viewport[4], scissor[4];
    uint32_t topology, depthFunc, blendSrc, blendDst, blendEquation;
    uint32_t cullFace, frontFace, polygonMode;
    uint32_t stencilFunc, stencilFail, stencilDepthFail, stencilPass;
    uint32_t stencilReference, stencilCompareMask, stencilWriteMask;
    float depthRange[2], depthBiasFactor, depthBiasUnits, lineWidth;
    bool depthTest, depthWrite, blend, cull, scissorTest, stencilTest, depthBias, depthClamp;
    bool colorWrite[4];
};

// Cumulative CPU recording diagnostics for the current backend lifetime.
// Call vulkanFinish first to include draws still waiting in the current batch.
// workerCount and NBLOOD_VULKAN_WORKERS both count TOTAL recorders, including
// caller slot 0. The automatic limit is one caller plus at most four background
// threads, bounded by available cores. The override can lower that limit; 1
// records synchronously on the caller and never starts a background thread.
struct VulkanRecordingStats
{
    uint32_t workerCount, startedWorkers, peakActiveRecorders;
    uint32_t backgroundWorkerCount, startedBackgroundWorkers;
    uint64_t callerThreadId;
    uint64_t batches, parallelBatches, callerParallelBatches, draws, secondaryBuffers, dispatchNanoseconds;
    // Slot 0 is the caller. Only slots actually used by a batch have entries.
    std::vector<uint64_t> workerDraws, workerThreadIds, workerRecordNanoseconds;
};

// Submission slots bound CPU/GPU resource reuse; they do not imply that the GPU
// rasterizes multiple frames simultaneously. Fence observations are diagnostics.
struct VulkanSubmissionStats
{
    uint32_t framesInFlight, pendingSubmissions, maxPendingSubmissions, maxObservedUnsignaled;
    uint32_t pendingRetiredImages;
    uint64_t submissions, completedSubmissions, presents;
    uint64_t fenceWaits, slotReuseWaits, finishWaits, readbackWaits;
    uint64_t deviceIdleWaits, queueIdleWaits, retiredImages;
};

#ifdef USE_VULKAN
bool vulkanCreate(SDL_Window *window, int width, int height, int vsync);
void vulkanDestroy();
bool vulkanActive();
bool vulkanSupported();
bool vulkanResize(int width, int height);
void vulkanSetVsync(int vsync);
bool vulkanPresent();
void vulkanFinish();
void vulkanFlush();
void vulkanDraw(const VulkanVertex *vertices, unsigned count, const VulkanDrawState &state);
void vulkanClear(uint32_t mask, const float color[4], float depth, uint32_t stencil,
                 const VulkanDrawState &state);
void vulkanUploadTexture(uint32_t texture, int level, int width, int height, const uint8_t *rgba);
void vulkanUploadTextureRegion(uint32_t texture, int level, int x, int y, int width, int height, const uint8_t *rgba);
void vulkanDeleteTexture(uint32_t texture);
bool vulkanReadPixels(int x, int y, int width, int height, uint8_t *rgba);
const char *vulkanDeviceName();
int vulkanMaxTextureSize();
float vulkanMaxAnisotropy();
bool vulkanDepthClampSupported();
void vulkanGetRecordingStats(VulkanRecordingStats *stats);
void vulkanGetSubmissionStats(VulkanSubmissionStats *stats);
unsigned vulkanAvailableRecordingCores();

// The frontend adapter owns virtual texture/buffer IDs and material state.
// Installing it redirects only this process's dispatch, with no GL context.
void vulkanFrontendInstall(int width, int height);
void vulkanFrontendUninstall();
void vulkanFrontendResize(int width, int height);
#else
static inline bool vulkanActive() { return false; }
static inline bool vulkanSupported() { return false; }
#endif
