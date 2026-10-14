// Native Vulkan backend for the shared Polymost scene frontend.
// Copyright (C) 2026 NBlood contributors. GPL-2.0-only.
#include "vulkan_renderer.h"

#ifdef USE_VULKAN
#include <SDL.h>
#include <SDL_vulkan.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "vulkan_dispatch.h"
#include "glad/glad.h" // Enumerants only; this backend never calls OpenGL.
#include "vulkan_polymost_spv.h"
#include "ambient_occlusion_vk_spv.h"
#include "realtime_lighting_vk_spv.h"
#include "realtime_shadow_vk_spv.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cfloat>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <new>
#include <vector>
#ifdef __linux__
#include <sched.h>
#endif

#ifdef VULKAN_BACKEND_TEST_DISPATCH
// Tests intercept resolved calls; production builds have no injection hook.
extern PFN_vkVoidFunction vulkanTestResolveProc(char const *, PFN_vkVoidFunction);
#endif

namespace {
PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
#define DECLARE_VULKAN_PROC(name) PFN_##name name = nullptr;
NBLOOD_VULKAN_GLOBAL_COMMANDS(DECLARE_VULKAN_PROC)
NBLOOD_VULKAN_INSTANCE_COMMANDS(DECLARE_VULKAN_PROC)
NBLOOD_VULKAN_DEVICE_COMMANDS(DECLARE_VULKAN_PROC)
#undef DECLARE_VULKAN_PROC

struct Buffer
{
    VkBuffer handle = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    void *mapped = nullptr;
};

struct Image
{
    VkImage handle = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    uint32_t width = 0, height = 0, levels = 1;
};

struct PipelineKey
{
    uint32_t topology, depthFunc, blendSrc, blendDst, blendEquation, cullFace, frontFace, polygonMode;
    uint32_t stencilFunc, stencilFail, stencilDepthFail, stencilPass;
    uint32_t depthTest, depthWrite, blend, cull, stencilTest, depthBias, depthClamp, colorMask;
    bool operator<(PipelineKey const &rhs) const { return std::memcmp(this, &rhs, sizeof(*this)) < 0; }
};

struct SamplerKey
{
    uint32_t minFilter, magFilter, wrapS, wrapT, anisotropy, minLod, maxLod;
    bool operator<(SamplerKey const &rhs) const { return std::memcmp(this, &rhs, sizeof(*this)) < 0; }
};

// The frontend may immediately reuse its vertex storage and mutable GL state.
// Packets and their contiguous vertex snapshot stay immutable until all workers
// finish. Resource handles are resolved by the producer; workers never access
// the texture, sampler or pipeline caches.
struct DrawPacket
{
    VulkanDrawState state;
    VkPipeline pipeline;
    VkDescriptorImageInfo images[5];
    VkViewport viewport;
    VkRect2D scissor;
    VkDeviceSize vertexOffset, uniformOffset;
    size_t firstVertex;
    uint32_t vertexCount;
    float lineWidth;
};

struct RecordingContext
{
    VkDevice device;
    VkRenderPass renderPass;
    VkFramebuffer framebuffer;
    VkPipelineLayout pipelineLayout;
    VkDescriptorSetLayout descriptorLayout;
    VkBuffer stream;
    uint8_t *mapped;
    DrawPacket const *packets;
    VulkanVertex const *vertices;
};

struct RecordingResources
{
    VkCommandPool commandPool = VK_NULL_HANDLE;
    std::vector<VkDescriptorPool> descriptorPools;
    size_t descriptorPoolIndex = 0;
    uint32_t descriptorSetsUsed = 0;
    std::vector<VkCommandBuffer> commands;
    size_t commandsUsed = 0;
};

struct SubmissionSlot
{
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE;
    Buffer stream;
    VkDeviceSize streamOffset = 0;
    uint32_t descriptorCount = 0;
    uint64_t serial = 0;
    bool pending = false, prepared = false;
    std::vector<RecordingResources *> recorders;
};

struct RecordingWorker
{
    SDL_Thread *thread = nullptr;
    SDL_sem *start = nullptr, *done = nullptr;
    bool stop = false, caller = false;
    RecordingResources *resources = nullptr;
    RecordingContext context = {};
    size_t first = 0, count = 0;
    VkCommandBuffer result = VK_NULL_HANDLE;
    VkResult error = VK_SUCCESS;
    char const *operation = nullptr;
    uint64_t threadId = 0, elapsedNanoseconds = 0;
};

struct RetiredImage { Image image; uint64_t serial; };
struct RetiredBuffer { Buffer buffer; uint64_t serial; };

struct AmbientOcclusion
{
    VkDescriptorSetLayout descriptorLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkShaderModule vertexShader = VK_NULL_HANDLE, fragmentShader = VK_NULL_HANDLE;
    VkImageView depthView = VK_NULL_HANDLE, stencilView = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    bool supported = false, warned = false;
};

// Keep the order identical to ambient_occlusion_vk.frag's std140 block and
// the shared ambient_occlusion.glsl algorithm used by OpenGL Polymost.
struct AmbientOcclusionUniforms
{
    float projection[4], depth[4], viewport[4], target[4];
};

constexpr uint32_t kShadowFaceSize = 256;
constexpr uint32_t kShadowFaces = 6;
struct RealtimeLighting
{
    VkDescriptorSetLayout descriptorLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE, shadowLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE, shadowPipeline = VK_NULL_HANDLE;
    VkShaderModule vertexShader = VK_NULL_HANDLE, fragmentShader = VK_NULL_HANDLE;
    VkShaderModule shadowVertexShader = VK_NULL_HANDLE, shadowFragmentShader = VK_NULL_HANDLE;
    VkRenderPass shadowRenderPass = VK_NULL_HANDLE;
    VkFramebuffer shadowFramebuffer = VK_NULL_HANDLE;
    VkFormat shadowFormat = VK_FORMAT_UNDEFINED;
    Image atlas;
    bool supported = false, warned = false, atlasInitialized = false;
};

struct RealtimeLightingUniforms
{
    float projection[4], depth[4], viewport[4], target[4];
    float eyeToWorld[16], lightParams[4];
    VulkanLight lights[kVulkanMaxLights];
};
static_assert(kVulkanMaxLights == 8 && sizeof(VulkanLight) == 32 && sizeof(RealtimeLightingUniforms) == 400,
              "Keep realtime_lighting_vk.frag's std140 light block in sync");

struct ShadowPush
{
    float light[4];
    int32_t face[4];
};

struct Backend
{
    SDL_Window *window = nullptr;
    bool loaderOwned = false, deviceDispatchReady = false;
    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties = {};
    VkPhysicalDeviceFeatures features = {};
    VkPhysicalDeviceMemoryProperties memory = {};
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat surfaceFormat = VK_FORMAT_UNDEFINED;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D extent = {};
    std::vector<VkImage> swapImages;
    std::vector<bool> swapInitialized;
    std::vector<VkSemaphore> presentationSemaphores;
    std::vector<SubmissionSlot *> slots;
    size_t slotIndex = 0;
    VkDescriptorSetLayout descriptorLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipelineCache pipelineCache = VK_NULL_HANDLE;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkShaderModule vertexShader = VK_NULL_HANDLE, fragmentShader = VK_NULL_HANDLE;
    Image color, depth;
    AmbientOcclusion ao;
    RealtimeLighting lighting;
    std::map<uint32_t, Image> textures;
    std::map<PipelineKey, VkPipeline> pipelines;
    std::map<SamplerKey, VkSampler> samplers;
    std::vector<DrawPacket> packets;
    std::vector<VulkanVertex> vertices;
    std::vector<RecordingWorker *> workers;
    VulkanRecordingStats recordingStats = {};
    VulkanSubmissionStats submissionStats = {};
    std::vector<RetiredImage> retiredImages;
    std::vector<RetiredBuffer> retiredBuffers;
    VkSubpassContents renderPassContents = VK_SUBPASS_CONTENTS_INLINE;
    bool recording = false, inRenderPass = false, failed = false, ready = false;
    bool suspended = false;
    bool portabilitySubset = false;
    bool depthClipControl = false;
    int vsync = 1;
    int swapchainVsync = 1;
};

Backend g;
constexpr VkDeviceSize kStreamBytes = 64u * 1024u * 1024u;
constexpr VkDeviceSize kTextureUploadBytes = 4u * 1024u * 1024u;
constexpr uint32_t kDescriptorSets = 16384;
constexpr size_t kDrawBatchSize = 1024;
constexpr size_t kDrawBatchVertexBytes = 4u * 1024u * 1024u;
constexpr size_t kDrawsPerWorker = 32;
constexpr uint32_t kWorkerDescriptorPageSets = 256;
// Bit 0 tracks the caller; the remaining bits count active background workers.
// One atomic transition can therefore prove caller/background overlap without
// racing independent snapshots of a count and a caller-active flag.
std::atomic<uint32_t> recordingParticipants(0), batchPeakRecorders(0);
std::atomic<bool> batchCallerOverlap(false);

bool flushDrawPackets();

SubmissionSlot &currentSlot() { return *g.slots[g.slotIndex]; }

bool check(VkResult result, char const *operation)
{
    if (result == VK_SUCCESS)
        return true;
    std::fprintf(stderr, "Vulkan: %s failed (VkResult %d)\n", operation, static_cast<int>(result));
    g.failed = true;
    return false;
}

bool fail(char const *message)
{
    std::fprintf(stderr, "Vulkan: %s\n", message);
    g.failed = true;
    return false;
}

template<typename Proc> bool resolveProc(Proc &destination, PFN_vkVoidFunction address, char const *name)
{
#ifdef VULKAN_BACKEND_TEST_DISPATCH
    address = vulkanTestResolveProc(name, address);
#endif
    destination = reinterpret_cast<Proc>(address);
    if (destination) return true;
    std::fprintf(stderr, "Vulkan: required entry point %s is unavailable\n", name);
    return fail("The installed Vulkan runtime is incomplete; update the graphics driver.");
}

bool loadGlobalDispatch()
{
    if (SDL_Vulkan_LoadLibrary(nullptr) < 0) return fail(SDL_GetError());
    g.loaderOwned = true;
    vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
    if (!vkGetInstanceProcAddr) return fail(SDL_GetError());
#define LOAD_GLOBAL_PROC(name) if (!resolveProc(name, vkGetInstanceProcAddr(VK_NULL_HANDLE, #name), #name)) return false;
    NBLOOD_VULKAN_GLOBAL_COMMANDS(LOAD_GLOBAL_PROC)
#undef LOAD_GLOBAL_PROC
    return true;
}

bool loadInstanceDispatch()
{
#define LOAD_INSTANCE_PROC(name) if (!resolveProc(name, vkGetInstanceProcAddr(g.instance, #name), #name)) return false;
    NBLOOD_VULKAN_INSTANCE_COMMANDS(LOAD_INSTANCE_PROC)
#undef LOAD_INSTANCE_PROC
    return true;
}

bool loadDeviceDispatch()
{
#define LOAD_DEVICE_PROC(name) if (!resolveProc(name, vkGetDeviceProcAddr(g.device, #name), #name)) return false;
    NBLOOD_VULKAN_DEVICE_COMMANDS(LOAD_DEVICE_PROC)
#undef LOAD_DEVICE_PROC
    g.deviceDispatchReady = true;
    return true;
}

uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags flags)
{
    for (uint32_t i = 0; i < g.memory.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (g.memory.memoryTypes[i].propertyFlags & flags) == flags)
            return i;
    return UINT32_MAX;
}

void destroyBuffer(Buffer &buffer)
{
    if (buffer.mapped) vkUnmapMemory(g.device, buffer.memory);
    if (buffer.handle) vkDestroyBuffer(g.device, buffer.handle, nullptr);
    if (buffer.memory) vkFreeMemory(g.device, buffer.memory, nullptr);
    buffer = Buffer();
}

bool createBuffer(Buffer &buffer, VkDeviceSize bytes, VkBufferUsageFlags usage)
{
    VkBufferCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = bytes;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!check(vkCreateBuffer(g.device, &info, nullptr, &buffer.handle), "create buffer")) return false;
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(g.device, buffer.handle, &requirements);
    uint32_t type = memoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type == UINT32_MAX) return fail("No host-visible coherent buffer memory is available");
    VkMemoryAllocateInfo allocation = {};
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = type;
    if (!check(vkAllocateMemory(g.device, &allocation, nullptr, &buffer.memory), "allocate buffer memory")) return false;
    if (!check(vkBindBufferMemory(g.device, buffer.handle, buffer.memory, 0), "bind buffer memory")) return false;
    if (!check(vkMapMemory(g.device, buffer.memory, 0, requirements.size, 0, &buffer.mapped), "map buffer memory")) return false;
    buffer.size = bytes;
    return true;
}

void destroyImage(Image &image)
{
    if (image.view) vkDestroyImageView(g.device, image.view, nullptr);
    if (image.handle) vkDestroyImage(g.device, image.handle, nullptr);
    if (image.memory) vkFreeMemory(g.device, image.memory, nullptr);
    image = Image();
}

bool createImage(Image &image, uint32_t width, uint32_t height, uint32_t levels, VkFormat format,
                 VkImageUsageFlags usage, VkImageAspectFlags aspect)
{
    image.width = width;
    image.height = height;
    image.levels = levels;
    VkImageCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = { width, height, 1 };
    info.mipLevels = levels;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!check(vkCreateImage(g.device, &info, nullptr, &image.handle), "create image")) return false;
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(g.device, image.handle, &requirements);
    uint32_t type = memoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) type = memoryType(requirements.memoryTypeBits, 0);
    if (type == UINT32_MAX) return fail("No compatible image memory is available");
    VkMemoryAllocateInfo allocation = {};
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = type;
    if (!check(vkAllocateMemory(g.device, &allocation, nullptr, &image.memory), "allocate image memory")) return false;
    if (!check(vkBindImageMemory(g.device, image.handle, image.memory, 0), "bind image memory")) return false;
    VkImageViewCreateInfo view = {};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = image.handle;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange = { aspect, 0, levels, 0, 1 };
    return check(vkCreateImageView(g.device, &view, nullptr, &image.view), "create image view");
}

void collectRetiredResources()
{
    for (size_t i = 0; i < g.retiredImages.size(); )
    {
        if (g.retiredImages[i].serial > g.submissionStats.completedSubmissions) { ++i; continue; }
        destroyImage(g.retiredImages[i].image);
        g.retiredImages.erase(g.retiredImages.begin() + i);
    }
    for (size_t i = 0; i < g.retiredBuffers.size(); )
    {
        if (g.retiredBuffers[i].serial > g.submissionStats.completedSubmissions) { ++i; continue; }
        destroyBuffer(g.retiredBuffers[i].buffer);
        g.retiredBuffers.erase(g.retiredBuffers.begin() + i);
    }
    g.submissionStats.pendingRetiredImages = static_cast<uint32_t>(g.retiredImages.size());
}

void markCompleted(uint64_t serial)
{
    // All submissions use the same queue. Completion of this fence covers all
    // earlier serials, including resources shared across submission slots.
    g.submissionStats.completedSubmissions = std::max(g.submissionStats.completedSubmissions, serial);
    for (auto *slot : g.slots)
        if (slot->pending && slot->serial <= g.submissionStats.completedSubmissions)
        {
            slot->pending = false;
            --g.submissionStats.pendingSubmissions;
        }
    collectRetiredResources();
}

