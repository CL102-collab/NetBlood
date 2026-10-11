// Native Vulkan backend regression checks. No original game data is required.
// SPDX-License-Identifier: GPL-2.0-only
#include "vulkan_renderer.h"
#include <SDL.h>
#include <glad/glad.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#ifdef __linux__
#include <sched.h>
#endif
#ifdef VULKAN_BACKEND_TEST_WRAP
#include <atomic>
#include <vulkan/vulkan.h>

namespace {
std::atomic<unsigned> injectedFailure(0);
std::atomic<uint64_t> failureCaller(0);
std::atomic<bool> failureObserved(false);
std::atomic<unsigned> backgroundGateReached(0);
std::atomic<unsigned> backgroundGateHeld(0);
std::atomic<bool> releaseBackgroundGate(false);

bool waitForBackgroundGate(bool waitForRelease)
{
    uint32_t const started = SDL_GetTicks();
    while (waitForRelease ? !releaseBackgroundGate.load() : backgroundGateHeld.load() == 0)
    {
        if (SDL_GetTicks() - started > 5000) return false;
        SDL_Delay(1);
    }
    return true;
}

bool injectWorkerFailure(unsigned kind)
{
    if (static_cast<uint64_t>(SDL_ThreadID()) == failureCaller.load()) return false;
    unsigned expected = kind;
    if (!injectedFailure.compare_exchange_strong(expected, 0)) return false;
    failureObserved.store(true);
    return true;
}
}

// Linux test-linker wrappers inject real worker error returns without adding
// failure controls to the renderer. All other calls reach the Vulkan loader.
extern "C" VKAPI_ATTR VkResult VKAPI_CALL __real_vkBeginCommandBuffer(
    VkCommandBuffer command, VkCommandBufferBeginInfo const *info);
