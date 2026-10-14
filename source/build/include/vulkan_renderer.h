// Native Vulkan submission for the shared Polymost scene frontend.
// Copyright (C) 2026 NBlood contributors. GPL-2.0-only.
#pragma once

#include "native_renderer.h"
#include <vector>

struct SDL_Window;

// Retain the Vulkan API names for backend callers and diagnostic tests.
using VulkanVertex = NativeVertex;
using VulkanUniforms = NativeUniforms;
using VulkanSamplerState = NativeSamplerState;
using VulkanDrawState = NativeDrawState;

constexpr unsigned kVulkanMaxLights = 8;

// Isotropic Build world coordinates: x, y, z / 16. RGB controls light color;
// color.w scales intensity without changing the light's range.
struct VulkanLight
{
    float positionRadius[4];
    float color[4];
};

struct VulkanShadowVertex
{
    float position[3];
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
// World-only screen-space ambient occlusion. The projection and viewport use
// Polymost's OpenGL conventions (positive eye Z, bottom-left viewport origin).
// Stencil bit 0x80 identifies eligible opaque world pixels. The pass multiplies
// RGB without modifying alpha or depth; no CPU readback or queue-idle wait.
bool vulkanAmbientOcclusion(const float projection[16], const int viewport[4],
                            float radius, float strength, float bias);
// World-only point lighting and cubemap shadows from opaque map geometry.
// eyeToWorld is column-major and maps the reconstructed Polymost eye position
// to isotropic world coordinates. Zero lights is an exact, allocation-free bypass.
bool vulkanRealtimeLighting(const float projection[16], const float eyeToWorld[16], const int viewport[4],
                            const VulkanLight *lights, unsigned lightCount,
                            const VulkanShadowVertex *shadowVertices, unsigned shadowVertexCount,
                            bool shadows = true);
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