bool pollCompletions()
{
    for (auto *slot : g.slots)
        if (slot->pending)
        {
            VkResult const result = vkGetFenceStatus(g.device, slot->fence);
            if (result == VK_SUCCESS) markCompleted(slot->serial);
            else if (result != VK_NOT_READY) return check(result, "query submission fence");
        }
    // These are observed unsignaled fences, not evidence of simultaneous GPU
    // rasterization. Already-completed serials discovered later in the poll are
    // removed before recording the high-water mark.
    g.submissionStats.maxObservedUnsignaled = std::max(g.submissionStats.maxObservedUnsignaled,
                                                       g.submissionStats.pendingSubmissions);
    return true;
}

enum class WaitReason { SlotReuse, Finish, Readback };

bool waitForSerial(uint64_t serial, WaitReason reason)
{
    if (serial <= g.submissionStats.completedSubmissions) return true;
    if (!pollCompletions()) return false;
    if (serial <= g.submissionStats.completedSubmissions) return true;
    SubmissionSlot *target = nullptr;
    for (auto *slot : g.slots)
        if (slot->pending && slot->serial >= serial && (!target || slot->serial < target->serial)) target = slot;
    if (!target) return fail("No fence covers an outstanding submission");
    ++g.submissionStats.fenceWaits;
    if (reason == WaitReason::SlotReuse) ++g.submissionStats.slotReuseWaits;
    else if (reason == WaitReason::Finish) ++g.submissionStats.finishWaits;
    else ++g.submissionStats.readbackWaits;
    if (!check(vkWaitForFences(g.device, 1, &target->fence, VK_TRUE, UINT64_MAX), "wait for submission fence")) return false;
    markCompleted(target->serial);
    return true;
}

bool prepareCurrentSlot()
{
    if (g.failed) return false;
    auto &slot = currentSlot();
    if (slot.prepared) return true;
    if (!waitForSerial(slot.serial, WaitReason::SlotReuse)) return false;
    if (!check(vkResetCommandPool(g.device, slot.commandPool, 0), "reset submission command pool")) return false;
    // Every CPU job was joined before submission, and the slot fence covers
    // every secondary and descriptor page before any pool or mapped byte reuse.
    for (auto *resources : slot.recorders)
    {
        size_t const usedPages = resources->descriptorPoolIndex + (resources->descriptorSetsUsed != 0);
        while (resources->descriptorPools.size() > std::max<size_t>(1, usedPages))
        {
            vkDestroyDescriptorPool(g.device, resources->descriptorPools.back(), nullptr);
            resources->descriptorPools.pop_back();
        }
        size_t const keepCommands = std::max<size_t>(1, resources->commandsUsed);
        if (resources->commands.size() > keepCommands)
        {
            vkFreeCommandBuffers(g.device, resources->commandPool,
                static_cast<uint32_t>(resources->commands.size() - keepCommands), resources->commands.data() + keepCommands);
            resources->commands.resize(keepCommands);
        }
        if (!check(vkResetCommandPool(g.device, resources->commandPool, 0), "reset recorder command pool")) return false;
        for (auto pool : resources->descriptorPools)
            if (!check(vkResetDescriptorPool(g.device, pool, 0), "reset recorder descriptor page")) return false;
        resources->descriptorPoolIndex = resources->descriptorSetsUsed = 0;
        resources->commandsUsed = 0;
    }
    if (!slot.stream.handle && !createBuffer(slot.stream, kStreamBytes,
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) return false;
    slot.streamOffset = 0;
    slot.descriptorCount = 0;
    slot.prepared = true;
    return true;
}

void retireImage(Image &image)
{
    ++g.submissionStats.retiredImages;
    g.retiredImages.push_back({ image, g.submissionStats.submissions });
    image = Image();
    collectRetiredResources();
}

void imageBarrier(VkImage image, VkImageAspectFlags aspect, uint32_t levels,
                  VkImageLayout oldLayout, VkImageLayout newLayout,
                  VkAccessFlags sourceAccess, VkAccessFlags destinationAccess,
                  VkPipelineStageFlags sourceStage, VkPipelineStageFlags destinationStage)
{
    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = sourceAccess;
    barrier.dstAccessMask = destinationAccess;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = { aspect, 0, levels, 0, 1 };
    vkCmdPipelineBarrier(currentSlot().command, sourceStage, destinationStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

bool beginCommands()
{
    if (g.failed) return false;
    if (g.recording) return true;
    if (!prepareCurrentSlot()) return false;
    VkCommandBufferBeginInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (!check(vkBeginCommandBuffer(currentSlot().command, &info), "begin command buffer")) return false;
    g.recording = true;
    return true;
}

void closeRenderPass()
{
    if (g.inRenderPass)
    {
        vkCmdEndRenderPass(currentSlot().command);
        g.inRenderPass = false;
    }
}

bool endRenderPass()
{
    if (!flushDrawPackets()) return false;
    closeRenderPass();
    return true;
}

// Submission advances the ring without waiting. Slot preparation waits only
// when that slot's previous GPU use has not completed; explicit Finish/readback
// and resize/teardown provide their stronger completion guarantees separately.
bool submit(VkSemaphore wait = VK_NULL_HANDLE, VkSemaphore signal = VK_NULL_HANDLE, uint64_t *submittedSerial = nullptr)
{
    if (submittedSerial) *submittedSerial = 0;
    if (g.failed) return false;
    if (!flushDrawPackets()) return false;
    if (!g.recording) return pollCompletions();
    closeRenderPass();
    auto &slot = currentSlot();
    if (!check(vkEndCommandBuffer(slot.command), "end command buffer")) return false;
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    info.commandBufferCount = 1;
    info.pCommandBuffers = &slot.command;
    if (wait)
    {
        info.waitSemaphoreCount = 1;
        info.pWaitSemaphores = &wait;
        info.pWaitDstStageMask = &stage;
    }
    if (signal)
    {
        info.signalSemaphoreCount = 1;
        info.pSignalSemaphores = &signal;
    }
    // Reset only immediately before a real submit. An acquire-out-of-date or
    // failed submit must never leave a pending slot waiting on an unsignaled
    // fence that the queue will not signal.
    if (!check(vkResetFences(g.device, 1, &slot.fence), "reset submission fence")) return false;
    if (!check(vkQueueSubmit(g.queue, 1, &info, slot.fence), "submit rendering")) return false;
    slot.serial = ++g.submissionStats.submissions;
    slot.pending = true;
    slot.prepared = false;
    ++g.submissionStats.pendingSubmissions;
    g.submissionStats.maxPendingSubmissions = std::max(g.submissionStats.maxPendingSubmissions,
                                                       g.submissionStats.pendingSubmissions);
    if (submittedSerial) *submittedSerial = slot.serial;
    g.recording = false;
    g.slotIndex = (g.slotIndex + 1) % g.slots.size();
    return pollCompletions();
}

bool reserveStream(VkDeviceSize bytes, VkDeviceSize alignment, VkDeviceSize &offset)
{
    if (!prepareCurrentSlot()) return false;
    VkDeviceSize aligned = (currentSlot().streamOffset + alignment - 1) & ~(alignment - 1);
    if (aligned + bytes > currentSlot().stream.size || currentSlot().descriptorCount == kDescriptorSets)
    {
        if (!submit() || !prepareCurrentSlot()) return false;
        aligned = 0;
    }
    auto &slot = currentSlot();
    if (bytes > slot.stream.size)
    {
        // This slot is fence-complete and its previous commands were reset.
        destroyBuffer(slot.stream);
        if (!createBuffer(slot.stream, std::max(bytes, kStreamBytes),
                          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT))
            return false;
    }
    offset = aligned;
    slot.streamOffset = aligned + bytes;
    return beginCommands();
}

bool beginRenderPass(VkSubpassContents contents = VK_SUBPASS_CONTENTS_INLINE)
{
    if (!beginCommands()) return false;
    if (g.inRenderPass && g.renderPassContents == contents) return true;
    closeRenderPass();
    VkRenderPassBeginInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    info.renderPass = g.renderPass;
    info.framebuffer = g.framebuffer;
    info.renderArea.extent = g.extent;
    vkCmdBeginRenderPass(currentSlot().command, &info, contents);
    g.renderPassContents = contents;
    g.inRenderPass = true;
    return true;
}

VkCompareOp comparison(uint32_t value)
{
    switch (value)
    {
        case GL_NEVER: return VK_COMPARE_OP_NEVER;
        case GL_LESS: return VK_COMPARE_OP_LESS;
        case GL_EQUAL: return VK_COMPARE_OP_EQUAL;
        case GL_LEQUAL: return VK_COMPARE_OP_LESS_OR_EQUAL;
        case GL_GREATER: return VK_COMPARE_OP_GREATER;
        case GL_NOTEQUAL: return VK_COMPARE_OP_NOT_EQUAL;
        case GL_GEQUAL: return VK_COMPARE_OP_GREATER_OR_EQUAL;
        default: return VK_COMPARE_OP_ALWAYS;
    }
}

VkStencilOp stencilOperation(uint32_t value)
{
    switch (value)
    {
        case GL_ZERO: return VK_STENCIL_OP_ZERO;
        case GL_REPLACE: return VK_STENCIL_OP_REPLACE;
        case GL_INCR: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
        case GL_DECR: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
        case GL_INVERT: return VK_STENCIL_OP_INVERT;
        case GL_INCR_WRAP: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
        case GL_DECR_WRAP: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
        default: return VK_STENCIL_OP_KEEP;
    }
}

VkBlendFactor blendFactor(uint32_t value)
{
    switch (value)
    {
        case GL_ZERO: return VK_BLEND_FACTOR_ZERO;
        case GL_ONE: return VK_BLEND_FACTOR_ONE;
        case GL_SRC_COLOR: return VK_BLEND_FACTOR_SRC_COLOR;
        case GL_ONE_MINUS_SRC_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case GL_DST_COLOR: return VK_BLEND_FACTOR_DST_COLOR;
        case GL_ONE_MINUS_DST_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case GL_SRC_ALPHA: return VK_BLEND_FACTOR_SRC_ALPHA;
        case GL_ONE_MINUS_SRC_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case GL_DST_ALPHA: return VK_BLEND_FACTOR_DST_ALPHA;
        case GL_ONE_MINUS_DST_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case GL_CONSTANT_COLOR: return VK_BLEND_FACTOR_CONSTANT_COLOR;
        case GL_ONE_MINUS_CONSTANT_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        case GL_CONSTANT_ALPHA: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
        case GL_ONE_MINUS_CONSTANT_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
        case GL_SRC_ALPHA_SATURATE: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        default: return VK_BLEND_FACTOR_ONE;
    }
}

VkBlendOp blendOperation(uint32_t value)
{
    switch (value)
    {
        case GL_FUNC_SUBTRACT: return VK_BLEND_OP_SUBTRACT;
        case GL_FUNC_REVERSE_SUBTRACT: return VK_BLEND_OP_REVERSE_SUBTRACT;
        case GL_MIN: return VK_BLEND_OP_MIN;
        case GL_MAX: return VK_BLEND_OP_MAX;
        default: return VK_BLEND_OP_ADD;
    }
}

VkPrimitiveTopology topology(uint32_t value)
{
    switch (value)
    {
        case GL_POINTS: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
        case GL_LINES: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        case GL_LINE_STRIP: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
        case GL_TRIANGLE_STRIP: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        case GL_TRIANGLE_FAN: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
        default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}

VkPipeline pipeline(VulkanDrawState const &state)
{
    PipelineKey key = {};
    key.topology = state.topology;
    key.depthFunc = state.depthFunc;
    key.blendSrc = state.blendSrc;
    key.blendDst = state.blendDst;
    key.blendEquation = state.blendEquation;
    key.cullFace = state.cullFace;
    key.frontFace = state.frontFace;
    key.polygonMode = state.polygonMode;
    key.stencilFunc = state.stencilFunc;
    key.stencilFail = state.stencilFail;
    key.stencilDepthFail = state.stencilDepthFail;
    key.stencilPass = state.stencilPass;
    key.depthTest = state.depthTest;
    key.depthWrite = state.depthWrite;
    key.blend = state.blend;
    key.cull = state.cull;
    key.stencilTest = state.stencilTest;
    key.depthBias = state.depthBias;
    key.depthClamp = state.depthClamp;
    for (unsigned i = 0; i < 4; ++i) if (state.colorWrite[i]) key.colorMask |= 1u << i;
    auto existing = g.pipelines.find(key);
    if (existing != g.pipelines.end()) return existing->second;

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = g.vertexShader;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = g.fragmentShader;
    stages[1].pName = "main";
    VkBool32 const negativeOneToOne = g.depthClipControl ? VK_TRUE : VK_FALSE;
    VkSpecializationMapEntry const depthRangeEntry = { 0, 0, sizeof(negativeOneToOne) };
    VkSpecializationInfo const depthRangeSpecialization = { 1, &depthRangeEntry, sizeof(negativeOneToOne), &negativeOneToOne };
    stages[0].pSpecializationInfo = &depthRangeSpecialization;
    VkVertexInputBindingDescription binding = { 0, sizeof(VulkanVertex), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription attributes[5] = {
        { 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(VulkanVertex, position) },
        { 1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(VulkanVertex, color) },
        { 2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(VulkanVertex, texcoord[0]) },
        { 3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(VulkanVertex, texcoord[1]) },
        { 4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(VulkanVertex, texcoord[2]) },
    };
    VkPipelineVertexInputStateCreateInfo input = {};
    input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    input.vertexBindingDescriptionCount = 1;
    input.pVertexBindingDescriptions = &binding;
    input.vertexAttributeDescriptionCount = 5;
    input.pVertexAttributeDescriptions = attributes;
    VkPipelineInputAssemblyStateCreateInfo assembly = {};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = topology(state.topology);
    VkPipelineViewportStateCreateInfo viewport = {};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = viewport.scissorCount = 1;
#ifdef VK_EXT_depth_clip_control
    VkPipelineViewportDepthClipControlCreateInfoEXT depthClip = {};
    depthClip.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT;
    depthClip.negativeOneToOne = VK_TRUE;
    if (g.depthClipControl) viewport.pNext = &depthClip;
#endif
    VkPipelineRasterizationStateCreateInfo raster = {};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    if (g.features.fillModeNonSolid)
    {
        if (state.polygonMode == GL_LINE) raster.polygonMode = VK_POLYGON_MODE_LINE;
        if (state.polygonMode == GL_POINT) raster.polygonMode = VK_POLYGON_MODE_POINT;
    }
    raster.cullMode = VK_CULL_MODE_NONE;
    if (state.cull)
        raster.cullMode = state.cullFace == GL_FRONT ? VK_CULL_MODE_FRONT_BIT :
                          state.cullFace == GL_FRONT_AND_BACK ? VK_CULL_MODE_FRONT_AND_BACK : VK_CULL_MODE_BACK_BIT;
    // The shader's Y flip compensates for Vulkan's downward framebuffer Y.
    // A GL counter-clockwise primitive therefore remains a front-facing CCW
    // primitive under Vulkan's framebuffer winding convention.
    raster.frontFace = state.frontFace == GL_CW ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.depthBiasEnable = state.depthBias;
    raster.depthClampEnable = state.depthClamp && g.features.depthClamp;
    raster.lineWidth = 1.f;
    VkPipelineMultisampleStateCreateInfo samples = {};
    samples.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth = {};
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthTestEnable = state.depthTest;
    // OpenGL suppresses depth writes when its depth test is disabled.
    depth.depthWriteEnable = state.depthWrite && state.depthTest;
    depth.depthCompareOp = comparison(state.depthFunc);
    depth.stencilTestEnable = state.stencilTest;
    depth.front.failOp = stencilOperation(state.stencilFail);
    depth.front.passOp = stencilOperation(state.stencilPass);
    depth.front.depthFailOp = stencilOperation(state.stencilDepthFail);
    depth.front.compareOp = comparison(state.stencilFunc);
    depth.back = depth.front;
    VkPipelineColorBlendAttachmentState attachment = {};
    attachment.blendEnable = state.blend;
    attachment.srcColorBlendFactor = blendFactor(state.blendSrc);
    attachment.dstColorBlendFactor = blendFactor(state.blendDst);
    attachment.colorBlendOp = blendOperation(state.blendEquation);
    attachment.srcAlphaBlendFactor = attachment.srcColorBlendFactor;
    attachment.dstAlphaBlendFactor = attachment.dstColorBlendFactor;
    attachment.alphaBlendOp = attachment.colorBlendOp;
    attachment.colorWriteMask = key.colorMask;
    VkPipelineColorBlendStateCreateInfo blending = {};
    blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blending.attachmentCount = 1;
    blending.pAttachments = &attachment;
    VkDynamicState dynamics[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS,
        VK_DYNAMIC_STATE_LINE_WIDTH, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
        VK_DYNAMIC_STATE_STENCIL_REFERENCE };
    VkPipelineDynamicStateCreateInfo dynamic = {};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = sizeof(dynamics) / sizeof(dynamics[0]);
    dynamic.pDynamicStates = dynamics;
    VkGraphicsPipelineCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &samples;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &blending;
    info.pDynamicState = &dynamic;
    info.layout = g.pipelineLayout;
    info.renderPass = g.renderPass;
    VkPipeline result = VK_NULL_HANDLE;
    if (!check(vkCreateGraphicsPipelines(g.device, g.pipelineCache, 1, &info, nullptr, &result), "create graphics pipeline"))
        return VK_NULL_HANDLE;
    g.pipelines.emplace(key, result);
    return result;
}

VkSamplerAddressMode addressMode(uint32_t mode)
{
    if (mode == GL_REPEAT) return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    if (mode == GL_MIRRORED_REPEAT) return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    if (mode == GL_CLAMP_TO_BORDER) return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
}

VkSampler sampler(VulkanSamplerState const &state)
{
    SamplerKey key = {};
    key.minFilter = state.minFilter;
    key.magFilter = state.magFilter;
    key.wrapS = state.wrapS;
    key.wrapT = state.wrapT;
    std::memcpy(&key.anisotropy, &state.anisotropy, sizeof(float));
    std::memcpy(&key.minLod, &state.minLod, sizeof(float));
    std::memcpy(&key.maxLod, &state.maxLod, sizeof(float));
    auto existing = g.samplers.find(key);
    if (existing != g.samplers.end()) return existing->second;
    VkSamplerCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = state.magFilter == GL_NEAREST ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    info.minFilter = state.minFilter == GL_NEAREST || state.minFilter == GL_NEAREST_MIPMAP_NEAREST ||
                     state.minFilter == GL_NEAREST_MIPMAP_LINEAR ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    info.mipmapMode = state.minFilter == GL_NEAREST_MIPMAP_LINEAR || state.minFilter == GL_LINEAR_MIPMAP_LINEAR ?
                      VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = addressMode(state.wrapS);
    info.addressModeV = addressMode(state.wrapT);
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.anisotropyEnable = g.features.samplerAnisotropy && state.anisotropy > 1.f;
    info.maxAnisotropy = std::max(1.f, std::min(state.anisotropy, g.properties.limits.maxSamplerAnisotropy));
    info.minLod = std::max(0.f, state.minLod);
    info.maxLod = state.minFilter == GL_NEAREST || state.minFilter == GL_LINEAR ? info.minLod : std::max(info.minLod, state.maxLod);
    info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    VkSampler result = VK_NULL_HANDLE;
    if (!check(vkCreateSampler(g.device, &info, nullptr, &result), "create texture sampler")) return VK_NULL_HANDLE;
    g.samplers.emplace(key, result);
    return result;
}

VkRect2D clipRect(VulkanDrawState const &state)
{
    int x0 = 0, y0 = 0, x1 = static_cast<int>(g.extent.width), y1 = static_cast<int>(g.extent.height);
    if (state.scissorTest)
    {
        x0 = std::max(0, state.scissor[0]);
        y0 = std::max(0, static_cast<int>(g.extent.height) - state.scissor[1] - state.scissor[3]);
        x1 = std::min(x1, state.scissor[0] + state.scissor[2]);
        y1 = std::min(y1, static_cast<int>(g.extent.height) - state.scissor[1]);
    }
    return { { x0, y0 }, { static_cast<uint32_t>(std::max(0, x1 - x0)), static_cast<uint32_t>(std::max(0, y1 - y0)) } };
}

uint64_t elapsedNanoseconds(uint64_t start)
{
    return static_cast<uint64_t>((SDL_GetPerformanceCounter() - start) *
                                (1000000000.0 / SDL_GetPerformanceFrequency()));
}

bool workerCheck(RecordingWorker &worker, VkResult result, char const *operation)
{
    if (result == VK_SUCCESS) return true;
    worker.error = result;
    worker.operation = operation;
    return false;
}

bool allocateWorkerDescriptor(RecordingWorker &worker, VkDescriptorSet &set)
{
    if (worker.resources->descriptorSetsUsed == kWorkerDescriptorPageSets)
    {
        ++worker.resources->descriptorPoolIndex;
        worker.resources->descriptorSetsUsed = 0;
    }
    if (worker.resources->descriptorPoolIndex == worker.resources->descriptorPools.size())
    {
        VkDescriptorPoolSize sizes[] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kWorkerDescriptorPageSets },
                                        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kWorkerDescriptorPageSets * 5 } };
        VkDescriptorPoolCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        info.maxSets = kWorkerDescriptorPageSets;
        info.poolSizeCount = 2;
        info.pPoolSizes = sizes;
        VkDescriptorPool pool;
        if (!workerCheck(worker, vkCreateDescriptorPool(worker.context.device, &info, nullptr, &pool), "create worker descriptor page")) return false;
        worker.resources->descriptorPools.push_back(pool);
    }
    VkDescriptorSetAllocateInfo allocate = {};
    allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate.descriptorPool = worker.resources->descriptorPools[worker.resources->descriptorPoolIndex];
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &worker.context.descriptorLayout;
    if (!workerCheck(worker, vkAllocateDescriptorSets(worker.context.device, &allocate, &set), "allocate worker draw descriptors")) return false;
    ++worker.resources->descriptorSetsUsed;
    return true;
}

bool recordWorkerCommands(RecordingWorker &worker)
{
    auto const &context = worker.context;
    if (worker.resources->commandsUsed == worker.resources->commands.size())
    {
        VkCommandBufferAllocateInfo allocate = {};
        allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocate.commandPool = worker.resources->commandPool;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
        allocate.commandBufferCount = 1;
        VkCommandBuffer command;
        if (!workerCheck(worker, vkAllocateCommandBuffers(context.device, &allocate, &command), "allocate secondary command buffer"))
            return false;
        worker.resources->commands.push_back(command);
    }
    worker.result = worker.resources->commands[worker.resources->commandsUsed++];
    VkCommandBufferInheritanceInfo inheritance = {};
    inheritance.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
    inheritance.renderPass = context.renderPass;
    inheritance.framebuffer = context.framebuffer;
    inheritance.subpass = 0;
    VkCommandBufferBeginInfo begin = {};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT | VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
    begin.pInheritanceInfo = &inheritance;
    if (!workerCheck(worker, vkBeginCommandBuffer(worker.result, &begin), "begin secondary command buffer")) return false;

    for (size_t index = worker.first; index < worker.first + worker.count; ++index)
    {
        DrawPacket const &packet = context.packets[index];
        auto const &state = packet.state;
        std::memcpy(context.mapped + packet.vertexOffset, context.vertices + packet.firstVertex,
                    static_cast<size_t>(packet.vertexCount) * sizeof(VulkanVertex));
        std::memcpy(context.mapped + packet.uniformOffset, &state.uniforms, sizeof(VulkanUniforms));
        VkDescriptorSet set;
        if (!allocateWorkerDescriptor(worker, set)) return false;
        VkDescriptorBufferInfo uniform = { context.stream, packet.uniformOffset, sizeof(VulkanUniforms) };
        VkWriteDescriptorSet writes[6] = {};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = set;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].pBufferInfo = &uniform;
        for (unsigned i = 0; i < 5; ++i)
        {
            writes[i + 1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i + 1].dstSet = set;
            writes[i + 1].dstBinding = i + 1;
            writes[i + 1].descriptorCount = 1;
            writes[i + 1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i + 1].pImageInfo = &packet.images[i];
        }
        vkUpdateDescriptorSets(context.device, 6, writes, 0, nullptr);
        VkCommandBuffer command = worker.result;
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, packet.pipeline);
        vkCmdSetViewport(command, 0, 1, &packet.viewport);
        vkCmdSetScissor(command, 0, 1, &packet.scissor);
        vkCmdSetDepthBias(command, state.depthBiasUnits, 0.f, state.depthBiasFactor);
        vkCmdSetLineWidth(command, packet.lineWidth);
        vkCmdSetStencilCompareMask(command, VK_STENCIL_FACE_FRONT_AND_BACK, state.stencilCompareMask);
        vkCmdSetStencilWriteMask(command, VK_STENCIL_FACE_FRONT_AND_BACK, state.stencilWriteMask);
        vkCmdSetStencilReference(command, VK_STENCIL_FACE_FRONT_AND_BACK, state.stencilReference);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, context.pipelineLayout, 0, 1, &set, 0, nullptr);
        vkCmdBindVertexBuffers(command, 0, 1, &context.stream, &packet.vertexOffset);
        vkCmdDraw(command, packet.vertexCount, 1, 0, 0);
    }
    return workerCheck(worker, vkEndCommandBuffer(worker.result), "end secondary command buffer");
}