extern "C" VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkBeginCommandBuffer(
    VkCommandBuffer command, VkCommandBufferBeginInfo const *info)
{
    if (info->flags & VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT)
    {
        if (injectedFailure.load() == 3)
        {
            if (static_cast<uint64_t>(SDL_ThreadID()) == failureCaller.load())
            {
                // Do not fail the caller until a background thread has really
                // begun a secondary command buffer and is still recording it.
                bool const backgroundActive = waitForBackgroundGate(false);
                injectedFailure.store(0);
                failureObserved.store(backgroundActive);
                releaseBackgroundGate.store(true);
                return VK_ERROR_OUT_OF_HOST_MEMORY;
            }
            VkResult const result = __real_vkBeginCommandBuffer(command, info);
            if (result == VK_SUCCESS)
            {
                backgroundGateHeld.fetch_add(1);
                backgroundGateReached.fetch_add(1);
                waitForBackgroundGate(true);
                backgroundGateHeld.fetch_sub(1);
            }
            return result;
        }
        if (injectWorkerFailure(1)) return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    return __real_vkBeginCommandBuffer(command, info);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL __real_vkAllocateDescriptorSets(
    VkDevice device, VkDescriptorSetAllocateInfo const *info, VkDescriptorSet *sets);
extern "C" VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkAllocateDescriptorSets(
    VkDevice device, VkDescriptorSetAllocateInfo const *info, VkDescriptorSet *sets)
{
    if (injectWorkerFailure(2)) return VK_ERROR_OUT_OF_HOST_MEMORY;
    return __real_vkAllocateDescriptorSets(device, info, sets);
}
#endif

namespace {
struct ReferenceImage
{
    std::string label;
    std::vector<unsigned char> rgba;
};

std::vector<ReferenceImage> referenceImages;
size_t comparisonIndex = 0;
bool recordReference = true;

unsigned recordingWorkerLimit(unsigned cores)
{
    return cores > 5 ? 5 : (cores ? cores : 1);
}

VulkanDrawState defaults(int width = 64, int height = 64)
{
    VulkanDrawState state = {};
    state.viewport[2] = width;
    state.viewport[3] = height;
    state.topology = GL_TRIANGLES;
    state.depthFunc = GL_LESS;
    state.depthRange[1] = 1.f;
    state.depthWrite = true;
    state.blendSrc = GL_SRC_ALPHA;
    state.blendDst = GL_ONE_MINUS_SRC_ALPHA;
    state.blendEquation = GL_FUNC_ADD;
    state.frontFace = GL_CCW;
    state.cullFace = GL_BACK;
    state.polygonMode = GL_FILL;
    state.stencilFunc = GL_ALWAYS;
    state.stencilFail = state.stencilDepthFail = state.stencilPass = GL_KEEP;
    state.stencilCompareMask = state.stencilWriteMask = 255;
    state.lineWidth = 1.f;
    for (unsigned i = 0; i < 4; ++i)
    {
        state.colorWrite[i] = true;
        state.uniforms.modelView[i * 5] = state.uniforms.projection[i * 5] = state.uniforms.rotation[i * 5] = 1.f;
        for (unsigned j = 0; j < 3; ++j) state.uniforms.textureMatrix[j][i * 5] = 1.f;
    }
    for (auto &sample : state.samplers)
    {
        sample.minFilter = sample.magFilter = GL_NEAREST;
        sample.wrapS = sample.wrapT = GL_CLAMP_TO_EDGE;
        sample.anisotropy = 1.f;
        sample.maxLod = 1000.f;
    }
    return state;
}

void require(bool condition, char const *message)
{
    if (!condition)
    {
        std::fprintf(stderr, "FAIL: %s\n", message);
        vulkanDestroy();
        SDL_Quit();
        std::exit(1);
    }
}

void triangle(VulkanDrawState const &state, float r, float g, float b, float a = 1.f, float z = 0.f)
{
    VulkanVertex vertices[3] = {};
    float const positions[3][2] = { { -1.f, -1.f }, { 3.f, -1.f }, { -1.f, 3.f } };
    for (unsigned i = 0; i < 3; ++i)
    {
        vertices[i].position[0] = positions[i][0];
        vertices[i].position[1] = positions[i][1];
        vertices[i].position[2] = z;
        vertices[i].position[3] = 1.f;
        vertices[i].color[0] = r;
        vertices[i].color[1] = g;
        vertices[i].color[2] = b;
        vertices[i].color[3] = a;
        vertices[i].texcoord[0][0] = (positions[i][0] + 1.f) * 0.5f;
        vertices[i].texcoord[0][1] = (positions[i][1] + 1.f) * 0.5f;
        vertices[i].texcoord[0][3] = 1.f;
    }
    vulkanDraw(vertices, 3, state);
    require(vulkanActive(), "draw must leave the renderer active");
}

void pixel(char const *label, int r, int g, int b, int x = 32, int y = 32)
{
    unsigned char rgba[4] = {};
    require(vulkanReadPixels(x, y, 1, 1, rgba), "read back rendered pixel");
    if (std::abs(static_cast<int>(rgba[0]) - r) > 2 || std::abs(static_cast<int>(rgba[1]) - g) > 2 ||
        std::abs(static_cast<int>(rgba[2]) - b) > 2)
    {
        std::fprintf(stderr, "%s: expected RGB %d %d %d; actual RGBA %u %u %u %u\n",
                     label, r, g, b, rgba[0], rgba[1], rgba[2], rgba[3]);
        require(false, label);
    }
    std::printf("PASS: %s\n", label);
}

// Compare every channel, including alpha, against the same sequence recorded
// with one worker. A few sampled pixels would miss reordered transparent draws
// or a draw packet accidentally retaining a caller's mutable state.
void compareImage(char const *label, int width = 64, int height = 64)
{
    std::vector<unsigned char> rgba(static_cast<size_t>(width) * height * 4);
    require(vulkanReadPixels(0, 0, width, height, rgba.data()), "read complete comparison image");
    if (recordReference)
    {
        referenceImages.push_back({ label, rgba });
        std::printf("REFERENCE: %s\n", label);
        return;
    }
    require(comparisonIndex < referenceImages.size(), "matching reference image exists");
    ReferenceImage const &reference = referenceImages[comparisonIndex++];
    require(reference.label == label, "comparison checkpoints retain their order");
    require(reference.rgba.size() == rgba.size(), "comparison image dimensions agree");
    if (rgba != reference.rgba)
    {
        size_t offset = 0;
        while (rgba[offset] == reference.rgba[offset]) ++offset;
        std::fprintf(stderr, "%s: pixel (%zu,%zu) channel %zu differs: single=%u parallel=%u\n",
                     label, offset / 4 % width, offset / 4 / width, offset % 4,
                     reference.rgba[offset], rgba[offset]);
        require(false, "single-worker and parallel images must match exactly");
    }
    std::printf("PASS: exact single/parallel image match: %s\n", label);
}

void orderingAndBarriers()
{
    VulkanDrawState state = defaults();
    float const black[] = { 0.f, 0.f, 0.f, 1.f };
    float const blue[] = { 0.f, 0.f, 1.f, 1.f };
    vulkanClear(GL_COLOR_BUFFER_BIT, black, 1.f, 0, state);
    state.blend = state.scissorTest = true;
    state.scissor[2] = state.scissor[3] = 4;
    // Each pass touches all 256 tiles. Different overlapping alpha colors make
    // the original command order observable across worker chunk boundaries.
    for (unsigned pass = 0; pass < 12; ++pass)
        for (unsigned tile = 0; tile < 256; ++tile)
        {
            state.scissor[0] = (tile % 16) * 4;
            state.scissor[1] = (tile / 16) * 4;
            triangle(state, ((pass + tile) % 7) / 6.f, ((pass * 3 + tile) % 5) / 4.f,
                     ((pass * 5 + tile) % 11) / 10.f, 0.25f);
        }
    pixel("transparent painter order", 118, 124, 88, 1, 1);
    compareImage("overlapping transparent draws and mutable per-draw state");

    state = defaults();
    vulkanClear(GL_COLOR_BUFFER_BIT, black, 1.f, 0, state);
    unsigned char const red[] = { 255, 0, 0, 255 };
    unsigned char const green[] = { 0, 255, 0, 255 };
    unsigned char const yellow[] = { 255, 255, 0, 255 };
    vulkanUploadTexture(7, 0, 1, 1, red);
    state.samplers[0].texture = 7;
    state.uniforms.fixed[0] = 1.f;
    state.scissorTest = true;
    state.scissor[2] = 16;
    state.scissor[3] = 64;
    for (unsigned i = 0; i < 512; ++i) triangle(state, 1.f, 1.f, 1.f);
    // Uploading the same image must wait for the queued red draws, even though
    // their descriptor refers to the image that is about to become green.
    vulkanUploadTextureRegion(7, 0, 0, 0, 1, 1, green);
    state.scissor[0] = 16;
    for (unsigned i = 0; i < 512; ++i) triangle(state, 1.f, 1.f, 1.f);
    state.scissor[0] = 32;
    vulkanClear(GL_COLOR_BUFFER_BIT, blue, 1.f, 0, state);
    // Destroy/reuse of a texture ID must preserve already queued descriptors.
    vulkanDeleteTexture(7);
    vulkanUploadTexture(7, 0, 1, 1, yellow);
    state.scissor[0] = 48;
    for (unsigned i = 0; i < 512; ++i) triangle(state, 1.f, 1.f, 1.f);
    pixel("queued draws before texture mutation", 255, 0, 0, 8, 32);
    pixel("queued draws before texture deletion", 0, 255, 0, 24, 32);
    pixel("clear ordered between draw batches", 0, 0, 255, 40, 32);
    pixel("texture ID reuse after queued draws", 255, 255, 0, 56, 32);
    compareImage("texture uploads, deletion, clear and readback boundaries");
    // Start a new batch after the readback drained the prior workers.
    state.uniforms.fixed[0] = 0.f;
    state.blend = true;
    for (unsigned i = 0; i < 512; ++i) triangle(state, 1.f, 0.f, 0.f, 0.125f);
    compareImage("draws resume after readback");
    vulkanDeleteTexture(7);
}

void streamRollover()
{
    VulkanDrawState state = defaults();
    float const black[] = { 0.f, 0.f, 0.f, 1.f };
    vulkanClear(GL_COLOR_BUFFER_BIT, black, 1.f, 0, state);
    state.scissorTest = true;
    state.scissor[2] = state.scissor[3] = 1;
    // More than 64 MiB of vertices without a present/readback exercises stream
    // retirement independently of descriptor exhaustion. Only one triangle in
    // each packet covers a pixel; the rest are valid degenerate triangles.
    std::vector<VulkanVertex> vertices(12288);
    for (auto &vertex : vertices) vertex.position[3] = 1.f;
    vertices[0].position[0] = vertices[0].position[1] = -1.f;
    vertices[1].position[0] = 3.f;
    vertices[1].position[1] = -1.f;
    vertices[2].position[0] = -1.f;
    vertices[2].position[1] = 3.f;
    for (unsigned draw = 0; draw < 80; ++draw)
    {
        state.scissor[0] = draw % 64;
        state.scissor[1] = draw / 64;
        for (unsigned i = 0; i < 3; ++i)
        {
            vertices[i].color[0] = float((draw + 1) & 1);
            vertices[i].color[1] = float(((draw + 1) >> 1) & 1);
            vertices[i].color[2] = float(((draw + 1) >> 2) & 1);
            vertices[i].color[3] = 1.f;
        }
        vulkanDraw(vertices.data(), static_cast<unsigned>(vertices.size()), state);
    }
    pixel("stream rollover preserves first packet", 255, 0, 0, 0, 0);
    pixel("stream rollover preserves earlier packet", 0, 255, 0, 1, 0);
    pixel("stream rollover preserves later packet", 255, 255, 0, 10, 1);
    compareImage("vertex stream rollover with queued packets");
}

void checkRecordingWorkers(unsigned workerCount)
{
    vulkanFinish();
    VulkanRecordingStats stats = {};
    vulkanGetRecordingStats(&stats);
    require(stats.workerCount == workerCount, "requested recording worker count is active");
    require(stats.startedWorkers == workerCount, "large batches start every requested worker");
    require(stats.backgroundWorkerCount == workerCount - 1 && stats.startedBackgroundWorkers == workerCount - 1,
            "total recorders consist of the caller and the requested background threads");
    require(stats.callerThreadId == static_cast<uint64_t>(SDL_ThreadID()), "recording caller identity is correct");
    require(stats.workerDraws.size() == stats.startedWorkers && stats.workerThreadIds.size() == stats.startedWorkers &&
            stats.workerRecordNanoseconds.size() == stats.startedWorkers, "worker diagnostic vectors match started workers");
    require(stats.batches && stats.secondaryBuffers && stats.draws,
            "test draws reached secondary command buffer recording");
    uint64_t recordedDraws = 0;
    for (unsigned i = 0; i < workerCount; ++i)
    {
        require(stats.workerDraws[i] > 0, "each recording worker recorded draws");
        require(stats.workerRecordNanoseconds[i] > 0, "each recording worker spent time recording");
        recordedDraws += stats.workerDraws[i];
        std::printf("Worker %u: thread %llu, %llu draws, %.3f ms recording\n", i,
                    static_cast<unsigned long long>(stats.workerThreadIds[i]),
                    static_cast<unsigned long long>(stats.workerDraws[i]),
                    stats.workerRecordNanoseconds[i] / 1000000.0);
        if (i > 0)
        {
            require(stats.workerThreadIds[i] != static_cast<uint64_t>(SDL_ThreadID()),
                    "parallel recording occurs off the calling thread");
            for (unsigned previous = 0; previous < i; ++previous)
                require(stats.workerThreadIds[i] != stats.workerThreadIds[previous],
                        "each worker records on a distinct thread");
        }
        else
            require(stats.workerThreadIds[i] == static_cast<uint64_t>(SDL_ThreadID()),
                    "recorder zero is always the calling thread, including parallel batches");
    }
    require(recordedDraws == stats.draws, "workers account for every recorded draw");
    require(stats.peakActiveRecorders <= workerCount, "active recorders respect configured worker count");
    require(stats.peakActiveRecorders >= (workerCount > 1 ? 2u : 1u),
            "multiple workers actually overlap command buffer recording");
    require(workerCount > 1 ? stats.callerParallelBatches > 0 : stats.callerParallelBatches == 0,
            "the caller itself participates in observed parallel recording");
    std::printf("PASS: %u recorders (caller + %u background), peak overlap %u, %llu draws in %llu secondary buffers\n",
                stats.workerCount, stats.backgroundWorkerCount, stats.peakActiveRecorders, static_cast<unsigned long long>(stats.draws),
                static_cast<unsigned long long>(stats.secondaryBuffers));
}

void run(SDL_Window *window, unsigned workerCount)
{
    require(vulkanCreate(window, 64, 64, 0), "create native Vulkan renderer");
    require(SDL_GL_GetCurrentContext() == nullptr, "native Vulkan requires no OpenGL context");
    VulkanDrawState state = defaults();
    float const blue[] = { 0.f, 0.f, 1.f, 1.f };
    float const black[] = { 0.f, 0.f, 0.f, 1.f };
    vulkanClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT, blue, 1.f, 0, state);
    pixel("color clear", 0, 0, 255);
    triangle(state, 1.f, 0.f, 0.f);
    pixel("triangle rasterization", 255, 0, 0);

    vulkanClear(GL_COLOR_BUFFER_BIT, blue, 1.f, 0, state);
    state.cull = true;
    triangle(state, 1.f, 0.f, 0.f);
    pixel("CCW front face survives backface culling", 255, 0, 0);
    state.cullFace = GL_FRONT;
    triangle(state, 0.f, 1.f, 0.f);
    pixel("front face culling", 255, 0, 0);
    state.cull = false;

    state.depthTest = true;
    triangle(state, 0.f, 1.f, 0.f, 1.f, -0.5f);
    triangle(state, 1.f, 0.f, 0.f, 1.f, 0.5f);
    pixel("depth occlusion", 0, 255, 0);
    state.depthTest = false;
    triangle(state, 1.f, 0.f, 0.f, 1.f, 2.f);
    pixel("far-plane clipping", 0, 255, 0);
    if (vulkanDepthClampSupported())
    {
        state.depthClamp = true;
        triangle(state, 1.f, 0.f, 0.f, 1.f, 2.f);
        pixel("depth clamping", 255, 0, 0);
        state.depthClamp = false;
        triangle(state, 0.f, 1.f, 0.f);
    }
    state.blend = true;
    triangle(state, 1.f, 0.f, 0.f, 0.5f);
    pixel("source alpha blending", 128, 127, 0);
    state.blendSrc = state.blendDst = GL_ONE;
    state.blendEquation = GL_FUNC_REVERSE_SUBTRACT;
    triangle(state, 0.25f, 0.f, 0.f);
    pixel("reverse subtract tint blending", 64, 127, 0);
    state = defaults();

    state.scissorTest = true;
    state.scissor[0] = state.scissor[1] = 0;
    state.scissor[2] = state.scissor[3] = 16;
    vulkanClear(GL_COLOR_BUFFER_BIT, blue, 1.f, 0, state);
    pixel("scissor bottom-left origin", 0, 0, 255, 8, 8);
    pixel("scissor preserves other pixels", 64, 127, 0, 8, 56);
    state = defaults();

    unsigned char const texture[] = { 255, 0, 0, 255, 0, 255, 0, 255,
                                      0, 0, 255, 255, 255, 255, 255, 255 };
    vulkanUploadTexture(1, 0, 2, 2, texture);
    state.samplers[0].texture = 1;
    state.uniforms.fixed[0] = 1.f;
    triangle(state, 1.f, 1.f, 1.f);
    pixel("texture bottom-left origin", 255, 0, 0, 8, 8);
    pixel("texture top-left orientation", 0, 0, 255, 8, 56);
    pixel("texture bottom-right orientation", 0, 255, 0, 56, 8);
    unsigned char const yellow[] = { 255, 255, 0, 255 };
    // Exercise an upload in the middle of an unsubmitted render pass.
    triangle(state, 1.f, 1.f, 1.f);
    vulkanUploadTextureRegion(1, 0, 0, 1, 1, 1, yellow);
    triangle(state, 1.f, 1.f, 1.f);
    pixel("subimage upload between draws", 255, 255, 0, 8, 56);
    pixel("subimage preserves other texels", 0, 255, 0, 56, 8);
    vulkanUploadTexture(1, 1, 1, 1, yellow);
    state.samplers[0].minFilter = GL_NEAREST_MIPMAP_NEAREST;
    state.samplers[0].minLod = state.samplers[0].maxLod = 1.f;
    triangle(state, 1.f, 1.f, 1.f);
    pixel("explicit mip level and sampler LOD limits", 255, 255, 0);
    state = defaults();

    // A tall NPOT texture exercises multiple staging copies and the final short
    // row range. Check actual texels around its far edge, not an aggregate image.
    int const largeWidth = 1026, largeHeight = 1027;
    std::vector<unsigned char> largeTexture(static_cast<size_t>(largeWidth) * largeHeight * 4, 255);
    for (int row = 0; row < largeHeight; ++row)
        for (int column = 0; column < largeWidth; ++column)
        {
            auto texel = largeTexture.data() + (static_cast<size_t>(row) * largeWidth + column) * 4;
            texel[0] = row == 0 || row == largeHeight - 2 ? 255 : 0;
            texel[1] = row == 0 ? 0 : 255;
            texel[2] = row == largeHeight - 1 ? 255 : 0;
        }
    vulkanUploadTexture(2, 0, largeWidth, largeHeight, largeTexture.data());
    state.samplers[0].texture = 2;
    state.uniforms.fixed[0] = 1.f;
    state.uniforms.textureMatrix[0][5] = 0.f;
    int const checkedRows[] = { 0, largeHeight / 2, largeHeight - 2, largeHeight - 1 };
    int const expectedColors[][3] = { { 255, 0, 0 }, { 0, 255, 0 }, { 255, 255, 0 }, { 0, 255, 255 } };
    for (unsigned i = 0; i < sizeof(checkedRows) / sizeof(checkedRows[0]); ++i)
    {
        state.uniforms.textureMatrix[0][13] = (checkedRows[i] + 0.5f) / largeHeight;
        triangle(state, 1.f, 1.f, 1.f);
        pixel("large NPOT texture row", expectedColors[i][0], expectedColors[i][1], expectedColors[i][2]);
    }
    int const regionWidth = largeWidth - 2, regionHeight = largeHeight - 2;
    largeTexture.assign(static_cast<size_t>(regionWidth) * regionHeight * 4, 255);
    for (size_t i = 1; i < largeTexture.size(); i += 4) largeTexture[i] = 0;
    vulkanUploadTextureRegion(2, 0, 1, 1, regionWidth, regionHeight, largeTexture.data());
    state.uniforms.textureMatrix[0][13] = (largeHeight - 1.5f) / largeHeight;
    triangle(state, 1.f, 1.f, 1.f);
    pixel("large subimage final staging chunk", 255, 0, 255);
    state.uniforms.textureMatrix[0][0] = 0.f;
    state.uniforms.textureMatrix[0][12] = 0.5f / largeWidth;
    triangle(state, 1.f, 1.f, 1.f);
    pixel("large subimage preserves left edge", 255, 255, 0);
    state.uniforms.textureMatrix[0][12] = 0.5f;
    state.uniforms.textureMatrix[0][13] = (largeHeight - 0.5f) / largeHeight;
    triangle(state, 1.f, 1.f, 1.f);
    pixel("large subimage preserves final row", 0, 255, 255);
    vulkanDeleteTexture(2);
    state = defaults();

    vulkanClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT, blue, 1.f, 3, state);
    state.stencilTest = true;
    state.stencilFunc = GL_EQUAL;
    state.stencilReference = 3;
    triangle(state, 1.f, 0.f, 0.f);
    pixel("stencil comparison accepts matching value", 255, 0, 0);
    state.stencilReference = 1;
    triangle(state, 0.f, 1.f, 0.f);
    pixel("stencil comparison rejects differing value", 255, 0, 0);
    state.stencilWriteMask = 1;
    vulkanClear(GL_STENCIL_BUFFER_BIT, black, 1.f, 0, state);
    state.stencilReference = 2;
    triangle(state, 0.f, 1.f, 0.f);
    pixel("masked stencil clear preserves other bits", 0, 255, 0);
    state = defaults();
    state.colorWrite[0] = state.colorWrite[2] = state.colorWrite[3] = false;
    vulkanClear(GL_COLOR_BUFFER_BIT, blue, 1.f, 0, state);
    pixel("masked color clear", 0, 0, 0);
    state = defaults();
    require(vulkanPresent(), "present rendered image");

    orderingAndBarriers();
    streamRollover();

    // Exhaust a descriptor pool without a readback or explicit finish. This
    // catches accidental reuse of descriptors/vertices while the GPU reads them.
    state.scissorTest = true;
    state.scissor[2] = state.scissor[3] = 1;
    for (unsigned i = 0; i < 16400; ++i)
    {
        state.scissor[0] = i % 64;
        state.scissor[1] = (i / 64) % 64;
        unsigned const color = i / 4096 + i + 1;
        triangle(state, float(color & 1), float((color >> 1) & 1), float((color >> 2) & 1));
    }
    pixel("descriptor rollover preserves draw order", 255, 0, 255, 0, 0);
    compareImage("descriptor rollover with queued packets");
    require(vulkanPresent(), "present after descriptor rollover");

    SDL_SetWindowSize(window, 96, 96);
    SDL_PumpEvents();
    require(vulkanResize(96, 96), "recreate swapchain after resize");
    state = defaults(96, 96);
    triangle(state, 0.f, 1.f, 0.f);
    pixel("resized render target", 0, 255, 0, 80, 80);
    compareImage("resized render target", 96, 96);
    require(vulkanPresent(), "present resized swapchain");
    vulkanSetVsync(1);
    triangle(state, 0.f, 0.f, 1.f);
    pixel("vsync swapchain recreation", 0, 0, 255);
    require(vulkanPresent(), "present after changing vsync");
    vulkanDeleteTexture(1);
    checkRecordingWorkers(workerCount);
    vulkanDestroy();
    require(!vulkanActive(), "destroy resets renderer state");
    require(vulkanCreate(window, 96, 96, 0), "recreate backend after teardown");
    triangle(state, 1.f, 0.f, 0.f);
    pixel("backend recreation", 255, 0, 0);
    compareImage("backend recreation", 96, 96);
    // Deliberately reject an invalid upload: ownership must remain Vulkan so
    // SDL cannot dispatch OpenGL calls after a runtime renderer failure.
    std::puts("Checking runtime failure ownership (one invalid-subimage diagnostic is expected)");
    for (unsigned i = 0; i < 512; ++i) triangle(state, 0.f, 1.f, 0.f);
    vulkanUploadTextureRegion(999, 0, 0, 0, 1, 1, yellow);
    require(vulkanActive(), "failed backend retains Vulkan window ownership");
    require(!vulkanPresent(), "runtime failure propagates through presentation");
    vulkanDestroy();
    require(!vulkanActive(), "failed renderer teardown releases ownership");
    require(vulkanCreate(window, 96, 96, 0), "recreate workers after failed renderer teardown");
    triangle(state, 0.f, 0.f, 1.f);
    pixel("workers recover after failed renderer teardown", 0, 0, 255);
    compareImage("recreation after failure with pending draws", 96, 96);
    require(vulkanPresent(), "present after failed renderer teardown and recreation");
    vulkanDestroy();
}

void automaticWorkers(SDL_Window *window)
{
    require(SDL_setenv("NBLOOD_VULKAN_WORKERS", "0", 1) == 0, "configure automatic recording workers");
    unsigned const cores = vulkanAvailableRecordingCores();
    require(cores >= 1, "automatic CPU detection always provides at least one core");
    require(vulkanCreate(window, 96, 96, 0), "create renderer with automatic worker count");
    VulkanDrawState state = defaults(96, 96);
    triangle(state, 0.f, 1.f, 0.f);
    pixel("automatic worker count renders correctly", 0, 255, 0);
    VulkanRecordingStats stats = {};
    vulkanGetRecordingStats(&stats);
    unsigned const expectedWorkers = recordingWorkerLimit(cores);
    require(stats.workerCount == expectedWorkers, "automatic count includes the caller plus at most four background threads");
    require(stats.backgroundWorkerCount == expectedWorkers - 1, "automatic background capacity reserves one caller slot");
    require(stats.startedWorkers == 1 && stats.startedBackgroundWorkers == 0,
            "one draw uses the caller without creating background threads");
    require(stats.workerThreadIds.size() == 1 && stats.workerThreadIds[0] == static_cast<uint64_t>(SDL_ThreadID()),
            "one draw records on the calling thread even when parallel recording is configured");
    for (unsigned draw = 0; draw < 32; ++draw) triangle(state, 0.f, 1.f, 0.f);
    pixel("small draw batch remains on the caller", 0, 255, 0);
    vulkanGetRecordingStats(&stats);
    require(stats.startedWorkers == 1 && stats.startedBackgroundWorkers == 0 && stats.callerParallelBatches == 0,
            "a small batch neither creates nor dispatches background recording threads");
    std::printf("PASS: %u available cores select %u total recorders; small batches use only the caller\n",
                cores, stats.workerCount);
    for (unsigned draw = 0; draw < 512; ++draw) triangle(state, 0.f, 1.f, 0.f);
    pixel("automatic caller and background recording", 0, 255, 0);
    checkRecordingWorkers(expectedWorkers);
    vulkanDestroy();
    require(SDL_setenv("NBLOOD_VULKAN_WORKERS", "999", 1) == 0, "request worker count above allowed limit");
    require(vulkanCreate(window, 96, 96, 0), "create renderer with oversized worker request");
    triangle(state, 0.f, 1.f, 0.f);
    pixel("clamped worker override renders correctly", 0, 255, 0);
    vulkanGetRecordingStats(&stats);
    require(stats.workerCount == expectedWorkers, "explicit worker count cannot exceed automatic limit");
    vulkanDestroy();
    require(SDL_setenv("NBLOOD_VULKAN_WORKERS", "0", 1) == 0, "restore automatic worker selection");

#ifdef __linux__
    // Exercise real affinity masks for one, two and three CPUs. Background
    // threads inherit each mask; restore the original after every renderer.
    cpu_set_t original;
    if (sched_getaffinity(0, sizeof(original), &original) != 0)
    {
        std::perror("SKIP: cannot read affinity for automatic recording policy tests");
        return;
    }
    for (unsigned available = 1; available <= 3; ++available)
    {
        cpu_set_t selected;
        CPU_ZERO(&selected);
        unsigned count = 0;
        for (int cpu = 0; cpu < CPU_SETSIZE && count < available; ++cpu)
            if (CPU_ISSET(cpu, &original))
            {
                CPU_SET(cpu, &selected);
                ++count;
            }
        if (count != available)
        {
            std::printf("SKIP: %u-core automatic recording test needs more available CPUs\n", available);
            continue;
        }
        if (sched_setaffinity(0, sizeof(selected), &selected) != 0)
        {
            std::perror("SKIP: cannot restrict affinity for automatic recording policy test");
            continue;
        }
        require(vulkanAvailableRecordingCores() == available, "automatic CPU detection respects test affinity");
        require(vulkanCreate(window, 96, 96, 0), "create automatic renderer on restricted available cores");
        for (unsigned draw = 0; draw < 512; ++draw) triangle(state, 1.f, 0.f, 0.f);
        pixel("restricted-core automatic recording", 255, 0, 0);
        checkRecordingWorkers(available);
        vulkanDestroy();
        require(sched_setaffinity(0, sizeof(original), &original) == 0, "restore affinity after CPU policy test");
        std::printf("PASS: %u available cores select caller + %u background recorders\n", available, available - 1);
    }
#endif
}

void workerFailures(SDL_Window *window)
{
#ifdef VULKAN_BACKEND_TEST_WRAP
    if (recordingWorkerLimit(vulkanAvailableRecordingCores()) < 2)
    {
        std::puts("SKIP: concurrent recording error injection needs at least two available CPU cores");
        return;
    }
    require(SDL_setenv("NBLOOD_VULKAN_WORKERS", "5", 1) == 0, "configure workers for injected Vulkan failures");
    for (unsigned failure = 1; failure <= 3; ++failure)
    {
        require(vulkanCreate(window, 96, 96, 0), "create backend before injected worker failure");
        failureCaller.store(static_cast<uint64_t>(SDL_ThreadID()));
        failureObserved.store(false);
        backgroundGateReached.store(0);
        backgroundGateHeld.store(0);
        releaseBackgroundGate.store(false);
        injectedFailure.store(failure);
        char const *failureLabel = failure == 1 ? "background secondary command buffer begin" :
                                   failure == 2 ? "background descriptor allocation" :
                                                  "caller secondary begin while a background recorder is active";
        std::printf("Checking %s failure (one Vulkan failure diagnostic is expected)\n", failureLabel);
        VulkanDrawState const state = defaults(96, 96);
        for (unsigned draw = 0; draw < 512; ++draw) triangle(state, 1.f, 0.f, 0.f);
        vulkanFinish();
        require(failureObserved.load(), "injected Vulkan failure occurred on its intended recording thread");
        VulkanRecordingStats stats = {};
        vulkanGetRecordingStats(&stats);
        require(!stats.workerDraws.empty(), "failed recording batch still publishes completed work diagnostics");
        if (failure == 3)
        {
            require(backgroundGateReached.load() > 0 && backgroundGateHeld.load() == 0,
                    "background recording began before caller failure and resumed afterward");
            uint64_t backgroundDraws = 0;
            for (size_t i = 1; i < stats.workerDraws.size(); ++i) backgroundDraws += stats.workerDraws[i];
            require(backgroundDraws > 0 && stats.callerParallelBatches > 0,
                    "caller failure waits for concurrently recording background jobs to finish");
        }
        else
            require(stats.workerDraws[0] > 0, "caller finishes its recording work before a background failure is published");
        require(vulkanActive(), "worker failure preserves native window ownership");
        require(!vulkanPresent(), "worker failure reaches the presentation caller");
        vulkanDestroy();
        require(!vulkanActive(), "worker failure teardown joins workers and releases ownership");
        require(vulkanCreate(window, 96, 96, 0), "recreate backend after worker failure");
        triangle(state, 0.f, 1.f, 0.f);
        pixel("rendering recovers after an injected worker failure", 0, 255, 0);
        require(vulkanPresent(), "presentation recovers after an injected worker failure");
        vulkanDestroy();
    }
#else
    (void)window;
    std::puts("SKIP: worker Vulkan failure injection requires the Linux test linker wrappers");
#endif
}
} // namespace

int main()
{
    if (SDL_Init(SDL_INIT_VIDEO))
    {
        std::fprintf(stderr, "SDL video initialization failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window *window = SDL_CreateWindow("Native Vulkan regression tests", SDL_WINDOWPOS_UNDEFINED,
                                          SDL_WINDOWPOS_UNDEFINED, 64, 64,
                                          SDL_WINDOW_VULKAN | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!window)
    {
        std::fprintf(stderr, "SDL Vulkan window creation failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    require(SDL_setenv("NBLOOD_VULKAN_WORKERS", "1", 1) == 0, "configure synchronous recording reference");
    std::puts("Testing one synchronous recording worker");
    run(window, 1);
    recordReference = false;
    require(SDL_setenv("NBLOOD_VULKAN_WORKERS", "5", 1) == 0, "configure caller plus parallel recording workers");
    SDL_SetWindowSize(window, 64, 64);
    SDL_PumpEvents();
    unsigned const parallelWorkers = recordingWorkerLimit(vulkanAvailableRecordingCores());
    std::printf("Testing %u recording workers\n", parallelWorkers);
    if (parallelWorkers < 2)
        std::puts("SKIP: recording overlap requires at least two available CPU cores");
    run(window, parallelWorkers);
    require(comparisonIndex == referenceImages.size(), "every reference image was compared");
    automaticWorkers(window);
    workerFailures(window);
    SDL_DestroyWindow(window);
    SDL_Quit();
    std::puts("PASS: all native Vulkan backend regression checks");
    return 0;
}