void recordWorker(RecordingWorker &worker)
{
    worker.threadId = static_cast<uint64_t>(SDL_ThreadID());
    worker.error = VK_SUCCESS;
    worker.operation = nullptr;
    worker.result = VK_NULL_HANDLE;
    uint64_t const start = SDL_GetPerformanceCounter();
    // The observed interval covers actual descriptor/stream preparation and
    // command recording, excluding thread creation, sleeping and queue waits.
    uint32_t const participants = worker.caller ? (recordingParticipants.fetch_or(1) | 1u) :
                                                          recordingParticipants.fetch_add(2) + 2;
    uint32_t const active = (participants >> 1) + (participants & 1);
    if ((participants & 1) && (participants >> 1)) batchCallerOverlap.store(true);
    uint32_t peak = batchPeakRecorders.load();
    while (active > peak && !batchPeakRecorders.compare_exchange_weak(peak, active)) {}
    recordWorkerCommands(worker);
    if (worker.caller) recordingParticipants.fetch_and(~1u);
    else recordingParticipants.fetch_sub(2);
    worker.elapsedNanoseconds = elapsedNanoseconds(start);
}

int SDLCALL recordingThread(void *argument)
{
    auto &worker = *static_cast<RecordingWorker *>(argument);
    for (;;)
    {
        SDL_SemWait(worker.start);
        if (worker.stop) break;
        recordWorker(worker);
        SDL_SemPost(worker.done);
    }
    return 0;
}

bool createRecordingWorkers()
{
    unsigned const cores = vulkanAvailableRecordingCores();
    // Keep the caller busy too: up to four background threads plus the caller,
    // with no more total recorders than cores available to this process.
    unsigned const limit = std::max(1u, std::min(5u, cores));
    unsigned count = limit;
    unsigned requestedCount = 0;
    char const *option = SDL_getenv("NBLOOD_VULKAN_WORKERS");
    if (option && *option)
    {
        char *end = nullptr;
        errno = 0;
        long const requested = std::strtol(option, &end, 10);
        if (errno || end == option || !end || *end || requested < 0 || static_cast<unsigned long>(requested) > UINT_MAX)
            return fail("NBLOOD_VULKAN_WORKERS must be 0 (automatic) or a positive total recorder count");
        if (requested)
        {
            requestedCount = static_cast<unsigned>(requested);
            count = std::min(limit, static_cast<unsigned>(requested));
            if (static_cast<unsigned long>(requested) > limit)
                std::fprintf(stderr, "Vulkan: requested %ld total recorders; using %u (available cores=%u, caller plus at most 4 background threads)\n",
                             requested, count, cores);
        }
    }
    g.recordingStats.workerCount = count;
    g.recordingStats.backgroundWorkerCount = count - 1;
    g.recordingStats.callerThreadId = static_cast<uint64_t>(SDL_ThreadID());
    g.packets.reserve(kDrawBatchSize);
    g.vertices.reserve(kDrawBatchVertexBytes / sizeof(VulkanVertex));
    std::fprintf(stderr, "Vulkan recording configuration: available_cores=%u workers=%u background_workers=%u requested=%u caller_thread_id=%llu\n",
                 cores, count, count - 1, requestedCount, static_cast<unsigned long long>(g.recordingStats.callerThreadId));
    return true;
}

bool ensureRecordingWorkers(size_t count)
{
    while (g.workers.size() < count)
    {
        size_t const index = g.workers.size();
        auto *worker = new (std::nothrow) RecordingWorker;
        if (!worker) return fail("Could not allocate a recording worker");
        worker->caller = index == 0;
        g.workers.push_back(worker);
        if (!worker->caller)
        {
            worker->start = SDL_CreateSemaphore(0);
            worker->done = SDL_CreateSemaphore(0);
            if (!worker->start || !worker->done) return fail(SDL_GetError());
            char name[32];
            std::snprintf(name, sizeof(name), "Vulkan record %u", static_cast<unsigned>(index));
            worker->thread = SDL_CreateThread(recordingThread, name, worker);
            if (!worker->thread) return fail(SDL_GetError());
            ++g.recordingStats.startedBackgroundWorkers;
        }
        ++g.recordingStats.startedWorkers;
        g.recordingStats.workerDraws.push_back(0);
        g.recordingStats.workerThreadIds.push_back(0);
        g.recordingStats.workerRecordNanoseconds.push_back(0);
    }
    auto &slot = currentSlot();
    while (slot.recorders.size() < count)
    {
        auto *resources = new (std::nothrow) RecordingResources;
        if (!resources) return fail("Could not allocate slot recorder resources");
        slot.recorders.push_back(resources);
        VkCommandPoolCreateInfo pool = {};
        pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pool.queueFamilyIndex = g.queueFamily;
        if (!check(vkCreateCommandPool(g.device, &pool, nullptr, &resources->commandPool), "create slot recorder command pool")) return false;
    }
    return true;
}

void destroyRecordingWorkers()
{
    // Jobs are joined at every flush; stop and join sleeping threads before
    // freeing anything they could reference, including failed startup cases.
    for (auto *worker : g.workers)
    {
        if (worker->thread)
        {
            worker->stop = true;
            SDL_SemPost(worker->start);
            SDL_WaitThread(worker->thread, nullptr);
        }
        if (worker->start) SDL_DestroySemaphore(worker->start);
        if (worker->done) SDL_DestroySemaphore(worker->done);
        delete worker;
    }
    g.workers.clear();
    recordingParticipants.store(0);
    batchPeakRecorders.store(0);
    batchCallerOverlap.store(false);
}

bool flushDrawPackets()
{
    if (g.failed) return false;
    if (g.packets.empty()) return true;
    size_t const count = std::min<size_t>(g.recordingStats.workerCount, (g.packets.size() + kDrawsPerWorker - 1) / kDrawsPerWorker);
    if (!count || !ensureRecordingWorkers(count)) return false;
    if (!beginRenderPass(VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS)) return false;
    RecordingContext context = { g.device, g.renderPass, g.framebuffer, g.pipelineLayout, g.descriptorLayout,
        currentSlot().stream.handle, static_cast<uint8_t *>(currentSlot().stream.mapped), g.packets.data(), g.vertices.data() };
    uint64_t const start = SDL_GetPerformanceCounter();
    batchPeakRecorders.store(0);
    batchCallerOverlap.store(false);
    for (size_t index = 0; index < count; ++index)
    {
        auto &worker = *g.workers[index];
        worker.resources = currentSlot().recorders[index];
        worker.context = context;
        worker.first = g.packets.size() * index / count;
        worker.count = g.packets.size() * (index + 1) / count - worker.first;
    }
    // Launch every background slice before recording the first contiguous
    // slice on the caller. Tiny batches have only slot 0 and start no threads.
    for (size_t index = 1; index < count; ++index)
        SDL_SemPost(g.workers[index]->start);
    recordWorker(*g.workers[0]);
    // Always join every assigned job, even if an earlier job failed. Only this
    // producer publishes errors to g.failed or mutates shared resource maps.
    for (size_t index = 1; index < count; ++index)
        SDL_SemWait(g.workers[index]->done);
    g.recordingStats.dispatchNanoseconds += elapsedNanoseconds(start);
    uint32_t const peak = batchPeakRecorders.load();
    g.recordingStats.peakActiveRecorders = std::max(g.recordingStats.peakActiveRecorders, peak);
    ++g.recordingStats.batches;
    if (peak > 1) ++g.recordingStats.parallelBatches;
    if (batchCallerOverlap.load()) ++g.recordingStats.callerParallelBatches;
    std::vector<VkCommandBuffer> commands(count);
    for (size_t index = 0; index < count; ++index)
    {
        auto const &worker = *g.workers[index];
        g.recordingStats.workerThreadIds[index] = worker.threadId;
        g.recordingStats.workerRecordNanoseconds[index] += worker.elapsedNanoseconds;
        if (worker.error != VK_SUCCESS) check(worker.error, worker.operation);
        else
        {
            g.recordingStats.workerDraws[index] += worker.count;
            g.recordingStats.draws += worker.count;
            ++g.recordingStats.secondaryBuffers;
        }
        commands[index] = worker.result;
    }
    if (g.failed) return false;
    // Contiguous ranges execute in original frontend order, never completion
    // order. This preserves blending, depth/stencil updates and masked clears.
    vkCmdExecuteCommands(currentSlot().command, static_cast<uint32_t>(count), commands.data());
    g.packets.clear();
    g.vertices.clear();
    return true;
}

bool createRenderPass()
{
    VkAttachmentDescription attachments[2] = {};
    attachments[0].format = VK_FORMAT_R8G8B8A8_UNORM;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = attachments[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments[1].format = g.depthFormat;
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp = attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachments[1].storeOp = attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[1].initialLayout = attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference color = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference depth = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color;
    subpass.pDepthStencilAttachment = &depth;
    VkSubpassDependency dependencies[2] = {};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                   VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                   VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                   VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    VkRenderPassCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = 2;
    info.pAttachments = attachments;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 2;
    info.pDependencies = dependencies;
    return check(vkCreateRenderPass(g.device, &info, nullptr, &g.renderPass), "create render pass");
}

bool createAmbientOcclusionResources()
{
    if (!g.ao.supported) return true;
    VkDescriptorSetLayoutBinding bindings[3] = {};
    for (uint32_t i = 0; i < 3; ++i)
    {
        bindings[i].binding = i;
        bindings[i].descriptorType = i ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo descriptor = {};
    descriptor.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    descriptor.bindingCount = 3;
    descriptor.pBindings = bindings;
    if (!check(vkCreateDescriptorSetLayout(g.device, &descriptor, nullptr, &g.ao.descriptorLayout), "create AO descriptor layout")) return false;
    VkPipelineLayoutCreateInfo layout = {};
    layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &g.ao.descriptorLayout;
    if (!check(vkCreatePipelineLayout(g.device, &layout, nullptr, &g.ao.pipelineLayout), "create AO pipeline layout")) return false;

    // Depth is sampled, never simultaneously attached by this pass. Color is
    // multiplied through blending, avoiding a scene-color copy or feedback loop.
    VkAttachmentDescription attachment = {};
    attachment.format = VK_FORMAT_R8G8B8A8_UNORM;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference color = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color;
    VkSubpassDependency dependencies[2] = {};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    for (auto &dependency : dependencies)
    {
        dependency.srcStageMask = dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    }
    VkRenderPassCreateInfo renderPass = {};
    renderPass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPass.attachmentCount = 1;
    renderPass.pAttachments = &attachment;
    renderPass.subpassCount = 1;
    renderPass.pSubpasses = &subpass;
    renderPass.dependencyCount = 2;
    renderPass.pDependencies = dependencies;
    if (!check(vkCreateRenderPass(g.device, &renderPass, nullptr, &g.ao.renderPass), "create AO render pass")) return false;

    VkShaderModuleCreateInfo shader = {};
    shader.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shader.codeSize = sizeof(ambient_occlusion_vk_vert_spv);
    shader.pCode = ambient_occlusion_vk_vert_spv;
    if (!check(vkCreateShaderModule(g.device, &shader, nullptr, &g.ao.vertexShader), "create AO vertex shader")) return false;
    shader.codeSize = sizeof(ambient_occlusion_vk_frag_spv);
    shader.pCode = ambient_occlusion_vk_frag_spv;
    if (!check(vkCreateShaderModule(g.device, &shader, nullptr, &g.ao.fragmentShader), "create AO fragment shader")) return false;
    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = g.ao.vertexShader;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = g.ao.fragmentShader;
    stages[0].pName = stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vertex = {};
    vertex.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo assembly = {};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport = {};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster = {};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.f;
    VkPipelineMultisampleStateCreateInfo samples = {};
    samples.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend = {};
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_COLOR;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
    VkPipelineColorBlendStateCreateInfo blending = {};
    blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blending.attachmentCount = 1;
    blending.pAttachments = &blend;
    VkDynamicState dynamics[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamic = {};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamics;
    VkGraphicsPipelineCreateInfo pipeline = {};
    pipeline.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline.stageCount = 2;
    pipeline.pStages = stages;
    pipeline.pVertexInputState = &vertex;
    pipeline.pInputAssemblyState = &assembly;
    pipeline.pViewportState = &viewport;
    pipeline.pRasterizationState = &raster;
    pipeline.pMultisampleState = &samples;
    pipeline.pColorBlendState = &blending;
    pipeline.pDynamicState = &dynamic;
    pipeline.layout = g.ao.pipelineLayout;
    pipeline.renderPass = g.ao.renderPass;
    if (!check(vkCreateGraphicsPipelines(g.device, g.pipelineCache, 1, &pipeline, nullptr, &g.ao.pipeline), "create AO pipeline")) return false;
    VkSamplerCreateInfo sampler = {};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    return check(vkCreateSampler(g.device, &sampler, nullptr, &g.ao.sampler), "create AO sampler");
}

bool createRealtimeLightingResources()
{
    // Keep the scene's chosen depth format unchanged. Optional lighting must
    // never change depth rounding on devices lacking sampled depth support.
    if (!g.ao.supported) return true;
    VkFormatFeatureFlags const required = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    for (VkFormat format : { VK_FORMAT_D32_SFLOAT, VK_FORMAT_D16_UNORM })
    {
        VkFormatProperties properties;
        VkImageFormatProperties imageProperties;
        vkGetPhysicalDeviceFormatProperties(g.physical, format, &properties);
        if ((properties.optimalTilingFeatures & required) == required &&
            vkGetPhysicalDeviceImageFormatProperties(g.physical, format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0, &imageProperties) == VK_SUCCESS &&
            imageProperties.maxExtent.width >= kShadowFaceSize * kVulkanMaxLights &&
            imageProperties.maxExtent.height >= kShadowFaceSize * kShadowFaces)
        {
            g.lighting.shadowFormat = format;
            break;
        }
    }
    if (g.lighting.shadowFormat == VK_FORMAT_UNDEFINED) return true;

    VkDescriptorSetLayoutBinding bindings[4] = {};
    for (uint32_t i = 0; i < 4; ++i)
    {
        bindings[i].binding = i;
        bindings[i].descriptorType = i ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo descriptor = {};
    descriptor.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    descriptor.bindingCount = 4;
    descriptor.pBindings = bindings;
    if (!check(vkCreateDescriptorSetLayout(g.device, &descriptor, nullptr, &g.lighting.descriptorLayout), "create lighting descriptor layout")) return false;
    VkPipelineLayoutCreateInfo layout = {};
    layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &g.lighting.descriptorLayout;
    if (!check(vkCreatePipelineLayout(g.device, &layout, nullptr, &g.lighting.pipelineLayout), "create lighting pipeline layout")) return false;
    VkPushConstantRange push = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ShadowPush) };
    layout.setLayoutCount = 0;
    layout.pSetLayouts = nullptr;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &push;
    if (!check(vkCreatePipelineLayout(g.device, &layout, nullptr, &g.lighting.shadowLayout), "create shadow pipeline layout")) return false;

    VkAttachmentDescription attachment = {};
    attachment.format = g.lighting.shadowFormat;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    // Discard old texels, but the dependency still waits for preceding lighting
    // reads before overwriting this shared atlas, including across submissions.
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    VkAttachmentReference depthReference = { 0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.pDepthStencilAttachment = &depthReference;
    VkSubpassDependency dependencies[2] = {};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    VkRenderPassCreateInfo renderPass = {};
    renderPass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPass.attachmentCount = 1;
    renderPass.pAttachments = &attachment;
    renderPass.subpassCount = 1;
    renderPass.pSubpasses = &subpass;
    renderPass.dependencyCount = 2;
    renderPass.pDependencies = dependencies;
    if (!check(vkCreateRenderPass(g.device, &renderPass, nullptr, &g.lighting.shadowRenderPass), "create shadow render pass")) return false;
    if (!createImage(g.lighting.atlas, kShadowFaceSize * kVulkanMaxLights, kShadowFaceSize * kShadowFaces, 1,
                     g.lighting.shadowFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                     VK_IMAGE_ASPECT_DEPTH_BIT)) return false;
    VkFramebufferCreateInfo framebuffer = {};
    framebuffer.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebuffer.renderPass = g.lighting.shadowRenderPass;
    framebuffer.attachmentCount = 1;
    framebuffer.pAttachments = &g.lighting.atlas.view;
    framebuffer.width = g.lighting.atlas.width;
    framebuffer.height = g.lighting.atlas.height;
    framebuffer.layers = 1;
    if (!check(vkCreateFramebuffer(g.device, &framebuffer, nullptr, &g.lighting.shadowFramebuffer), "create shadow framebuffer")) return false;

    VkShaderModuleCreateInfo shader = {};
    shader.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shader.codeSize = sizeof(realtime_lighting_vk_vert_spv);
    shader.pCode = realtime_lighting_vk_vert_spv;
    if (!check(vkCreateShaderModule(g.device, &shader, nullptr, &g.lighting.vertexShader), "create lighting vertex shader")) return false;
    shader.codeSize = sizeof(realtime_lighting_vk_frag_spv);
    shader.pCode = realtime_lighting_vk_frag_spv;
    if (!check(vkCreateShaderModule(g.device, &shader, nullptr, &g.lighting.fragmentShader), "create lighting fragment shader")) return false;
    shader.codeSize = sizeof(realtime_shadow_vk_vert_spv);
    shader.pCode = realtime_shadow_vk_vert_spv;
    if (!check(vkCreateShaderModule(g.device, &shader, nullptr, &g.lighting.shadowVertexShader), "create shadow vertex shader")) return false;
    shader.codeSize = sizeof(realtime_shadow_vk_frag_spv);
    shader.pCode = realtime_shadow_vk_frag_spv;
    if (!check(vkCreateShaderModule(g.device, &shader, nullptr, &g.lighting.shadowFragmentShader), "create shadow fragment shader")) return false;
    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = g.lighting.vertexShader;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = g.lighting.fragmentShader;
    stages[0].pName = stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vertex = {};
    vertex.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo assembly = {};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport = {};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster = {};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.f;
    VkPipelineMultisampleStateCreateInfo samples = {};
    samples.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend = {};
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
    VkPipelineColorBlendStateCreateInfo blending = {};
    blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blending.attachmentCount = 1;
    blending.pAttachments = &blend;
    VkDynamicState dynamics[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamic = {};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamics;
    VkGraphicsPipelineCreateInfo pipeline = {};
    pipeline.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline.stageCount = 2;
    pipeline.pStages = stages;
    pipeline.pVertexInputState = &vertex;
    pipeline.pInputAssemblyState = &assembly;
    pipeline.pViewportState = &viewport;
    pipeline.pRasterizationState = &raster;
    pipeline.pMultisampleState = &samples;
    pipeline.pColorBlendState = &blending;
    pipeline.pDynamicState = &dynamic;
    pipeline.layout = g.lighting.pipelineLayout;
    pipeline.renderPass = g.ao.renderPass;
    if (!check(vkCreateGraphicsPipelines(g.device, g.pipelineCache, 1, &pipeline, nullptr, &g.lighting.pipeline), "create lighting pipeline")) return false;

    VkVertexInputBindingDescription binding = { 0, sizeof(VulkanShadowVertex), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription attribute = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 };
    vertex.vertexBindingDescriptionCount = 1;
    vertex.pVertexBindingDescriptions = &binding;
    vertex.vertexAttributeDescriptionCount = 1;
    vertex.pVertexAttributeDescriptions = &attribute;
    stages[0].module = g.lighting.shadowVertexShader;
    stages[1].module = g.lighting.shadowFragmentShader;
    blending.attachmentCount = 0;
    blending.pAttachments = nullptr;
    VkPipelineDepthStencilStateCreateInfo depth = {};
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthTestEnable = depth.depthWriteEnable = VK_TRUE;
    depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    pipeline.pDepthStencilState = &depth;
    pipeline.layout = g.lighting.shadowLayout;
    pipeline.renderPass = g.lighting.shadowRenderPass;
    if (!check(vkCreateGraphicsPipelines(g.device, g.pipelineCache, 1, &pipeline, nullptr, &g.lighting.shadowPipeline), "create shadow pipeline")) return false;
    g.lighting.supported = true;
    return true;
}

bool createSubmissionSlots()
{
    unsigned count = 2;
    char const *option = SDL_getenv("NBLOOD_VULKAN_FRAMES_IN_FLIGHT");
    if (option && *option)
    {
        char *end = nullptr;
        errno = 0;
        long const requested = std::strtol(option, &end, 10);
        if (errno || end == option || !end || *end || requested < 1 || requested > 3)
            return fail("NBLOOD_VULKAN_FRAMES_IN_FLIGHT must be 1, 2 or 3");
        count = static_cast<unsigned>(requested);
    }
    g.submissionStats.framesInFlight = count;
    std::fprintf(stderr, "Vulkan submission configuration: frames_in_flight=%u\n", count);
    for (unsigned index = 0; index < count; ++index)
    {
        auto *slot = new (std::nothrow) SubmissionSlot;
        if (!slot) return fail("Could not allocate submission slot");
        g.slots.push_back(slot);
        VkCommandPoolCreateInfo pool = {};
        pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pool.queueFamilyIndex = g.queueFamily;
        if (!check(vkCreateCommandPool(g.device, &pool, nullptr, &slot->commandPool), "create submission command pool")) return false;
        VkCommandBufferAllocateInfo command = {};
        command.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        command.commandPool = slot->commandPool;
        command.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        command.commandBufferCount = 1;
        if (!check(vkAllocateCommandBuffers(g.device, &command, &slot->command), "allocate submission command buffer")) return false;
        VkFenceCreateInfo fence = {};
        fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (!check(vkCreateFence(g.device, &fence, nullptr, &slot->fence), "create submission fence")) return false;
        VkSemaphoreCreateInfo semaphore = {};
        semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (!check(vkCreateSemaphore(g.device, &semaphore, nullptr, &slot->acquired), "create slot acquire semaphore")) return false;
    }
    return true;
}

void destroySubmissionSlots()
{
    for (auto *slot : g.slots)
    {
        for (auto *resources : slot->recorders)
        {
            if (resources->commandPool) vkDestroyCommandPool(g.device, resources->commandPool, nullptr);
            for (auto pool : resources->descriptorPools) vkDestroyDescriptorPool(g.device, pool, nullptr);
            delete resources;
        }
        destroyBuffer(slot->stream);
        if (slot->acquired) vkDestroySemaphore(g.device, slot->acquired, nullptr);
        if (slot->fence) vkDestroyFence(g.device, slot->fence, nullptr);
        if (slot->commandPool) vkDestroyCommandPool(g.device, slot->commandPool, nullptr);
        delete slot;
    }
    g.slots.clear();
}

void destroyTargets()
{
    if (g.ao.framebuffer) vkDestroyFramebuffer(g.device, g.ao.framebuffer, nullptr);
    if (g.ao.depthView) vkDestroyImageView(g.device, g.ao.depthView, nullptr);
    if (g.ao.stencilView) vkDestroyImageView(g.device, g.ao.stencilView, nullptr);
    g.ao.framebuffer = VK_NULL_HANDLE;
    g.ao.depthView = g.ao.stencilView = VK_NULL_HANDLE;
    if (g.framebuffer) vkDestroyFramebuffer(g.device, g.framebuffer, nullptr);
    g.framebuffer = VK_NULL_HANDLE;
    destroyImage(g.color);
    destroyImage(g.depth);
    if (g.swapchain) vkDestroySwapchainKHR(g.device, g.swapchain, nullptr);
    g.swapchain = VK_NULL_HANDLE;
    g.swapImages.clear();
    g.swapInitialized.clear();
    for (auto semaphore : g.presentationSemaphores)
        if (semaphore) vkDestroySemaphore(g.device, semaphore, nullptr);
    g.presentationSemaphores.clear();
}

bool createTargets(int width, int height)
{
    if (width <= 0 || height <= 0)
    {
        g.suspended = true;
        return true;
    }
    VkSurfaceCapabilitiesKHR caps;
    if (!check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g.physical, g.surface, &caps), "get surface capabilities")) return false;
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
        return fail("The Vulkan surface cannot receive the rendered image");
    uint32_t count = 0;
    if (!check(vkGetPhysicalDeviceSurfaceFormatsKHR(g.physical, g.surface, &count, nullptr), "get surface formats")) return false;
    std::vector<VkSurfaceFormatKHR> formats(count);
    if (!count) return fail("The Vulkan surface has no supported formats");
    if (!check(vkGetPhysicalDeviceSurfaceFormatsKHR(g.physical, g.surface, &count, formats.data()), "get surface formats")) return false;
    VkSurfaceFormatKHR format = formats[0];
    if (format.format == VK_FORMAT_UNDEFINED) format.format = VK_FORMAT_B8G8R8A8_UNORM;
    for (auto const &candidate : formats)
        if (candidate.format == VK_FORMAT_B8G8R8A8_UNORM || candidate.format == VK_FORMAT_R8G8B8A8_UNORM)
        {
            format = candidate;
            break;
        }
    VkFormatProperties properties;
    vkGetPhysicalDeviceFormatProperties(g.physical, format.format, &properties);
    if (!(properties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) return fail("Surface format does not support presentation blits");
    if (!check(vkGetPhysicalDeviceSurfacePresentModesKHR(g.physical, g.surface, &count, nullptr), "get present modes")) return false;
    std::vector<VkPresentModeKHR> modes(count);
    if (!check(vkGetPhysicalDeviceSurfacePresentModesKHR(g.physical, g.surface, &count, modes.data()), "get present modes")) return false;
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    if (g.vsync <= 0)
    {
        if (std::find(modes.begin(), modes.end(), VK_PRESENT_MODE_MAILBOX_KHR) != modes.end()) presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
        else if (std::find(modes.begin(), modes.end(), VK_PRESENT_MODE_IMMEDIATE_KHR) != modes.end()) presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    }
    if (g.vsync < 0 && std::find(modes.begin(), modes.end(), VK_PRESENT_MODE_FIFO_RELAXED_KHR) != modes.end())
        presentMode = VK_PRESENT_MODE_FIFO_RELAXED_KHR;
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX)
    {
        extent.width = std::max(caps.minImageExtent.width, std::min(caps.maxImageExtent.width, static_cast<uint32_t>(width)));
        extent.height = std::max(caps.minImageExtent.height, std::min(caps.maxImageExtent.height, static_cast<uint32_t>(height)));
    }
    if (!extent.width || !extent.height)
    {
        g.suspended = true;
        return true;
    }
    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount && imageCount > caps.maxImageCount) imageCount = caps.maxImageCount;
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    for (uint32_t bit = 1; !(caps.supportedCompositeAlpha & alpha) && bit <= 8; bit <<= 1)
        alpha = static_cast<VkCompositeAlphaFlagBitsKHR>(bit);
    VkSwapchainCreateInfoKHR info = {};
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface = g.surface;
    info.minImageCount = imageCount;
    info.imageFormat = format.format;
    info.imageColorSpace = format.colorSpace;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = alpha;
    info.presentMode = presentMode;
    info.clipped = VK_TRUE;
    if (!check(vkCreateSwapchainKHR(g.device, &info, nullptr, &g.swapchain), "create swapchain")) return false;
    if (!check(vkGetSwapchainImagesKHR(g.device, g.swapchain, &imageCount, nullptr), "get swapchain images")) return false;
    g.swapImages.resize(imageCount);
    g.swapInitialized.assign(imageCount, false);
    if (!check(vkGetSwapchainImagesKHR(g.device, g.swapchain, &imageCount, g.swapImages.data()), "get swapchain images")) return false;
    // A frame fence does not prove the presentation engine consumed its wait
    // semaphore. Reuse presentation semaphores only when that IMAGE is acquired
    // again, rather than when a submission slot becomes fence-complete.
    g.presentationSemaphores.resize(imageCount, VK_NULL_HANDLE);
    VkSemaphoreCreateInfo semaphore = {};
    semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (auto &handle : g.presentationSemaphores)
        if (!check(vkCreateSemaphore(g.device, &semaphore, nullptr, &handle), "create image presentation semaphore")) return false;
    g.extent = extent;
    g.surfaceFormat = format.format;
    g.swapchainVsync = g.vsync;
    if (!createImage(g.color, extent.width, extent.height, 1, VK_FORMAT_R8G8B8A8_UNORM,
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                     VK_IMAGE_ASPECT_COLOR_BIT)) return false;
    if (!createImage(g.depth, extent.width, extent.height, 1, g.depthFormat,
                     VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                         (g.ao.supported ? VK_IMAGE_USAGE_SAMPLED_BIT : 0),
                     VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) return false;
    VkImageView attachments[] = { g.color.view, g.depth.view };
    VkFramebufferCreateInfo framebuffer = {};
    framebuffer.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebuffer.renderPass = g.renderPass;
    framebuffer.attachmentCount = 2;
    framebuffer.pAttachments = attachments;
    framebuffer.width = extent.width;
    framebuffer.height = extent.height;
    framebuffer.layers = 1;
    if (!check(vkCreateFramebuffer(g.device, &framebuffer, nullptr, &g.framebuffer), "create framebuffer")) return false;
    if (g.ao.supported)
    {
        // Vulkan exposes the depth and stencil channels through distinct
        // aspect views. Stencil is sampled as unsigned integers by the AO mask.
        VkImageViewCreateInfo view = {};
        view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image = g.depth.handle;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = g.depthFormat;
        view.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
        if (!check(vkCreateImageView(g.device, &view, nullptr, &g.ao.depthView), "create AO depth view")) return false;
        view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
        if (!check(vkCreateImageView(g.device, &view, nullptr, &g.ao.stencilView), "create AO stencil view")) return false;
        framebuffer.renderPass = g.ao.renderPass;
        framebuffer.attachmentCount = 1;
        framebuffer.pAttachments = &g.color.view;
        if (!check(vkCreateFramebuffer(g.device, &framebuffer, nullptr, &g.ao.framebuffer), "create AO framebuffer")) return false;
    }
    if (!beginCommands()) return false;
    imageBarrier(g.color.handle, VK_IMAGE_ASPECT_COLOR_BIT, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkClearColorValue black = {};
    black.float32[3] = 1.f;
    VkImageSubresourceRange colorRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdClearColorImage(currentSlot().command, g.color.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &colorRange);
    imageBarrier(g.color.handle, VK_IMAGE_ASPECT_COLOR_BIT, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    VkImageAspectFlags aspects = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    imageBarrier(g.depth.handle, aspects, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkClearDepthStencilValue clearDepth = { 1.f, 0 };
    VkImageSubresourceRange depthRange = { aspects, 0, 1, 0, 1 };
    vkCmdClearDepthStencilImage(currentSlot().command, g.depth.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearDepth, 1, &depthRange);
    imageBarrier(g.depth.handle, aspects, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
    g.suspended = false;
    return true;
}

bool initialize(SDL_Window *window, int width, int height, int vsync)
{
    g.window = window;
    g.vsync = vsync;
    if (!loadGlobalDispatch()) return false;
    unsigned extensionCount = 0;
    if (!SDL_Vulkan_GetInstanceExtensions(window, &extensionCount, nullptr)) return fail(SDL_GetError());
    std::vector<char const *> extensions(extensionCount);
    if (!SDL_Vulkan_GetInstanceExtensions(window, &extensionCount, extensions.data())) return fail(SDL_GetError());
    uint32_t availableCount = 0;
    if (!check(vkEnumerateInstanceExtensionProperties(nullptr, &availableCount, nullptr), "enumerate instance extensions")) return false;
    std::vector<VkExtensionProperties> availableExtensions(availableCount);
    if (!check(vkEnumerateInstanceExtensionProperties(nullptr, &availableCount, availableExtensions.data()), "enumerate instance extensions")) return false;
    auto addInstanceExtension = [&](char const *name) {
        bool available = false;
        for (auto const &extension : availableExtensions)
            if (!std::strcmp(extension.extensionName, name)) available = true;
        if (!available) return false;
        for (auto const *extension : extensions)
            if (!std::strcmp(extension, name)) return true;
        extensions.push_back(name);
        return true;
    };
    // MoltenVK devices are exposed through the loader's portability opt-in.
    // Enable only advertised extensions; Linux/Windows loaders need neither.
    VkInstanceCreateFlags instanceFlags = 0;
#ifdef VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME
    if (addInstanceExtension(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME))
        instanceFlags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
#endif
    addInstanceExtension(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
    VkApplicationInfo application = {};
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.pApplicationName = "NBlood native Vulkan";
    application.applicationVersion = 1;
    application.pEngineName = "Build Polymost";
    application.engineVersion = 1;
    application.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo instance = {};
    instance.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance.flags = instanceFlags;
    instance.pApplicationInfo = &application;
    instance.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    instance.ppEnabledExtensionNames = extensions.data();
    if (!check(vkCreateInstance(&instance, nullptr, &g.instance), "create instance")) return false;
    if (!loadInstanceDispatch()) return false;
    if (!SDL_Vulkan_CreateSurface(window, g.instance, &g.surface)) return fail(SDL_GetError());
    uint32_t count = 0;
    if (!check(vkEnumeratePhysicalDevices(g.instance, &count, nullptr), "enumerate physical devices")) return false;
    std::vector<VkPhysicalDevice> physicals(count);
    if (!count) return fail("No Vulkan physical device is available");
    if (!check(vkEnumeratePhysicalDevices(g.instance, &count, physicals.data()), "enumerate physical devices")) return false;
    int bestScore = -1;
    for (VkPhysicalDevice physical : physicals)
    {
        uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, families.data());
        uint32_t deviceExtensionCount = 0;
        if (vkEnumerateDeviceExtensionProperties(physical, nullptr, &deviceExtensionCount, nullptr) != VK_SUCCESS) continue;
        std::vector<VkExtensionProperties> deviceExtensions(deviceExtensionCount);
        if (vkEnumerateDeviceExtensionProperties(physical, nullptr, &deviceExtensionCount, deviceExtensions.data()) != VK_SUCCESS) continue;
        bool hasSwapchain = false;
        bool hasPortabilitySubset = false;
        bool hasDepthClipControl = false;
        for (auto const &extension : deviceExtensions)
        {
            if (!std::strcmp(extension.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) hasSwapchain = true;
            if (!std::strcmp(extension.extensionName, "VK_KHR_portability_subset")) hasPortabilitySubset = true;
#ifdef VK_EXT_depth_clip_control
            if (!std::strcmp(extension.extensionName, VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME)) hasDepthClipControl = true;
#endif
        }
        if (!hasSwapchain) continue;
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(physical, &properties);
        int score = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 30 :
                    properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 20 : 10;
        for (uint32_t family = 0; family < familyCount; ++family)
        {
            VkBool32 present = VK_FALSE;
            if (vkGetPhysicalDeviceSurfaceSupportKHR(physical, family, g.surface, &present) != VK_SUCCESS) continue;
            if (present && families[family].queueCount && (families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) && score > bestScore)
            {
                bestScore = score;
                g.physical = physical;
                g.queueFamily = family;
                g.properties = properties;
                g.portabilitySubset = hasPortabilitySubset;
                g.depthClipControl = hasDepthClipControl;
            }
        }
    }
    if (!g.physical) return fail("No Vulkan device supports graphics and presentation to this window");
    vkGetPhysicalDeviceFeatures(g.physical, &g.features);
    vkGetPhysicalDeviceMemoryProperties(g.physical, &g.memory);
#ifdef VK_EXT_depth_clip_control
    VkPhysicalDeviceDepthClipControlFeaturesEXT depthClipFeatures = {};
    depthClipFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT;
    if (g.depthClipControl)
    {
        // Keep OpenGL's clip-space depth until the viewport transform when the
        // driver supports it. An earlier shader z=(z+w)/2 conversion can round
        // differently for coplanar geometry, despite the same D24 framebuffer.
        auto getFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2KHR>(
            vkGetInstanceProcAddr(g.instance, "vkGetPhysicalDeviceFeatures2KHR"));
        VkPhysicalDeviceFeatures2KHR features = {};
        features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2_KHR;
        features.pNext = &depthClipFeatures;
        if (getFeatures2) getFeatures2(g.physical, &features);
        g.depthClipControl = getFeatures2 && depthClipFeatures.depthClipControl;
    }
#endif
    VkPhysicalDeviceFeatures enabled = {};
    enabled.samplerAnisotropy = g.features.samplerAnisotropy;
    enabled.fillModeNonSolid = g.features.fillModeNonSolid;
    enabled.wideLines = g.features.wideLines;
    enabled.depthClamp = g.features.depthClamp;
    float priority = 1.f;
    VkDeviceQueueCreateInfo queue = {};
    queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue.queueFamilyIndex = g.queueFamily;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    std::vector<char const *> deviceExtensions = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    if (g.portabilitySubset) deviceExtensions.push_back("VK_KHR_portability_subset");
#ifdef VK_EXT_depth_clip_control
    if (g.depthClipControl) deviceExtensions.push_back(VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME);
#endif
    VkDeviceCreateInfo device = {};
    device.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device.queueCreateInfoCount = 1;
    device.pQueueCreateInfos = &queue;
    device.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
    device.ppEnabledExtensionNames = deviceExtensions.data();
    device.pEnabledFeatures = &enabled;
#ifdef VK_EXT_depth_clip_control
    if (g.depthClipControl) device.pNext = &depthClipFeatures;
#endif
    if (!check(vkCreateDevice(g.physical, &device, nullptr, &g.device), "create logical device")) return false;
    if (!loadDeviceDispatch()) return false;
    vkGetDeviceQueue(g.device, g.queueFamily, 0, &g.queue);
    VkFormatProperties colorProperties;
    vkGetPhysicalDeviceFormatProperties(g.physical, VK_FORMAT_R8G8B8A8_UNORM, &colorProperties);
    VkFormatFeatureFlags colorRequired = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if ((colorProperties.optimalTilingFeatures & colorRequired) != colorRequired) return fail("RGBA8 rendering and texture sampling are unavailable");
    // Match Polymost's requested 24-bit GL depth buffer where supported.
    for (VkFormat format : { VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT })
    {
        VkFormatProperties properties;
        vkGetPhysicalDeviceFormatProperties(g.physical, format, &properties);
        VkImageFormatProperties imageProperties;
        VkResult supported = vkGetPhysicalDeviceImageFormatProperties(g.physical, format, VK_IMAGE_TYPE_2D,
            VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            0, &imageProperties);
        if (supported == VK_SUCCESS && (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))
        {
            g.depthFormat = format;
            break;
        }
    }
    if (g.depthFormat == VK_FORMAT_UNDEFINED) return fail("No depth/stencil attachment format is available");
    {
        // Keep the preferred GL-compatible depth format even when sampling is
        // unavailable. AO is optional and must not alter AO-off depth rounding.
        VkFormatProperties properties;
        VkImageFormatProperties imageProperties;
        vkGetPhysicalDeviceFormatProperties(g.physical, g.depthFormat, &properties);
        g.ao.supported = (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) &&
            vkGetPhysicalDeviceImageFormatProperties(g.physical, g.depthFormat, VK_IMAGE_TYPE_2D,
                VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0, &imageProperties) == VK_SUCCESS;
    }
    VkDescriptorSetLayoutBinding bindings[6] = {};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    for (uint32_t i = 1; i < 6; ++i)
    {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo descriptorLayout = {};
    descriptorLayout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    descriptorLayout.bindingCount = 6;
    descriptorLayout.pBindings = bindings;
    if (!check(vkCreateDescriptorSetLayout(g.device, &descriptorLayout, nullptr, &g.descriptorLayout), "create descriptor layout")) return false;
    VkPipelineLayoutCreateInfo pipelineLayout = {};
    pipelineLayout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayout.setLayoutCount = 1;
    pipelineLayout.pSetLayouts = &g.descriptorLayout;
    if (!check(vkCreatePipelineLayout(g.device, &pipelineLayout, nullptr, &g.pipelineLayout), "create pipeline layout")) return false;
    VkPipelineCacheCreateInfo cache = {};
    cache.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    if (!check(vkCreatePipelineCache(g.device, &cache, nullptr, &g.pipelineCache), "create pipeline cache")) return false;
    VkShaderModuleCreateInfo shader = {};
    shader.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shader.codeSize = sizeof(vulkan_polymost_vert_spv);
    shader.pCode = vulkan_polymost_vert_spv;
    if (!check(vkCreateShaderModule(g.device, &shader, nullptr, &g.vertexShader), "create vertex shader")) return false;
    shader.codeSize = sizeof(vulkan_polymost_frag_spv);
    shader.pCode = vulkan_polymost_frag_spv;
    if (!check(vkCreateShaderModule(g.device, &shader, nullptr, &g.fragmentShader), "create fragment shader")) return false;
    if (!createRenderPass()) return false;
    if (!createAmbientOcclusionResources()) return false;
    if (!createRealtimeLightingResources()) return false;
    if (!createRecordingWorkers()) return false;
    if (!createSubmissionSlots()) return false;
    if (!createTargets(width, height)) return false;
    g.ready = true;
    uint8_t const white[] = { 255, 255, 255, 255 };
    vulkanUploadTexture(0, 0, 1, 1, white);
    if (g.failed || !submit()) return false;
    std::fprintf(stderr, "Vulkan: native renderer initialized on %s (%ux%u; %s)\n", g.properties.deviceName,
                 g.extent.width, g.extent.height, g.depthClipControl ? "native GL depth clip range" : "shader depth remapping");
    return true;
}
} // namespace

bool vulkanCreate(SDL_Window *window, int width, int height, int vsync)
{
    vulkanDestroy();
    if (initialize(window, width, height, vsync)) return true;
    vulkanDestroy();
    return false;
}

void vulkanDestroy()
{
    // No work can have been submitted before the full device dispatch table
    // loads. A partial initialization must not call an unresolved wait function.
    if (g.device && !g.deviceDispatchReady)
    {
        if (vkDestroyDevice) vkDestroyDevice(g.device, nullptr);
        g.device = VK_NULL_HANDLE;
    }
    if (g.device)
    {
        if (g.ready && !g.failed) submit();
        ++g.submissionStats.deviceIdleWaits;
        VkResult idleResult = vkDeviceWaitIdle(g.device);
        if (idleResult != VK_SUCCESS && idleResult != VK_ERROR_DEVICE_LOST)
        {
            std::fprintf(stderr, "Vulkan: teardown device wait failed (VkResult %d); waiting for the queue\n",
                         static_cast<int>(idleResult));
            ++g.submissionStats.queueIdleWaits;
            idleResult = vkQueueWaitIdle(g.queue);
        }
        if (idleResult == VK_SUCCESS) markCompleted(g.submissionStats.submissions);
        else if (idleResult != VK_ERROR_DEVICE_LOST)
        {
            // An allocation error does not establish completion or device loss.
            // Stop without running destructors that could free pending objects.
            std::fprintf(stderr, "Vulkan: fatal teardown wait failure (VkResult %d); GPU resource completion is unknown\n",
                         static_cast<int>(idleResult));
            std::fflush(stderr);
            std::_Exit(EXIT_FAILURE);
        }
        destroyRecordingWorkers();
        // A terminal device/queue error can leave retirements outstanding. The
        // device is now idle or lost; none of these resources are reused.
        for (auto &retired : g.retiredImages) destroyImage(retired.image);
        for (auto &retired : g.retiredBuffers) destroyBuffer(retired.buffer);
        g.retiredImages.clear();
        g.retiredBuffers.clear();
        g.submissionStats.pendingRetiredImages = 0;
        auto const &stats = g.recordingStats;
        if (stats.workerCount)
        {
            std::fprintf(stderr, "Vulkan recording stats: workers=%u started_workers=%u background_workers=%u started_background_workers=%u caller_thread_id=%llu peak_active=%u batches=%llu parallel_batches=%llu caller_parallel_batches=%llu draws=%llu secondary_buffers=%llu dispatch_ns=%llu\n",
                stats.workerCount, stats.startedWorkers, stats.backgroundWorkerCount, stats.startedBackgroundWorkers,
                static_cast<unsigned long long>(stats.callerThreadId), stats.peakActiveRecorders,
                static_cast<unsigned long long>(stats.batches), static_cast<unsigned long long>(stats.parallelBatches),
                static_cast<unsigned long long>(stats.callerParallelBatches),
                static_cast<unsigned long long>(stats.draws), static_cast<unsigned long long>(stats.secondaryBuffers),
                static_cast<unsigned long long>(stats.dispatchNanoseconds));
            for (size_t i = 0; i < stats.workerDraws.size(); ++i)
                std::fprintf(stderr, "Vulkan recording worker %u: thread_id=%llu draws=%llu record_ns=%llu\n",
                    static_cast<unsigned>(i), static_cast<unsigned long long>(stats.workerThreadIds[i]),
                    static_cast<unsigned long long>(stats.workerDraws[i]), static_cast<unsigned long long>(stats.workerRecordNanoseconds[i]));
        }
        auto const &submissions = g.submissionStats;
        if (submissions.framesInFlight)
            std::fprintf(stderr, "Vulkan submission stats: frames_in_flight=%u submissions=%llu completed=%llu pending=%u max_pending=%u max_observed_unsignaled=%u presents=%llu fence_waits=%llu slot_reuse_waits=%llu finish_waits=%llu readback_waits=%llu queue_idle_waits=%llu device_idle_waits=%llu retired_images=%llu pending_retired_images=%u\n",
                submissions.framesInFlight, static_cast<unsigned long long>(submissions.submissions),
                static_cast<unsigned long long>(submissions.completedSubmissions), submissions.pendingSubmissions,
                submissions.maxPendingSubmissions, submissions.maxObservedUnsignaled,
                static_cast<unsigned long long>(submissions.presents), static_cast<unsigned long long>(submissions.fenceWaits),
                static_cast<unsigned long long>(submissions.slotReuseWaits), static_cast<unsigned long long>(submissions.finishWaits),
                static_cast<unsigned long long>(submissions.readbackWaits), static_cast<unsigned long long>(submissions.queueIdleWaits),
                static_cast<unsigned long long>(submissions.deviceIdleWaits), static_cast<unsigned long long>(submissions.retiredImages),
                submissions.pendingRetiredImages);
        destroySubmissionSlots();
        for (auto const &pipeline : g.pipelines) vkDestroyPipeline(g.device, pipeline.second, nullptr);
        for (auto const &sampler : g.samplers) vkDestroySampler(g.device, sampler.second, nullptr);
        for (auto &texture : g.textures) destroyImage(texture.second);
        destroyTargets();
        if (g.lighting.shadowFramebuffer) vkDestroyFramebuffer(g.device, g.lighting.shadowFramebuffer, nullptr);
        destroyImage(g.lighting.atlas);
        if (g.lighting.pipeline) vkDestroyPipeline(g.device, g.lighting.pipeline, nullptr);
        if (g.lighting.shadowPipeline) vkDestroyPipeline(g.device, g.lighting.shadowPipeline, nullptr);
        if (g.lighting.vertexShader) vkDestroyShaderModule(g.device, g.lighting.vertexShader, nullptr);
        if (g.lighting.fragmentShader) vkDestroyShaderModule(g.device, g.lighting.fragmentShader, nullptr);
        if (g.lighting.shadowVertexShader) vkDestroyShaderModule(g.device, g.lighting.shadowVertexShader, nullptr);
        if (g.lighting.shadowFragmentShader) vkDestroyShaderModule(g.device, g.lighting.shadowFragmentShader, nullptr);
        if (g.lighting.pipelineLayout) vkDestroyPipelineLayout(g.device, g.lighting.pipelineLayout, nullptr);
        if (g.lighting.shadowLayout) vkDestroyPipelineLayout(g.device, g.lighting.shadowLayout, nullptr);
        if (g.lighting.shadowRenderPass) vkDestroyRenderPass(g.device, g.lighting.shadowRenderPass, nullptr);
        if (g.lighting.descriptorLayout) vkDestroyDescriptorSetLayout(g.device, g.lighting.descriptorLayout, nullptr);
        if (g.ao.pipeline) vkDestroyPipeline(g.device, g.ao.pipeline, nullptr);
        if (g.ao.vertexShader) vkDestroyShaderModule(g.device, g.ao.vertexShader, nullptr);
        if (g.ao.fragmentShader) vkDestroyShaderModule(g.device, g.ao.fragmentShader, nullptr);
        if (g.ao.sampler) vkDestroySampler(g.device, g.ao.sampler, nullptr);
        if (g.ao.pipelineLayout) vkDestroyPipelineLayout(g.device, g.ao.pipelineLayout, nullptr);
        if (g.ao.renderPass) vkDestroyRenderPass(g.device, g.ao.renderPass, nullptr);
        if (g.ao.descriptorLayout) vkDestroyDescriptorSetLayout(g.device, g.ao.descriptorLayout, nullptr);
        if (g.vertexShader) vkDestroyShaderModule(g.device, g.vertexShader, nullptr);
        if (g.fragmentShader) vkDestroyShaderModule(g.device, g.fragmentShader, nullptr);
        if (g.pipelineCache) vkDestroyPipelineCache(g.device, g.pipelineCache, nullptr);
        if (g.pipelineLayout) vkDestroyPipelineLayout(g.device, g.pipelineLayout, nullptr);
        if (g.renderPass) vkDestroyRenderPass(g.device, g.renderPass, nullptr);
        if (g.descriptorLayout) vkDestroyDescriptorSetLayout(g.device, g.descriptorLayout, nullptr);
        vkDestroyDevice(g.device, nullptr);
    }
    if (g.surface) vkDestroySurfaceKHR(g.instance, g.surface, nullptr);
    if (g.instance && vkDestroyInstance) vkDestroyInstance(g.instance, nullptr);
    if (g.loaderOwned) SDL_Vulkan_UnloadLibrary();
    vkGetInstanceProcAddr = nullptr;
#define CLEAR_VULKAN_PROC(name) name = nullptr;
    NBLOOD_VULKAN_GLOBAL_COMMANDS(CLEAR_VULKAN_PROC)
    NBLOOD_VULKAN_INSTANCE_COMMANDS(CLEAR_VULKAN_PROC)
    NBLOOD_VULKAN_DEVICE_COMMANDS(CLEAR_VULKAN_PROC)
#undef CLEAR_VULKAN_PROC
    g = Backend();
}

// Ownership is independent of health: SDL must never mistake a failed Vulkan
// renderer for an OpenGL context and dispatch GL calls against this window.
bool vulkanActive() { return g.ready; }

bool vulkanSupported()
{
    if (g.ready) return true;
    if (SDL_Vulkan_LoadLibrary(nullptr) < 0) return false;
    auto getProc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
    auto enumerate = getProc ? reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
        getProc(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties")) : nullptr;
    uint32_t count = 0;
    bool const supported = enumerate && enumerate(nullptr, &count, nullptr) == VK_SUCCESS && count != 0;
    SDL_Vulkan_UnloadLibrary();
    return supported;
}

static bool resizeTargets(int width, int height, bool force)
{
    if (!vulkanActive() || g.failed) return false;
    if (width <= 0 || height <= 0)
    {
        if (!submit()) return false;
        g.suspended = true;
        return true;
    }
    if (!force && !g.suspended && static_cast<uint32_t>(width) == g.extent.width &&
        static_cast<uint32_t>(height) == g.extent.height && g.vsync == g.swapchainVsync) return true;
    if (!submit()) return false;
    ++g.submissionStats.deviceIdleWaits;
    if (!check(vkDeviceWaitIdle(g.device), "wait for resize")) return false;
    markCompleted(g.submissionStats.submissions);
    destroyTargets();
    return createTargets(width, height);
}

bool vulkanResize(int width, int height) { return resizeTargets(width, height, false); }

void vulkanSetVsync(int vsync)
{
    if (!vulkanActive() || g.failed || g.vsync == vsync) return;
    g.vsync = vsync;
    int width, height;
    SDL_Vulkan_GetDrawableSize(g.window, &width, &height);
    vulkanResize(width, height);
}

void vulkanFinish()
{
    if (vulkanActive() && !g.failed && submit())
        waitForSerial(g.submissionStats.submissions, WaitReason::Finish);
}

void vulkanFlush() { if (vulkanActive() && !g.failed) submit(); }

bool vulkanAmbientOcclusion(float const projection[16], int const worldViewport[4],
                            float radius, float strength, float bias)
{
    if (!vulkanActive() || g.failed) return false;
    // In particular, AO-off must not flush a batch or otherwise change the
    // existing rendering/submission behavior.
    if (radius <= 0.f || strength <= 0.f || g.suspended) return true;
    if (!projection || !worldViewport || !std::isfinite(radius) || !std::isfinite(strength) || !std::isfinite(bias)) return false;
    for (unsigned i = 0; i < 16; ++i) if (!std::isfinite(projection[i])) return false;
    if (worldViewport[2] <= 0 || worldViewport[3] <= 0) return true;
    if (projection[0] <= 0.f || projection[5] <= 0.f ||
        std::fabs(projection[11] - 1.f) > 0.00001f || std::fabs(projection[15]) > 0.00001f) return false;
    if (!g.ao.supported)
    {
        if (!g.ao.warned)
        {
            std::fprintf(stderr, "Vulkan: ambient occlusion disabled; this device cannot sample the scene depth/stencil format\n");
            g.ao.warned = true;
        }
        return false;
    }
    int64_t const x0 = std::max<int64_t>(0, worldViewport[0]);
    int64_t const y0 = std::max<int64_t>(0, worldViewport[1]);
    int64_t const x1 = std::min<int64_t>(g.extent.width, int64_t(worldViewport[0]) + worldViewport[2]);
    int64_t const y1 = std::min<int64_t>(g.extent.height, int64_t(worldViewport[1]) + worldViewport[3]);
    if (x1 <= x0 || y1 <= y0) return true;

    if (!endRenderPass()) return false;
    VkDeviceSize offset;
    VkDeviceSize const alignment = std::max<VkDeviceSize>(16, g.properties.limits.minUniformBufferOffsetAlignment);
    if (!reserveStream(sizeof(AmbientOcclusionUniforms), alignment, offset) || !ensureRecordingWorkers(1)) return false;
    AmbientOcclusionUniforms uniforms = {};
    uniforms.projection[0] = projection[0]; uniforms.projection[1] = projection[5];
    uniforms.projection[2] = projection[8]; uniforms.projection[3] = projection[9];
    uniforms.depth[0] = projection[10]; uniforms.depth[1] = projection[14];
    uniforms.depth[2] = radius; uniforms.depth[3] = std::max(0.f, std::min(1.f, bias));
    for (unsigned i = 0; i < 4; ++i) uniforms.viewport[i] = float(worldViewport[i]);
    uniforms.target[0] = 1.f / g.extent.width; uniforms.target[1] = 1.f / g.extent.height;
    uniforms.target[2] = strength;
    std::memcpy(static_cast<uint8_t *>(currentSlot().stream.mapped) + offset, &uniforms, sizeof(uniforms));

    // Use the current fence-owned descriptor page. A postprocess descriptor may
    // outlive this CPU frame, just like the preceding parallel world packets.
    RecordingWorker allocator;
    allocator.context.device = g.device;
    allocator.context.descriptorLayout = g.ao.descriptorLayout;
    allocator.resources = currentSlot().recorders[0];
    VkDescriptorSet set;
    if (!allocateWorkerDescriptor(allocator, set)) return check(allocator.error, allocator.operation);
    ++currentSlot().descriptorCount;
    VkDescriptorBufferInfo uniform = { currentSlot().stream.handle, offset, sizeof(uniforms) };
    VkDescriptorImageInfo images[2] = {
        { g.ao.sampler, g.ao.depthView, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL },
        { g.ao.sampler, g.ao.stencilView, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL },
    };
    VkWriteDescriptorSet writes[3] = {};
    for (uint32_t i = 0; i < 3; ++i)
    {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = i ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        if (i) writes[i].pImageInfo = &images[i - 1];
        else writes[i].pBufferInfo = &uniform;
    }
    vkUpdateDescriptorSets(g.device, 3, writes, 0, nullptr);
    VkImageAspectFlags const aspects = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    imageBarrier(g.depth.handle, aspects, 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                 VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    VkRenderPassBeginInfo begin = {};
    begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    begin.renderPass = g.ao.renderPass;
    begin.framebuffer = g.ao.framebuffer;
    begin.renderArea.extent = g.extent;
    VkCommandBuffer const command = currentSlot().command;
    vkCmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport viewport = { 0.f, 0.f, float(g.extent.width), float(g.extent.height), 0.f, 1.f };
    VkRect2D scissor = { { int32_t(x0), int32_t(g.extent.height - y1) }, { uint32_t(x1 - x0), uint32_t(y1 - y0) } };
    vkCmdSetViewport(command, 0, 1, &viewport);
    vkCmdSetScissor(command, 0, 1, &scissor);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, g.ao.pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, g.ao.pipelineLayout, 0, 1, &set, 0, nullptr);
    vkCmdDraw(command, 3, 1, 0, 0);
    vkCmdEndRenderPass(command);
    imageBarrier(g.depth.handle, aspects, 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                 VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                 VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                 VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
    return true;
}

bool vulkanRealtimeLighting(float const projection[16], float const eyeToWorld[16], int const worldViewport[4],
                            VulkanLight const *lights, unsigned lightCount,
                            VulkanShadowVertex const *shadowVertices, unsigned shadowVertexCount, bool shadows)
{
    if (!vulkanActive() || g.failed) return false;
    // Disabling lighting, or visiting a sector without lights, leaves packet
    // batching and submission exactly as they were before this feature.
    if (!lightCount || g.suspended) return true;
    if (!projection || !eyeToWorld || !worldViewport || !lights || lightCount > kVulkanMaxLights) return false;
    if (shadows && (shadowVertexCount % 3 || (shadowVertexCount && !shadowVertices))) return false;
    for (unsigned i = 0; i < 16; ++i)
        if (!std::isfinite(projection[i]) || !std::isfinite(eyeToWorld[i])) return false;
    for (unsigned i = 0; i < lightCount; ++i)
    {
        for (unsigned j = 0; j < 4; ++j)
            if (!std::isfinite(lights[i].positionRadius[j]) || !std::isfinite(lights[i].color[j]) || lights[i].color[j] < 0.f) return false;
        if (lights[i].positionRadius[3] <= 0.001f) return false;
    }
    if (worldViewport[2] <= 0 || worldViewport[3] <= 0) return true;
    if (projection[0] <= 0.f || projection[5] <= 0.f ||
        std::fabs(projection[11] - 1.f) > 0.00001f || std::fabs(projection[15]) > 0.00001f) return false;
    if (!g.lighting.supported)
    {
        if (!g.lighting.warned)
        {
            std::fprintf(stderr, "Vulkan: realtime lighting disabled; sampled scene depth/stencil or shadow depth formats are unavailable\n");
            g.lighting.warned = true;
        }
        return false;
    }
    int64_t const x0 = std::max<int64_t>(0, worldViewport[0]);
    int64_t const y0 = std::max<int64_t>(0, worldViewport[1]);
    int64_t const x1 = std::min<int64_t>(g.extent.width, int64_t(worldViewport[0]) + worldViewport[2]);
    int64_t const y1 = std::min<int64_t>(g.extent.height, int64_t(worldViewport[1]) + worldViewport[3]);
    if (x1 <= x0 || y1 <= y0) return true;
    if (!endRenderPass()) return false;
    // Reserve uniforms and the complete immutable shadow mesh together. A
    // stream rollover may submit, so no per-call offsets may span two slots.
    unsigned const vertexCount = shadows ? shadowVertexCount : 0;
    VkDeviceSize const vertexBytes = VkDeviceSize(vertexCount) * sizeof(VulkanShadowVertex);
    VkDeviceSize const alignment = std::max<VkDeviceSize>(16, g.properties.limits.minUniformBufferOffsetAlignment);
    VkDeviceSize offset;
    if (!reserveStream(sizeof(RealtimeLightingUniforms) + vertexBytes, alignment, offset) || !ensureRecordingWorkers(1)) return false;
    VkDeviceSize const vertexOffset = offset + sizeof(RealtimeLightingUniforms);
    RealtimeLightingUniforms uniforms = {};
    uniforms.projection[0] = projection[0]; uniforms.projection[1] = projection[5];
    uniforms.projection[2] = projection[8]; uniforms.projection[3] = projection[9];
    uniforms.depth[0] = projection[10]; uniforms.depth[1] = projection[14];
    for (unsigned i = 0; i < 4; ++i) uniforms.viewport[i] = float(worldViewport[i]);
    uniforms.target[0] = 1.f / g.extent.width; uniforms.target[1] = 1.f / g.extent.height;
    std::memcpy(uniforms.eyeToWorld, eyeToWorld, sizeof(uniforms.eyeToWorld));
    uniforms.lightParams[0] = float(lightCount);
    uniforms.lightParams[1] = shadows ? 1.f : 0.f;
    uniforms.lightParams[2] = float(kShadowFaceSize);
    uniforms.lightParams[3] = float(kVulkanMaxLights);
    std::memcpy(uniforms.lights, lights, lightCount * sizeof(VulkanLight));
    uint8_t *mapped = static_cast<uint8_t *>(currentSlot().stream.mapped);
    std::memcpy(mapped + offset, &uniforms, sizeof(uniforms));
    if (vertexBytes) std::memcpy(mapped + vertexOffset, shadowVertices, vertexBytes);

    RecordingWorker allocator;
    allocator.context.device = g.device;
    allocator.context.descriptorLayout = g.lighting.descriptorLayout;
    allocator.resources = currentSlot().recorders[0];
    VkDescriptorSet set;
    if (!allocateWorkerDescriptor(allocator, set)) return check(allocator.error, allocator.operation);
    ++currentSlot().descriptorCount;
    VkDescriptorBufferInfo uniform = { currentSlot().stream.handle, offset, sizeof(uniforms) };
    VkDescriptorImageInfo images[3] = {
        { g.ao.sampler, g.ao.depthView, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL },
        { g.ao.sampler, g.ao.stencilView, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL },
        { g.ao.sampler, g.lighting.atlas.view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL },
    };
    VkWriteDescriptorSet writes[4] = {};
    for (uint32_t i = 0; i < 4; ++i)
    {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = i ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        if (i) writes[i].pImageInfo = &images[i - 1];
        else writes[i].pBufferInfo = &uniform;
    }
    vkUpdateDescriptorSets(g.device, 4, writes, 0, nullptr);

    VkCommandBuffer const command = currentSlot().command;
    if (shadows || !g.lighting.atlasInitialized)
    {
        VkClearValue clear = {};
        clear.depthStencil.depth = 1.f;
        VkRenderPassBeginInfo begin = {};
        begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        begin.renderPass = g.lighting.shadowRenderPass;
        begin.framebuffer = g.lighting.shadowFramebuffer;
        begin.renderArea.extent = { g.lighting.atlas.width, g.lighting.atlas.height };
        begin.clearValueCount = 1;
        begin.pClearValues = &clear;
        vkCmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE);
        if (vertexCount)
        {
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, g.lighting.shadowPipeline);
            vkCmdBindVertexBuffers(command, 0, 1, &currentSlot().stream.handle, &vertexOffset);
            for (unsigned i = 0; i < lightCount; ++i)
                for (uint32_t face = 0; face < kShadowFaces; ++face)
                {
                    ShadowPush push = {};
                    std::memcpy(push.light, lights[i].positionRadius, sizeof(push.light));
                    push.face[0] = int32_t(face);
                    VkViewport viewport = { float(i * kShadowFaceSize), float(face * kShadowFaceSize),
                        float(kShadowFaceSize), float(kShadowFaceSize), 0.f, 1.f };
                    VkRect2D scissor = { { int32_t(i * kShadowFaceSize), int32_t(face * kShadowFaceSize) }, { kShadowFaceSize, kShadowFaceSize } };
                    vkCmdSetViewport(command, 0, 1, &viewport);
                    vkCmdSetScissor(command, 0, 1, &scissor);
                    vkCmdPushConstants(command, g.lighting.shadowLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                       0, sizeof(push), &push);
                    vkCmdDraw(command, vertexCount, 1, 0, 0);
                }
        }
        vkCmdEndRenderPass(command);
        g.lighting.atlasInitialized = true;
    }
    VkImageAspectFlags const aspects = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    imageBarrier(g.depth.handle, aspects, 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                 VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    VkRenderPassBeginInfo begin = {};
    begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    begin.renderPass = g.ao.renderPass;
    begin.framebuffer = g.ao.framebuffer;
    begin.renderArea.extent = g.extent;
    vkCmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport viewport = { 0.f, 0.f, float(g.extent.width), float(g.extent.height), 0.f, 1.f };
    VkRect2D scissor = { { int32_t(x0), int32_t(g.extent.height - y1) }, { uint32_t(x1 - x0), uint32_t(y1 - y0) } };
    vkCmdSetViewport(command, 0, 1, &viewport);
    vkCmdSetScissor(command, 0, 1, &scissor);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, g.lighting.pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, g.lighting.pipelineLayout, 0, 1, &set, 0, nullptr);
    vkCmdDraw(command, 3, 1, 0, 0);
    vkCmdEndRenderPass(command);
    imageBarrier(g.depth.handle, aspects, 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                 VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                 VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                 VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
    return true;
}

bool vulkanPresent()
{
    if (!vulkanActive() || g.failed) return false;
    if (g.suspended)
    {
        int width, height;
        SDL_Vulkan_GetDrawableSize(g.window, &width, &height);
        if (width <= 0 || height <= 0) return submit();
        return resizeTargets(width, height, true);
    }
    if (!prepareCurrentSlot()) return false;
    VkSemaphore const acquired = currentSlot().acquired;
    uint32_t index;
    VkResult acquire = vkAcquireNextImageKHR(g.device, g.swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &index);
    if (acquire == VK_ERROR_OUT_OF_DATE_KHR)
    {
        int width, height;
        SDL_Vulkan_GetDrawableSize(g.window, &width, &height);
        return resizeTargets(width, height, true);
    }
    if (acquire != VK_SUBOPTIMAL_KHR && !check(acquire, "acquire swapchain image")) return false;
    if (!beginCommands()) return false;
    if (!endRenderPass()) return false;
    imageBarrier(g.color.handle, VK_IMAGE_ASPECT_COLOR_BIT, 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    imageBarrier(g.swapImages[index], VK_IMAGE_ASPECT_COLOR_BIT, 1,
                 g.swapInitialized[index] ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                 // Match submit's acquire wait scope so the layout transition
                 // itself, not just the blit, occurs after WSI releases the image.
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageBlit blit = {};
    blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    blit.dstSubresource = blit.srcSubresource;
    blit.srcOffsets[1] = blit.dstOffsets[1] = { static_cast<int32_t>(g.extent.width), static_cast<int32_t>(g.extent.height), 1 };
    vkCmdBlitImage(currentSlot().command, g.color.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g.swapImages[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &blit, VK_FILTER_NEAREST);
    imageBarrier(g.swapImages[index], VK_IMAGE_ASPECT_COLOR_BIT, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                 VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    imageBarrier(g.color.handle, VK_IMAGE_ASPECT_COLOR_BIT, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    g.swapInitialized[index] = true;
    // Capture these handles before submit advances the resource-slot cursor.
    VkSemaphore const rendered = g.presentationSemaphores[index];
    if (!submit(acquired, rendered)) return false;
    VkPresentInfoKHR present = {};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &rendered;
    present.swapchainCount = 1;
    present.pSwapchains = &g.swapchain;
    present.pImageIndices = &index;
    VkResult result = vkQueuePresentKHR(g.queue, &present);
    if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) ++g.submissionStats.presents;
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || acquire == VK_SUBOPTIMAL_KHR)
    {
        int width, height;
        SDL_Vulkan_GetDrawableSize(g.window, &width, &height);
        return resizeTargets(width, height, true);
    }
    return check(result, "present swapchain image");
}

void vulkanDraw(VulkanVertex const *vertices, unsigned count, VulkanDrawState const &state)
{
    if (!vulkanActive() || g.failed || g.suspended || !vertices || !count || state.viewport[2] <= 0 || state.viewport[3] <= 0) return;
    VkRect2D scissor = clipRect(state);
    if (!scissor.extent.width || !scissor.extent.height) return;
    VkDeviceSize alignment = std::max<VkDeviceSize>(16, g.properties.limits.minUniformBufferOffsetAlignment);
    VkDeviceSize vertexBytes = static_cast<VkDeviceSize>(count) * sizeof(VulkanVertex);
    if (!g.packets.empty() && (g.packets.size() == kDrawBatchSize ||
        g.vertices.size() * sizeof(VulkanVertex) + vertexBytes > kDrawBatchVertexBytes))
        if (!flushDrawPackets()) return;
    VkDeviceSize uniformRelative = (vertexBytes + alignment - 1) & ~(alignment - 1);
    VkDeviceSize offset;
    if (!reserveStream(uniformRelative + sizeof(VulkanUniforms), alignment, offset)) return;
    DrawPacket packet = {};
    packet.state = state;
    packet.pipeline = pipeline(state);
    if (!packet.pipeline) return;
    for (unsigned i = 0; i < 5; ++i)
    {
        auto texture = g.textures.find(state.samplers[i].texture);
        if (texture == g.textures.end()) texture = g.textures.find(0);
        if (texture == g.textures.end()) { fail("Missing fallback texture"); return; }
        packet.images[i].imageView = texture->second.view;
        packet.images[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        packet.images[i].sampler = sampler(state.samplers[i]);
        if (!packet.images[i].sampler) return;
    }
    packet.viewport.x = static_cast<float>(state.viewport[0]);
    packet.viewport.y = static_cast<float>(static_cast<int>(g.extent.height) - state.viewport[1] - state.viewport[3]);
    packet.viewport.width = static_cast<float>(state.viewport[2]);
    packet.viewport.height = static_cast<float>(state.viewport[3]);
    packet.viewport.minDepth = std::max(0.f, std::min(1.f, state.depthRange[0]));
    packet.viewport.maxDepth = std::max(0.f, std::min(1.f, state.depthRange[1]));
    packet.scissor = scissor;
    packet.lineWidth = g.features.wideLines ? std::max(g.properties.limits.lineWidthRange[0], std::min(g.properties.limits.lineWidthRange[1], state.lineWidth)) : 1.f;
    packet.vertexOffset = offset;
    packet.uniformOffset = offset + uniformRelative;
    packet.firstVertex = g.vertices.size();
    packet.vertexCount = count;
    g.vertices.insert(g.vertices.end(), vertices, vertices + count);
    g.packets.push_back(packet);
    ++currentSlot().descriptorCount;
    if (g.packets.size() == kDrawBatchSize || vertexBytes > kDrawBatchVertexBytes)
        flushDrawPackets();
}

void vulkanClear(uint32_t mask, float const color[4], float depth, uint32_t stencil, VulkanDrawState const &state)
{
    if (!vulkanActive() || g.failed || g.suspended) return;
    VkRect2D rect = clipRect(state);
    if (!rect.extent.width || !rect.extent.height) return;
    bool anyColor = false, allColor = true;
    for (bool enabled : state.colorWrite) { anyColor |= enabled; allColor &= enabled; }
    if (!anyColor) mask &= ~GL_COLOR_BUFFER_BIT;
    if (!state.depthWrite) mask &= ~GL_DEPTH_BUFFER_BIT;
    if (!(state.stencilWriteMask & 255u)) mask &= ~GL_STENCIL_BUFFER_BIT;
    if (!mask) return;
    // vkCmdClearAttachments has no per-channel/per-bit mask. A native fullscreen
    // draw preserves the exact OpenGL masks for the uncommon masked clear case.
    if (((mask & GL_COLOR_BUFFER_BIT) && !allColor) || ((mask & GL_STENCIL_BUFFER_BIT) && (state.stencilWriteMask & 255u) != 255u))
    {
        VulkanDrawState clear = {};
        clear.viewport[2] = static_cast<int>(g.extent.width);
        clear.viewport[3] = static_cast<int>(g.extent.height);
        std::copy(state.scissor, state.scissor + 4, clear.scissor);
        clear.scissorTest = state.scissorTest;
        clear.topology = GL_TRIANGLES;
        clear.polygonMode = GL_FILL;
        clear.frontFace = GL_CCW;
        clear.depthTest = clear.depthWrite = (mask & GL_DEPTH_BUFFER_BIT) != 0;
        clear.depthFunc = GL_ALWAYS;
        clear.depthRange[1] = 1.f;
        clear.stencilTest = (mask & GL_STENCIL_BUFFER_BIT) != 0;
        clear.stencilFunc = GL_ALWAYS;
        clear.stencilFail = clear.stencilDepthFail = clear.stencilPass = GL_REPLACE;
        clear.stencilReference = stencil;
        clear.stencilCompareMask = 255;
        clear.stencilWriteMask = state.stencilWriteMask;
        clear.lineWidth = 1.f;
        for (unsigned i = 0; i < 4; ++i) clear.colorWrite[i] = (mask & GL_COLOR_BUFFER_BIT) && state.colorWrite[i];
        for (unsigned i = 0; i < 4; ++i) clear.uniforms.modelView[i * 5] = clear.uniforms.projection[i * 5] = 1.f;
        clear.uniforms.effects[0] = 1.f;
        clear.uniforms.misc[0] = 1.f;
        clear.uniforms.correction[0] = clear.uniforms.correction[1] = 1.f;
        for (auto &sample : clear.samplers)
        {
            sample.minFilter = sample.magFilter = GL_NEAREST;
            sample.wrapS = sample.wrapT = GL_CLAMP_TO_EDGE;
            sample.anisotropy = 1.f;
        }
        VulkanVertex vertices[3] = {};
        float const positions[3][2] = { { -1.f, -1.f }, { 3.f, -1.f }, { -1.f, 3.f } };
        for (unsigned i = 0; i < 3; ++i)
        {
            vertices[i].position[0] = positions[i][0];
            vertices[i].position[1] = positions[i][1];
            vertices[i].position[2] = std::max(0.f, std::min(1.f, depth)) * 2.f - 1.f;
            vertices[i].position[3] = 1.f;
            std::copy(color, color + 4, vertices[i].color);
        }
        vulkanDraw(vertices, 3, clear);
        return;
    }
    if (!flushDrawPackets() || !beginRenderPass()) return;
    VkClearAttachment attachments[2] = {};
    uint32_t count = 0;
    if (mask & GL_COLOR_BUFFER_BIT)
    {
        attachments[count].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        attachments[count].colorAttachment = 0;
        std::copy(color, color + 4, attachments[count].clearValue.color.float32);
        ++count;
    }
    if (mask & (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT))
    {
        attachments[count].aspectMask = ((mask & GL_DEPTH_BUFFER_BIT) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) |
                                        ((mask & GL_STENCIL_BUFFER_BIT) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        attachments[count].clearValue.depthStencil = { std::max(0.f, std::min(1.f, depth)), stencil };
        ++count;
    }
    VkClearRect clearRect = { rect, 0, 1 };
    vkCmdClearAttachments(currentSlot().command, count, attachments, 1, &clearRect);
}

void vulkanUploadTexture(uint32_t texture, int level, int width, int height, uint8_t const *rgba)
{
    if (!vulkanActive() || g.failed || level < 0 || level >= 31 || width <= 0 || height <= 0) return;
    uint64_t baseWidth = static_cast<uint64_t>(width) << level;
    uint64_t baseHeight = static_cast<uint64_t>(height) << level;
    auto existing = g.textures.find(texture);
    // Non-square and NPOT textures have clamped dimensions on their last mips.
    bool fits = existing != g.textures.end() && static_cast<uint32_t>(level) < existing->second.levels &&
                std::max(1u, existing->second.width >> level) == static_cast<uint32_t>(width) &&
                std::max(1u, existing->second.height >> level) == static_cast<uint32_t>(height);
    if (!fits)
    {
        if (baseWidth > g.properties.limits.maxImageDimension2D || baseHeight > g.properties.limits.maxImageDimension2D)
        {
            fail("Texture dimensions exceed the Vulkan device limit");
            return;
        }
        if (existing != g.textures.end())
        {
            if (!submit()) return;
            retireImage(existing->second);
            g.textures.erase(existing);
        }
        uint32_t levels = 1;
        for (uint64_t size = std::max(baseWidth, baseHeight); size > 1; size >>= 1) ++levels;
        Image image;
        if (!createImage(image, static_cast<uint32_t>(baseWidth), static_cast<uint32_t>(baseHeight), levels, VK_FORMAT_R8G8B8A8_UNORM,
                         VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT))
        {
            destroyImage(image);
            return;
        }
        existing = g.textures.emplace(texture, image).first;
        if (!beginCommands()) return;
        if (!endRenderPass()) return;
        imageBarrier(image.handle, VK_IMAGE_ASPECT_COLOR_BIT, levels, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkClearColorValue white = {};
        for (float &channel : white.float32) channel = 1.f;
        VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1 };
        vkCmdClearColorImage(currentSlot().command, image.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &white, 1, &range);
        imageBarrier(image.handle, VK_IMAGE_ASPECT_COLOR_BIT, levels, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    }
    if (rgba) vulkanUploadTextureRegion(texture, level, 0, 0, width, height, rgba);
}

void vulkanUploadTextureRegion(uint32_t texture, int level, int x, int y, int width, int height, uint8_t const *rgba)
{
    if (!vulkanActive() || g.failed || !rgba || width <= 0 || height <= 0) return;
    auto existing = g.textures.find(texture);
    if (existing == g.textures.end() || level < 0 || static_cast<uint32_t>(level) >= existing->second.levels || x < 0 || y < 0)
    {
        fail("Invalid texture subimage destination");
        return;
    }
    Image const &image = existing->second;
    if (static_cast<uint64_t>(x) + width > std::max(1u, image.width >> level) ||
        static_cast<uint64_t>(y) + height > std::max(1u, image.height >> level))
    {
        fail("Texture subimage extends beyond its mip level");
        return;
    }
    // Large atlases/high-resolution textures must not permanently grow the
    // shared host-visible stream to an entire image (potentially gigabytes).
    // Each copy contains complete tightly packed RGBA rows, preserving arbitrary
    // subimage offsets. reserveStream submits completed chunks before reuse.
    VkDeviceSize const rowBytes = static_cast<VkDeviceSize>(width) * 4;
    uint32_t const rowsPerChunk = static_cast<uint32_t>(std::max<VkDeviceSize>(1, kTextureUploadBytes / rowBytes));
    for (uint32_t row = 0; row < static_cast<uint32_t>(height); )
    {
        uint32_t const rows = std::min(rowsPerChunk, static_cast<uint32_t>(height) - row);
        VkDeviceSize const bytes = rowBytes * rows;
        VkDeviceSize offset;
        if (!reserveStream(bytes, 4, offset)) return;
        std::memcpy(static_cast<uint8_t *>(currentSlot().stream.mapped) + offset,
                    rgba + static_cast<size_t>(rowBytes) * row, static_cast<size_t>(bytes));
        if (!endRenderPass()) return;
        imageBarrier(image.handle, VK_IMAGE_ASPECT_COLOR_BIT, image.levels, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy = {};
        copy.bufferOffset = offset;
        copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, static_cast<uint32_t>(level), 0, 1 };
        copy.imageOffset = { x, y + static_cast<int32_t>(row), 0 };
        copy.imageExtent = { static_cast<uint32_t>(width), rows, 1 };
        vkCmdCopyBufferToImage(currentSlot().command, currentSlot().stream.handle, image.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        imageBarrier(image.handle, VK_IMAGE_ASPECT_COLOR_BIT, image.levels, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        row += rows;
    }
}

void vulkanDeleteTexture(uint32_t texture)
{
    if (!vulkanActive() || g.failed || texture == 0) return;
    auto existing = g.textures.find(texture);
    if (existing == g.textures.end()) return;
    if (!submit()) return;
    retireImage(existing->second);
    g.textures.erase(existing);
}

bool vulkanReadPixels(int x, int y, int width, int height, uint8_t *rgba)
{
    if (!vulkanActive() || g.failed || g.suspended || !rgba || width <= 0 || height <= 0) return false;
    int x0 = std::max(0, x), y0 = std::max(0, y);
    int x1 = std::min(static_cast<int>(g.extent.width), x + width);
    int y1 = std::min(static_cast<int>(g.extent.height), y + height);
    std::memset(rgba, 0, static_cast<size_t>(width) * height * 4);
    if (x1 <= x0 || y1 <= y0) return true;
    uint32_t clippedWidth = static_cast<uint32_t>(x1 - x0), clippedHeight = static_cast<uint32_t>(y1 - y0);
    Buffer readback;
    if (!createBuffer(readback, static_cast<VkDeviceSize>(clippedWidth) * clippedHeight * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT))
    {
        destroyBuffer(readback);
        return false;
    }
    if (!beginCommands()) { destroyBuffer(readback); return false; }
    if (!endRenderPass()) { destroyBuffer(readback); return false; }
    imageBarrier(g.color.handle, VK_IMAGE_ASPECT_COLOR_BIT, 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy copy = {};
    copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    copy.imageOffset = { x0, static_cast<int32_t>(g.extent.height) - y1, 0 };
    copy.imageExtent = { clippedWidth, clippedHeight, 1 };
    vkCmdCopyImageToBuffer(currentSlot().command, g.color.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.handle, 1, &copy);
    imageBarrier(g.color.handle, VK_IMAGE_ASPECT_COLOR_BIT, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    VkBufferMemoryBarrier host = {};
    host.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host.buffer = readback.handle;
    host.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(currentSlot().command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &host, 0, nullptr);
    uint64_t serial = 0;
    if (!submit(VK_NULL_HANDLE, VK_NULL_HANDLE, &serial) || !waitForSerial(serial, WaitReason::Readback))
    {
        // A successful enqueue followed by a fence/query failure still owns
        // this buffer on the GPU. Retain it for terminal device-idle teardown.
        if (serial > g.submissionStats.completedSubmissions)
            g.retiredBuffers.push_back({ readback, serial });
        else destroyBuffer(readback);
        return false;
    }
    for (uint32_t row = 0; row < clippedHeight; ++row)
        std::memcpy(rgba + (static_cast<size_t>(y0 - y + row) * width + x0 - x) * 4,
                    static_cast<uint8_t const *>(readback.mapped) + static_cast<size_t>(clippedHeight - 1 - row) * clippedWidth * 4,
                    static_cast<size_t>(clippedWidth) * 4);
    destroyBuffer(readback);
    return true;
}

char const *vulkanDeviceName() { return g.properties.deviceName; }
int vulkanMaxTextureSize() { return static_cast<int>(g.properties.limits.maxImageDimension2D); }
float vulkanMaxAnisotropy() { return g.features.samplerAnisotropy ? g.properties.limits.maxSamplerAnisotropy : 1.f; }
bool vulkanDepthClampSupported() { return g.features.depthClamp != VK_FALSE; }
void vulkanGetRecordingStats(VulkanRecordingStats *stats)
{
    if (stats) *stats = g.recordingStats;
}

void vulkanGetSubmissionStats(VulkanSubmissionStats *stats)
{
    if (stats) *stats = g.submissionStats;
}

unsigned vulkanAvailableRecordingCores()
{
#ifdef __linux__
    cpu_set_t available;
    CPU_ZERO(&available);
    if (sched_getaffinity(0, sizeof(available), &available) == 0)
    {
        unsigned count = 0;
        for (unsigned i = 0; i < CPU_SETSIZE; ++i)
            if (CPU_ISSET(i, &available)) ++count;
        if (count) return count;
    }
#endif
    return static_cast<unsigned>(std::max(1, SDL_GetCPUCount()));
}
#endif // USE_VULKAN
