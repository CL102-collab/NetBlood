// Native Metal backend for the shared Polymost scene frontend.
// Copyright (C) 2026 NBlood contributors. GPL-2.0-only.
// Compile as Objective-C++ with ARC. No OpenGL context or Vulkan runtime is used.
#include "metal_renderer.h"

#ifdef USE_METAL
#include <SDL.h>
#include <SDL_metal.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include "glad/glad.h" // Frontend state enumerants only; never call OpenGL.
#include "metal_shaders.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace {
constexpr NSUInteger kStreamPageBytes = 4u * 1024u * 1024u;
constexpr NSUInteger kSubmissionBytes = 64u * 1024u * 1024u;
constexpr unsigned kFramesInFlight = 3;
constexpr MTLPixelFormat kColorFormat = MTLPixelFormatBGRA8Unorm;
constexpr MTLPixelFormat kDepthFormat = MTLPixelFormatDepth32Float_Stencil8;

struct StreamPage
{
    id<MTLBuffer> buffer = nil;
    NSUInteger used = 0;
};

struct SubmissionSlot
{
    id<MTLCommandBuffer> submitted = nil;
    std::vector<StreamPage> pages;
    size_t page = 0;
    NSUInteger bytes = 0;
};

struct StreamAllocation
{
    id<MTLBuffer> buffer = nil;
    NSUInteger offset = 0;
    uint8_t *data = nullptr;
};

struct Texture
{
    id<MTLTexture> image = nil;
    uint32_t width = 0, height = 0, levels = 0;
};

template<size_t Count> struct StateKey
{
    std::array<uint32_t, Count> values;
    bool operator<(StateKey const &other) const { return values < other.values; }
};
using PipelineKey = StateKey<5>;
using DepthKey = StateKey<10>;
using SamplerKey = StateKey<7>;

// Same 64-byte layout as the OpenGL/Vulkan ambient-occlusion pass.
struct AmbientOcclusionUniforms
{
    float projection[4], depth[4], viewport[4], target[4];
};

constexpr NSUInteger kShadowFaceSize = 256;
constexpr NSUInteger kShadowFaces = 6;
struct RealtimeLightingUniforms
{
    AmbientOcclusionUniforms ao;
    float eyeToWorld[16], params[4];
    RealtimeLight lights[kMaxRealtimeLights];
};
struct ShadowUniforms
{
    float light[4];
    int32_t face[4];
};
static_assert(kMaxRealtimeLights == 8 && sizeof(RealtimeLight) == 32 && sizeof(RealtimeLightingUniforms) == 400,
              "Keep MetalLightingUniforms in sync with the native lighting block");
static_assert(sizeof(RealtimeShadowVertex) == 12 && sizeof(ShadowUniforms) == 32,
              "Keep packed_float3 shadow vertices and MetalShadowUniforms in sync");

struct RealtimeLighting
{
    id<MTLRenderPipelineState> pipeline = nil, shadowPipeline = nil;
    id<MTLDepthStencilState> shadowDepth = nil;
    id<MTLTexture> atlas = nil;
    bool initialized = false, failed = false;
};

struct Backend
{
    SDL_Window *window = nullptr;
    SDL_MetalView view = nullptr;
    CAMetalLayer *layer = nil;
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    id<MTLLibrary> library = nil;
    id<MTLFunction> vertex = nil, fragment = nil;
    id<MTLRenderPipelineState> ambientPipeline = nil, presentPipeline = nil;
    id<MTLDepthStencilState> noDepth = nil;
    id<MTLSamplerState> nearestSampler = nil, presentSampler = nil;
    id<MTLTexture> color = nil, depth = nil, stencil = nil;
    id<MTLCommandBuffer> command = nil;
    id<MTLRenderCommandEncoder> render = nil;
    id<MTLBlitCommandEncoder> blit = nil;
    RealtimeLighting lighting;
    std::array<SubmissionSlot, kFramesInFlight> slots;
    unsigned slot = 0;
    std::map<PipelineKey, id<MTLRenderPipelineState>> pipelines;
    std::map<DepthKey, id<MTLDepthStencilState>> depths;
    std::map<SamplerKey, id<MTLSamplerState>> samplers;
    std::map<uint32_t, Texture> textures;
    std::string name;
    int width = 0, height = 0, vsync = 1;
    bool ready = false, failed = false, initialized = false, suspended = false;
};
Backend g;

bool fail(char const *message)
{
    std::fprintf(stderr, "Metal: %s\n", message);
    SDL_SetError("Metal: %s", message);
    g.failed = true;
    return false;
}

bool failure(NSError *error, char const *operation)
{
    std::string message(operation);
    if (error) { message += ": "; message += error.localizedDescription.UTF8String; }
    return fail(message.c_str());
}

void endEncoding()
{
    if (g.render) { [g.render endEncoding]; g.render = nil; }
    if (g.blit) { [g.blit endEncoding]; g.blit = nil; }
}

bool waitCommand(id<MTLCommandBuffer> command)
{
    if (!command) return true;
    [command waitUntilCompleted];
    return command.status != MTLCommandBufferStatusError || failure(command.error, "GPU command buffer failed");
}

bool beginCommands()
{
    if (g.command) return !g.failed;
    if (g.failed || !g.queue) return false;
    SubmissionSlot &slot = g.slots[g.slot];
    // The only normal-frame wait is recycling a bounded submission slot.
    // Command buffers retain resources used by their encoders, so replacing or
    // deleting a virtual texture never frees an in-flight GPU allocation.
    if (!waitCommand(slot.submitted)) return false;
    slot.submitted = nil;
    slot.page = 0;
    slot.bytes = 0;
    for (auto &page : slot.pages) page.used = 0;
    g.command = [g.queue commandBuffer];
    if (!g.command) return fail("Cannot allocate a command buffer");
    g.command.label = @"NetBlood native Metal submission";
    return true;
}

bool submit(id<CAMetalDrawable> drawable = nil)
{
    endEncoding();
    if (!g.command) return !g.failed;
    if (drawable) [g.command presentDrawable:drawable];
    g.slots[g.slot].submitted = g.command;
    [g.command commit];
    g.command = nil;
    g.slot = (g.slot + 1) % kFramesInFlight;
    return !g.failed;
}

bool reserveStream(NSUInteger bytes, NSUInteger alignment, StreamAllocation &allocation)
{
    if (!bytes || bytes > kSubmissionBytes) return fail("Draw exceeds the Metal streaming-buffer limit");
    if (!beginCommands()) return false;
    if (g.slots[g.slot].bytes > kSubmissionBytes - bytes)
        if (!submit() || !beginCommands()) return false;
    SubmissionSlot &slot = g.slots[g.slot];
    while (slot.page < slot.pages.size())
    {
        StreamPage &page = slot.pages[slot.page];
        NSUInteger offset = (page.used + alignment - 1) & ~(alignment - 1);
        if (offset <= page.buffer.length && bytes <= page.buffer.length - offset)
        {
            allocation.buffer = page.buffer;
            allocation.offset = offset;
            allocation.data = static_cast<uint8_t *>(page.buffer.contents) + offset;
            page.used = offset + bytes;
            slot.bytes += bytes;
            return true;
        }
        ++slot.page;
    }
    StreamPage page;
    page.buffer = [g.device newBufferWithLength:std::max(kStreamPageBytes, bytes) options:MTLResourceStorageModeShared];
    if (!page.buffer) return fail("Cannot allocate a Metal streaming buffer");
    page.buffer.label = @"NetBlood frame-owned vertex/uniform/transfer stream";
    slot.pages.push_back(page);
    return reserveStream(bytes, alignment, allocation);
}

bool beginBlit()
{
    if (g.blit) return true;
    if (!beginCommands()) return false;
    if (g.render) { [g.render endEncoding]; g.render = nil; }
    g.blit = [g.command blitCommandEncoder];
    return g.blit != nil || fail("Cannot create a transfer encoder");
}

MTLRenderPassDescriptor *scenePass()
{
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = g.color;
    pass.colorAttachments[0].loadAction = g.initialized ? MTLLoadActionLoad : MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
    pass.depthAttachment.texture = g.depth;
    pass.depthAttachment.loadAction = g.initialized ? MTLLoadActionLoad : MTLLoadActionClear;
    pass.depthAttachment.storeAction = MTLStoreActionStore;
    pass.depthAttachment.clearDepth = 1.;
    pass.stencilAttachment.texture = g.depth;
    pass.stencilAttachment.loadAction = g.initialized ? MTLLoadActionLoad : MTLLoadActionClear;
    pass.stencilAttachment.storeAction = MTLStoreActionStore;
    pass.stencilAttachment.clearStencil = 0;
    return pass;
}

bool beginRender()
{
    if (g.render) return true;
    if (!beginCommands()) return false;
    if (g.blit) { [g.blit endEncoding]; g.blit = nil; }
    g.render = [g.command renderCommandEncoderWithDescriptor:scenePass()];
    if (!g.render) return fail("Cannot create a scene encoder");
    g.render.label = @"Polymost scene through native Metal";
    g.initialized = true;
    return true;
}

MTLCompareFunction comparison(uint32_t value)
{
    switch (value)
    {
        case GL_NEVER: return MTLCompareFunctionNever;
        case GL_LESS: return MTLCompareFunctionLess;
        case GL_EQUAL: return MTLCompareFunctionEqual;
        case GL_LEQUAL: return MTLCompareFunctionLessEqual;
        case GL_GREATER: return MTLCompareFunctionGreater;
        case GL_NOTEQUAL: return MTLCompareFunctionNotEqual;
        case GL_GEQUAL: return MTLCompareFunctionGreaterEqual;
        default: return MTLCompareFunctionAlways;
    }
}

MTLStencilOperation stencilOperation(uint32_t value)
{
    switch (value)
    {
        case GL_ZERO: return MTLStencilOperationZero;
        case GL_REPLACE: return MTLStencilOperationReplace;
        case GL_INCR: return MTLStencilOperationIncrementClamp;
        case GL_DECR: return MTLStencilOperationDecrementClamp;
        case GL_INVERT: return MTLStencilOperationInvert;
        case GL_INCR_WRAP: return MTLStencilOperationIncrementWrap;
        case GL_DECR_WRAP: return MTLStencilOperationDecrementWrap;
        default: return MTLStencilOperationKeep;
    }
}

MTLBlendFactor blendFactor(uint32_t value)
{
    switch (value)
    {
        case GL_ZERO: return MTLBlendFactorZero;
        case GL_ONE: return MTLBlendFactorOne;
        case GL_SRC_COLOR: return MTLBlendFactorSourceColor;
        case GL_ONE_MINUS_SRC_COLOR: return MTLBlendFactorOneMinusSourceColor;
        case GL_DST_COLOR: return MTLBlendFactorDestinationColor;
        case GL_ONE_MINUS_DST_COLOR: return MTLBlendFactorOneMinusDestinationColor;
        case GL_SRC_ALPHA: return MTLBlendFactorSourceAlpha;
        case GL_ONE_MINUS_SRC_ALPHA: return MTLBlendFactorOneMinusSourceAlpha;
        case GL_DST_ALPHA: return MTLBlendFactorDestinationAlpha;
        case GL_ONE_MINUS_DST_ALPHA: return MTLBlendFactorOneMinusDestinationAlpha;
        case GL_CONSTANT_COLOR: return MTLBlendFactorBlendColor;
        case GL_ONE_MINUS_CONSTANT_COLOR: return MTLBlendFactorOneMinusBlendColor;
        case GL_CONSTANT_ALPHA: return MTLBlendFactorBlendAlpha;
        case GL_ONE_MINUS_CONSTANT_ALPHA: return MTLBlendFactorOneMinusBlendAlpha;
        case GL_SRC_ALPHA_SATURATE: return MTLBlendFactorSourceAlphaSaturated;
        default: return MTLBlendFactorOne;
    }
}

MTLBlendOperation blendOperation(uint32_t value)
{
    switch (value)
    {
        case GL_FUNC_SUBTRACT: return MTLBlendOperationSubtract;
        case GL_FUNC_REVERSE_SUBTRACT: return MTLBlendOperationReverseSubtract;
        case GL_MIN: return MTLBlendOperationMin;
        case GL_MAX: return MTLBlendOperationMax;
        default: return MTLBlendOperationAdd;
    }
}

MTLBlendFactor alphaBlendFactor(uint32_t value)
{
    // GL's color factors use their alpha component for the alpha equation.
    // Translate explicitly rather than relying on RGB-only Metal enumerants.
    switch (value)
    {
        case GL_SRC_COLOR: return MTLBlendFactorSourceAlpha;
        case GL_ONE_MINUS_SRC_COLOR: return MTLBlendFactorOneMinusSourceAlpha;
        case GL_DST_COLOR: return MTLBlendFactorDestinationAlpha;
        case GL_ONE_MINUS_DST_COLOR: return MTLBlendFactorOneMinusDestinationAlpha;
        case GL_CONSTANT_COLOR: return MTLBlendFactorBlendAlpha;
        case GL_ONE_MINUS_CONSTANT_COLOR: return MTLBlendFactorOneMinusBlendAlpha;
        case GL_SRC_ALPHA_SATURATE: return MTLBlendFactorOne;
        default: return blendFactor(value);
    }
}

MTLPrimitiveType topology(uint32_t value)
{
    switch (value)
    {
        case GL_POINTS: return MTLPrimitiveTypePoint;
        case GL_LINES: return MTLPrimitiveTypeLine;
        case GL_LINE_STRIP: return MTLPrimitiveTypeLineStrip;
        case GL_TRIANGLE_STRIP: return MTLPrimitiveTypeTriangleStrip;
        default: return MTLPrimitiveTypeTriangle;
    }
}

MTLSamplerAddressMode addressMode(uint32_t value)
{
    switch (value)
    {
        case GL_REPEAT: return MTLSamplerAddressModeRepeat;
        case GL_MIRRORED_REPEAT: return MTLSamplerAddressModeMirrorRepeat;
        case GL_CLAMP_TO_BORDER: return MTLSamplerAddressModeClampToBorderColor;
        default: return MTLSamplerAddressModeClampToEdge;
    }
}

id<MTLRenderPipelineState> pipeline(NativeDrawState const &state)
{
    uint32_t mask = 0;
    if (state.colorWrite[0]) mask |= MTLColorWriteMaskRed;
    if (state.colorWrite[1]) mask |= MTLColorWriteMaskGreen;
    if (state.colorWrite[2]) mask |= MTLColorWriteMaskBlue;
    if (state.colorWrite[3]) mask |= MTLColorWriteMaskAlpha;
    PipelineKey key = {{state.blend, state.blendSrc, state.blendDst, state.blendEquation, mask}};
    auto found = g.pipelines.find(key);
    if (found != g.pipelines.end()) return found->second;
    MTLRenderPipelineDescriptor *descriptor = [MTLRenderPipelineDescriptor new];
    descriptor.label = @"NetBlood Polymost material";
    descriptor.vertexFunction = g.vertex;
    descriptor.fragmentFunction = g.fragment;
    descriptor.colorAttachments[0].pixelFormat = kColorFormat;
    descriptor.depthAttachmentPixelFormat = kDepthFormat;
    descriptor.stencilAttachmentPixelFormat = kDepthFormat;
    auto attachment = descriptor.colorAttachments[0];
    attachment.writeMask = static_cast<MTLColorWriteMask>(mask);
    attachment.blendingEnabled = state.blend;
    attachment.sourceRGBBlendFactor = blendFactor(state.blendSrc);
    attachment.destinationRGBBlendFactor = blendFactor(state.blendDst);
    attachment.sourceAlphaBlendFactor = alphaBlendFactor(state.blendSrc);
    attachment.destinationAlphaBlendFactor = alphaBlendFactor(state.blendDst);
    attachment.rgbBlendOperation = attachment.alphaBlendOperation = blendOperation(state.blendEquation);
    MTLVertexDescriptor *vertex = [MTLVertexDescriptor vertexDescriptor];
    for (NSUInteger i = 0; i < 5; ++i)
    {
        vertex.attributes[i].format = MTLVertexFormatFloat4;
        vertex.attributes[i].offset = i * sizeof(float) * 4;
        vertex.attributes[i].bufferIndex = 0;
    }
    vertex.layouts[0].stride = sizeof(NativeVertex);
    vertex.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;
    descriptor.vertexDescriptor = vertex;
    NSError *error = nil;
    id<MTLRenderPipelineState> result = [g.device newRenderPipelineStateWithDescriptor:descriptor error:&error];
    if (!result) { failure(error, "Cannot create a scene pipeline"); return nil; }
    g.pipelines.emplace(key, result);
    return result;
}

id<MTLDepthStencilState> depthState(NativeDrawState const &state)
{
    DepthKey key = {{state.depthTest, state.depthWrite, state.depthFunc, state.stencilTest,
                    state.stencilFunc, state.stencilFail, state.stencilDepthFail, state.stencilPass,
                    state.stencilCompareMask & 255u, state.stencilWriteMask & 255u}};
    auto found = g.depths.find(key);
    if (found != g.depths.end()) return found->second;
    MTLDepthStencilDescriptor *descriptor = [MTLDepthStencilDescriptor new];
    descriptor.depthCompareFunction = state.depthTest ? comparison(state.depthFunc) : MTLCompareFunctionAlways;
    descriptor.depthWriteEnabled = state.depthTest && state.depthWrite;
    if (state.stencilTest)
    {
        MTLStencilDescriptor *stencil = [MTLStencilDescriptor new];
        stencil.stencilCompareFunction = comparison(state.stencilFunc);
        stencil.stencilFailureOperation = stencilOperation(state.stencilFail);
        stencil.depthFailureOperation = stencilOperation(state.stencilDepthFail);
        stencil.depthStencilPassOperation = stencilOperation(state.stencilPass);
        stencil.readMask = state.stencilCompareMask & 255u;
        stencil.writeMask = state.stencilWriteMask & 255u;
        descriptor.frontFaceStencil = descriptor.backFaceStencil = stencil;
    }
    id<MTLDepthStencilState> result = [g.device newDepthStencilStateWithDescriptor:descriptor];
    if (!result) { fail("Cannot create depth/stencil state"); return nil; }
    g.depths.emplace(key, result);
    return result;
}

id<MTLSamplerState> sampler(NativeSamplerState const &state)
{
    SamplerKey key = {{state.minFilter, state.magFilter, state.wrapS, state.wrapT, 0, 0, 0}};
    std::memcpy(&key.values[4], &state.anisotropy, sizeof(float));
    std::memcpy(&key.values[5], &state.minLod, sizeof(float));
    std::memcpy(&key.values[6], &state.maxLod, sizeof(float));
    auto found = g.samplers.find(key);
    if (found != g.samplers.end()) return found->second;
    MTLSamplerDescriptor *descriptor = [MTLSamplerDescriptor new];
    descriptor.minFilter = state.minFilter == GL_NEAREST || state.minFilter == GL_NEAREST_MIPMAP_NEAREST ||
                           state.minFilter == GL_NEAREST_MIPMAP_LINEAR ? MTLSamplerMinMagFilterNearest : MTLSamplerMinMagFilterLinear;
    descriptor.magFilter = state.magFilter == GL_NEAREST ? MTLSamplerMinMagFilterNearest : MTLSamplerMinMagFilterLinear;
    bool const noMips = state.minFilter == GL_NEAREST || state.minFilter == GL_LINEAR;
    descriptor.mipFilter = noMips ? MTLSamplerMipFilterNotMipmapped :
        (state.minFilter == GL_NEAREST_MIPMAP_LINEAR || state.minFilter == GL_LINEAR_MIPMAP_LINEAR ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNearest);
    descriptor.sAddressMode = addressMode(state.wrapS);
    descriptor.tAddressMode = addressMode(state.wrapT);
    descriptor.rAddressMode = MTLSamplerAddressModeClampToEdge;
    descriptor.borderColor = MTLSamplerBorderColorTransparentBlack;
    descriptor.maxAnisotropy = static_cast<NSUInteger>(std::max(1.f, std::min(16.f, state.anisotropy)));
    descriptor.lodMinClamp = std::max(0.f, state.minLod);
    descriptor.lodMaxClamp = noMips ? descriptor.lodMinClamp : std::max(descriptor.lodMinClamp, state.maxLod);
    id<MTLSamplerState> result = [g.device newSamplerStateWithDescriptor:descriptor];
    if (!result) { fail("Cannot create a texture sampler"); return nil; }
    g.samplers.emplace(key, result);
    return result;
}

MTLScissorRect clipRect(NativeDrawState const &state)
{
    int64_t x0 = 0, y0 = 0, x1 = g.width, y1 = g.height;
    if (state.scissorTest)
    {
        x0 = std::max<int64_t>(0, state.scissor[0]);
        y0 = std::max<int64_t>(0, int64_t(g.height) - state.scissor[1] - state.scissor[3]);
        x1 = std::min<int64_t>(x1, int64_t(state.scissor[0]) + state.scissor[2]);
        y1 = std::min<int64_t>(y1, int64_t(g.height) - state.scissor[1]);
    }
    return { static_cast<NSUInteger>(x0), static_cast<NSUInteger>(y0),
             static_cast<NSUInteger>(std::max<int64_t>(0, x1 - x0)),
             static_cast<NSUInteger>(std::max<int64_t>(0, y1 - y0)) };
}

void transformedPosition(NativeVertex const &vertex, NativeUniforms const &uniforms, double result[4])
{
    auto multiply = [](float const matrix[16], double const input[4], double output[4])
    {
        for (unsigned row = 0; row < 4; ++row)
        {
            output[row] = 0;
            for (unsigned column = 0; column < 4; ++column) output[row] += matrix[column * 4 + row] * input[column];
        }
    };
    double position[4], temporary[4];
    std::copy(vertex.position, vertex.position + 4, position);
    if (uniforms.misc[3] == 3.f) { std::copy(position, position + 4, result); return; }
    if (uniforms.misc[3] == 4.f)
        multiply(uniforms.projection, position, result);
    else if (uniforms.misc[3] == 1.f)
    {
        multiply(uniforms.rotation, position, temporary);
        multiply(uniforms.projection, temporary, result);
    }
    else
    {
        multiply(uniforms.modelView, position, temporary);
        multiply(uniforms.projection, temporary, result);
    }
}

void polygonPoints(NativeVertex const *vertices, unsigned count, NativeDrawState const &state,
                   std::vector<NativeVertex> &points)
{
    // Metal exposes line/fill polygon rasterization, but no point fill mode.
    // The frontend's r_polygonmode=2 visualization therefore submits the source
    // triangle vertices as points. Face culling must happen before expansion:
    // Metal's point primitives themselves have no front/back face.
    auto append = [&state, &points](NativeVertex const &a, NativeVertex const &b, NativeVertex const &c)
    {
        if (state.cull)
        {
            double clip[3][4];
            transformedPosition(a, state.uniforms, clip[0]);
            transformedPosition(b, state.uniforms, clip[1]);
            transformedPosition(c, state.uniforms, clip[2]);
            // Homogeneous signed area avoids division at w=0 and retains the
            // visible polygon's orientation across the camera's clip plane.
            double const area = clip[0][0] * (clip[1][1] * clip[2][3] - clip[2][1] * clip[1][3])
                              - clip[0][1] * (clip[1][0] * clip[2][3] - clip[2][0] * clip[1][3])
                              + clip[0][3] * (clip[1][0] * clip[2][1] - clip[2][0] * clip[1][1]);
            bool const front = (area > 0.) == (state.frontFace != GL_CW);
            if (area == 0. || !std::isfinite(area) ||
                (state.cullFace == GL_FRONT ? front : !front)) return;
        }
        points.push_back(a); points.push_back(b); points.push_back(c);
    };
    if (state.topology == GL_TRIANGLE_STRIP)
    {
        for (unsigned i = 2; i < count; ++i)
            if (i & 1) append(vertices[i - 1], vertices[i - 2], vertices[i]);
            else append(vertices[i - 2], vertices[i - 1], vertices[i]);
    }
    else
        for (unsigned i = 0; i + 2 < count; i += 3) append(vertices[i], vertices[i + 1], vertices[i + 2]);
}

bool createTargets(int width, int height)
{
    if (width <= 0 || height <= 0) { g.suspended = true; return true; }
    if (width > 16384 || height > 16384) return fail("Render target exceeds the Metal device limit");
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kColorFormat
        width:width height:height mipmapped:NO];
    descriptor.storageMode = MTLStorageModePrivate;
    descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLTexture> color = [g.device newTextureWithDescriptor:descriptor];
    descriptor.pixelFormat = kDepthFormat;
    descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
    id<MTLTexture> depth = [g.device newTextureWithDescriptor:descriptor];
    id<MTLTexture> stencil = [depth newTextureViewWithPixelFormat:MTLPixelFormatX32_Stencil8];
    if (!color || !depth || !stencil) return fail("Cannot allocate the scene color/depth/stencil targets");
    color.label = @"NetBlood logical-resolution scene color";
    depth.label = @"NetBlood world depth and room-over-room stencil";
    g.color = color;
    g.depth = depth;
    g.stencil = stencil;
    g.width = width;
    g.height = height;
    g.initialized = false;
    g.suspended = false;
    return true;
}

id<MTLRenderPipelineState> postprocessPipeline(NSString *vertex, NSString *fragment, bool ambient)
{
    MTLRenderPipelineDescriptor *descriptor = [MTLRenderPipelineDescriptor new];
    descriptor.vertexFunction = [g.library newFunctionWithName:vertex];
    descriptor.fragmentFunction = [g.library newFunctionWithName:fragment];
    descriptor.colorAttachments[0].pixelFormat = kColorFormat;
    if (!descriptor.vertexFunction || !descriptor.fragmentFunction) { fail("A required Metal shader entry point is missing"); return nil; }
    if (ambient)
    {
        auto color = descriptor.colorAttachments[0];
        color.writeMask = MTLColorWriteMaskRed | MTLColorWriteMaskGreen | MTLColorWriteMaskBlue;
        color.blendingEnabled = YES;
        color.sourceRGBBlendFactor = MTLBlendFactorZero;
        color.destinationRGBBlendFactor = MTLBlendFactorSourceColor;
        color.sourceAlphaBlendFactor = MTLBlendFactorZero;
        color.destinationAlphaBlendFactor = MTLBlendFactorOne;
    }
    NSError *error = nil;
    id<MTLRenderPipelineState> result = [g.device newRenderPipelineStateWithDescriptor:descriptor error:&error];
    if (!result) failure(error, "Cannot create a postprocessing pipeline");
    return result;
}

bool disableRealtimeLighting(char const *reason, NSError *error = nil)
{
    if (!g.lighting.failed)
    {
        std::fprintf(stderr, "Metal: realtime lighting disabled; %s", reason);
        if (error) std::fprintf(stderr, ": %s", error.localizedDescription.UTF8String);
        std::fprintf(stderr, "\n");
    }
    // Command buffers retain already encoded resources. Releasing these CPU
    // references cannot invalidate a preceding frame's atlas or pipeline.
    g.lighting = RealtimeLighting{};
    g.lighting.failed = true;
    return false;
}

bool createRealtimeLightingResources()
{
    if (g.lighting.failed) return false;
    if (g.lighting.pipeline) return true;
    if (!g.depth || !g.stencil) return disableRealtimeLighting("sampled scene depth/stencil is unavailable");
    MTLRenderPipelineDescriptor *descriptor = [MTLRenderPipelineDescriptor new];
    descriptor.vertexFunction = [g.library newFunctionWithName:@"metalAOVertex"];
    descriptor.fragmentFunction = [g.library newFunctionWithName:@"metalLightingFragment"];
    if (!descriptor.vertexFunction || !descriptor.fragmentFunction)
        return disableRealtimeLighting("lighting shader entry points are unavailable");
    auto color = descriptor.colorAttachments[0];
    color.pixelFormat = kColorFormat;
    color.writeMask = MTLColorWriteMaskRed | MTLColorWriteMaskGreen | MTLColorWriteMaskBlue;
    color.blendingEnabled = YES;
    color.rgbBlendOperation = color.alphaBlendOperation = MTLBlendOperationAdd;
    color.sourceRGBBlendFactor = MTLBlendFactorDestinationColor;
    color.destinationRGBBlendFactor = MTLBlendFactorOne;
    color.sourceAlphaBlendFactor = MTLBlendFactorZero;
    color.destinationAlphaBlendFactor = MTLBlendFactorOne;
    NSError *error = nil;
    g.lighting.pipeline = [g.device newRenderPipelineStateWithDescriptor:descriptor error:&error];
    if (!g.lighting.pipeline) return disableRealtimeLighting("cannot create the lighting pipeline", error);

    descriptor = [MTLRenderPipelineDescriptor new];
    descriptor.vertexFunction = [g.library newFunctionWithName:@"metalShadowVertex"];
    descriptor.fragmentFunction = [g.library newFunctionWithName:@"metalShadowFragment"];
    descriptor.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    if (!descriptor.vertexFunction || !descriptor.fragmentFunction)
        return disableRealtimeLighting("shadow shader entry points are unavailable");
    error = nil;
    g.lighting.shadowPipeline = [g.device newRenderPipelineStateWithDescriptor:descriptor error:&error];
    if (!g.lighting.shadowPipeline) return disableRealtimeLighting("cannot create the shadow pipeline", error);
    MTLDepthStencilDescriptor *depth = [MTLDepthStencilDescriptor new];
    depth.depthCompareFunction = MTLCompareFunctionLessEqual;
    depth.depthWriteEnabled = YES;
    g.lighting.shadowDepth = [g.device newDepthStencilStateWithDescriptor:depth];
    if (!g.lighting.shadowDepth) return disableRealtimeLighting("cannot create the shadow depth state");
    MTLTextureDescriptor *texture = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
        width:kShadowFaceSize * kMaxRealtimeLights height:kShadowFaceSize * kShadowFaces mipmapped:NO];
    texture.storageMode = MTLStorageModePrivate;
    texture.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    // Device-created textures use tracked hazards. Separate encoders and the
    // single queue order atlas reads/writes, including across in-flight frames.
    g.lighting.atlas = [g.device newTextureWithDescriptor:texture];
    if (!g.lighting.atlas) return disableRealtimeLighting("sampled shadow depth textures are unavailable");
    g.lighting.atlas.label = @"NetBlood realtime point-light shadow atlas";
    return true;
}

bool copyPixels(Texture const &texture, int level, int x, int y, int width, int height, uint8_t const *rgba)
{
    NSUInteger const rowBytes = static_cast<NSUInteger>(width) * 4;
    // 256-byte rows work for both Intel and Apple Silicon transfer encoders.
    NSUInteger const stride = (rowBytes + 255) & ~NSUInteger(255);
    NSUInteger const rowsPerChunk = std::max<NSUInteger>(1, kStreamPageBytes / stride);
    for (NSUInteger row = 0; row < static_cast<NSUInteger>(height); )
    {
        NSUInteger const rows = std::min(rowsPerChunk, static_cast<NSUInteger>(height) - row);
        StreamAllocation allocation;
        if (!reserveStream(stride * rows, 256, allocation)) return false;
        if (rgba)
            for (NSUInteger i = 0; i < rows; ++i)
                std::memcpy(allocation.data + i * stride, rgba + (row + i) * rowBytes, rowBytes);
        else std::memset(allocation.data, 255, stride * rows);
        if (!beginBlit()) return false;
        [g.blit copyFromBuffer:allocation.buffer sourceOffset:allocation.offset sourceBytesPerRow:stride
            sourceBytesPerImage:stride * rows sourceSize:MTLSizeMake(width, rows, 1)
            toTexture:texture.image destinationSlice:0 destinationLevel:level
            destinationOrigin:MTLOriginMake(x, y + row, 0)];
        row += rows;
    }
    return true;
}
} // namespace

bool metalSupported()
{
    // Display-mode enumeration asks this repeatedly. Keep the probe independent
    // of the active backend and create the system device only once for it.
    static bool const supported = []() { @autoreleasepool { return MTLCreateSystemDefaultDevice() != nil; } }();
    return supported;
}

bool metalCreate(SDL_Window *window, int width, int height, int vsync)
{
    @autoreleasepool
    {
        metalDestroy();
        if (!window || width <= 0 || height <= 0) return fail("Invalid Metal window dimensions");
        g.window = window;
        g.vsync = vsync;
        g.device = MTLCreateSystemDefaultDevice();
        if (!g.device) return fail("This Mac does not provide a Metal-capable graphics device");
        g.name = g.device.name.UTF8String;
        g.view = SDL_Metal_CreateView(window);
        if (!g.view) { std::string error = SDL_GetError(); metalDestroy(); return fail(error.c_str()); }
        g.layer = (__bridge CAMetalLayer *)SDL_Metal_GetLayer(g.view);
        if (!g.layer) { metalDestroy(); return fail("SDL did not create a CAMetalLayer"); }
        g.layer.device = g.device;
        g.layer.pixelFormat = kColorFormat;
        g.layer.framebufferOnly = YES;
        g.layer.opaque = YES;
        g.layer.presentsWithTransaction = NO;
        g.layer.displaySyncEnabled = vsync != 0;
        g.layer.maximumDrawableCount = kFramesInFlight;
        g.layer.allowsNextDrawableTimeout = YES;
        g.queue = [g.device newCommandQueue];
        if (!g.queue) { metalDestroy(); return fail("Cannot create a Metal command queue"); }
        MTLCompileOptions *options = [MTLCompileOptions new];
        options.languageVersion = MTLLanguageVersion2_3;
        // Material and depth arithmetic must retain the frontend's operation order.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        // Still required for the macOS 11 deployment target; newer SDKs mark
        // this setter deprecated in favor of macOS 15-only mathMode.
        options.fastMathEnabled = NO;
#pragma clang diagnostic pop
        NSError *error = nil;
        g.library = [g.device newLibraryWithSource:[NSString stringWithUTF8String:kMetalShaderSource]
            options:options error:&error];
        if (!g.library) { failure(error, "Cannot compile the native Metal shaders"); metalDestroy(); return false; }
        g.vertex = [g.library newFunctionWithName:@"metalSceneVertex"];
        g.fragment = [g.library newFunctionWithName:@"metalSceneFragment"];
        g.ambientPipeline = postprocessPipeline(@"metalAOVertex", @"metalAOFragment", true);
        g.presentPipeline = postprocessPipeline(@"metalPresentVertex", @"metalPresentFragment", false);
        MTLDepthStencilDescriptor *depth = [MTLDepthStencilDescriptor new];
        depth.depthCompareFunction = MTLCompareFunctionAlways;
        depth.depthWriteEnabled = NO;
        g.noDepth = [g.device newDepthStencilStateWithDescriptor:depth];
        NativeSamplerState nearest = {};
        nearest.minFilter = nearest.magFilter = GL_NEAREST;
        nearest.wrapS = nearest.wrapT = GL_CLAMP_TO_EDGE;
        nearest.anisotropy = 1.f;
        g.nearestSampler = sampler(nearest);
        nearest.minFilter = nearest.magFilter = GL_LINEAR;
        g.presentSampler = sampler(nearest);
        if (!g.vertex || !g.fragment || !g.ambientPipeline || !g.presentPipeline || !g.noDepth ||
            !g.nearestSampler || !g.presentSampler || !createTargets(width, height))
        {
            if (!g.failed) fail("Cannot initialize native Metal resources");
            metalDestroy();
            return false;
        }
        g.ready = true;
        uint8_t const white[4] = { 255, 255, 255, 255 };
        metalUploadTexture(0, 0, 1, 1, white);
        if (g.failed) { metalDestroy(); return false; }
        std::fprintf(stderr, "Metal: %s, three submissions in flight\n", g.name.c_str());
        return true;
    }
}

void metalDestroy()
{
    @autoreleasepool
    {
        // Even a failed encoder may have submitted work that still owns targets.
        // Teardown waits for it before removing SDL's native view.
        if (g.command) submit();
        for (auto &slot : g.slots) if (slot.submitted) waitCommand(slot.submitted);
        SDL_MetalView view = g.view;
        g = Backend{};
        if (view) SDL_Metal_DestroyView(view);
    }
}

bool metalActive() { return g.ready && g.device != nil; }

bool metalResize(int width, int height)
{
    @autoreleasepool
    {
        if (!metalActive() || g.failed) return false;
        if (width <= 0 || height <= 0) { g.suspended = true; return true; }
        if (!g.suspended && width == g.width && height == g.height) return true;
        // Commands retain old targets until completion. Resize requires neither
        // a device-wide idle wait nor destroying virtual textures/materials.
        if (!submit()) return false;
        return createTargets(width, height);
    }
}

void metalSetVsync(int vsync)
{
    @autoreleasepool
    {
        g.vsync = vsync;
        if (g.layer) g.layer.displaySyncEnabled = vsync != 0;
    }
}

void metalFinish()
{
    @autoreleasepool
    {
        if (!metalActive()) return;
        submit();
        for (auto &slot : g.slots) if (slot.submitted) waitCommand(slot.submitted);
    }
}

void metalFlush() { @autoreleasepool { if (metalActive() && !g.failed) submit(); } }

bool metalPresent()
{
    @autoreleasepool
    {
        if (!metalActive() || g.failed) return false;
        int width = 0, height = 0;
        SDL_Metal_GetDrawableSize(g.window, &width, &height);
        if (width <= 0 || height <= 0 || g.suspended) return submit();
        g.layer.drawableSize = CGSizeMake(width, height);
        // Clear an untouched scene before sampling it (e.g. a new video mode).
        if (!g.initialized && !beginRender()) return false;
        endEncoding();
        id<CAMetalDrawable> drawable = [g.layer nextDrawable];
        // Occluded/minimized windows can temporarily have no drawable.
        if (!drawable) return submit();
        if (!beginCommands()) return false;
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = drawable.texture;
        pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> encoder = [g.command renderCommandEncoderWithDescriptor:pass];
        if (!encoder) return fail("Cannot create the drawable presentation encoder");
        [encoder setRenderPipelineState:g.presentPipeline];
        [encoder setDepthStencilState:g.noDepth];
        [encoder setFragmentTexture:g.color atIndex:0];
        [encoder setFragmentSamplerState:g.presentSampler atIndex:0];
        [encoder setViewport:MTLViewport{0, 0, double(drawable.texture.width), double(drawable.texture.height), 0, 1}];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [encoder endEncoding];
        return submit(drawable);
    }
}

void metalDraw(NativeVertex const *vertices, unsigned count, NativeDrawState const &state)
{
    @autoreleasepool
    {
        if (!metalActive() || g.failed || g.suspended || !vertices || !count || state.viewport[2] <= 0 || state.viewport[3] <= 0) return;
        MTLScissorRect const scissor = clipRect(state);
        if (!scissor.width || !scissor.height) return;
        bool const triangles = state.topology == GL_TRIANGLES || state.topology == GL_TRIANGLE_STRIP;
        if (state.cull && state.cullFace == GL_FRONT_AND_BACK && triangles) return;
        std::vector<NativeVertex> points;
        bool const pointPolygons = triangles && state.polygonMode == GL_POINT;
        if (pointPolygons)
        {
            polygonPoints(vertices, count, state, points);
            if (points.empty()) return;
            if (points.size() > std::numeric_limits<unsigned>::max()) { fail("Point draw exceeds the Metal vertex limit"); return; }
            vertices = points.data();
            count = static_cast<unsigned>(points.size());
        }
        id<MTLRenderPipelineState> material = pipeline(state);
        id<MTLDepthStencilState> depth = depthState(state);
        if (!material || !depth) return;
        // Reserve vertices and uniforms atomically: crossing the stream limit
        // must never put vertex data in a recycled slot before the draw uses it.
        NSUInteger const vertexBytes = static_cast<NSUInteger>(count) * sizeof(NativeVertex);
        NSUInteger const uniformOffset = (vertexBytes + 255) & ~NSUInteger(255);
        if (uniformOffset > kSubmissionBytes - sizeof(NativeUniforms)) { fail("Draw exceeds the Metal streaming-buffer limit"); return; }
        StreamAllocation allocation;
        if (!reserveStream(uniformOffset + sizeof(NativeUniforms), 256, allocation)) return;
        std::memcpy(allocation.data, vertices, vertexBytes);
        std::memcpy(allocation.data + uniformOffset, &state.uniforms, sizeof(NativeUniforms));
        if (!beginRender()) return;
        [g.render setRenderPipelineState:material];
        [g.render setDepthStencilState:depth];
        [g.render setStencilReferenceValue:state.stencilReference & 255u];
        double const nearDepth = std::max(0.f, std::min(1.f, state.depthRange[0]));
        double const farDepth = std::max(0.f, std::min(1.f, state.depthRange[1]));
        // Metal requires znear <= zfar. Reverse the shader's 0..w clip depth to
        // reproduce GL's legal reversed depth range without changing its ABI.
        float const viewportState[4] = { nearDepth > farDepth ? 1.f : 0.f, 0.f, 0.f, 0.f };
        [g.render setViewport:MTLViewport{double(state.viewport[0]), double(g.height) - state.viewport[1] - state.viewport[3],
            double(state.viewport[2]), double(state.viewport[3]), std::min(nearDepth, farDepth), std::max(nearDepth, farDepth)}];
        [g.render setScissorRect:scissor];
        [g.render setCullMode:!state.cull ? MTLCullModeNone : state.cullFace == GL_FRONT ? MTLCullModeFront : MTLCullModeBack];
        [g.render setFrontFacingWinding:state.frontFace == GL_CW ? MTLWindingClockwise : MTLWindingCounterClockwise];
        [g.render setTriangleFillMode:state.polygonMode == GL_LINE ? MTLTriangleFillModeLines : MTLTriangleFillModeFill];
        [g.render setDepthClipMode:state.depthClamp ? MTLDepthClipModeClamp : MTLDepthClipModeClip];
        [g.render setDepthBias:state.depthBias ? state.depthBiasUnits : 0.f
            slopeScale:state.depthBias ? state.depthBiasFactor : 0.f clamp:0.f];
        [g.render setBlendColorRed:0 green:0 blue:0 alpha:0];
        [g.render setVertexBuffer:allocation.buffer offset:allocation.offset atIndex:0];
        [g.render setVertexBuffer:allocation.buffer offset:allocation.offset + uniformOffset atIndex:1];
        [g.render setVertexBytes:viewportState length:sizeof(viewportState) atIndex:2];
        [g.render setFragmentBuffer:allocation.buffer offset:allocation.offset + uniformOffset atIndex:1];
        for (unsigned i = 0; i < 5; ++i)
        {
            auto texture = g.textures.find(state.samplers[i].texture);
            if (texture == g.textures.end()) texture = g.textures.find(0);
            if (texture == g.textures.end()) { fail("Missing fallback texture"); return; }
            id<MTLSamplerState> sampling = sampler(state.samplers[i]);
            if (!sampling) return;
            [g.render setFragmentTexture:texture->second.image atIndex:i];
            [g.render setFragmentSamplerState:sampling atIndex:i];
        }
        [g.render drawPrimitives:pointPolygons ? MTLPrimitiveTypePoint : topology(state.topology) vertexStart:0 vertexCount:count];
    }
}

void metalClear(uint32_t mask, float const color[4], float depth, uint32_t stencil, NativeDrawState const &state)
{
    @autoreleasepool
    {
        if (!metalActive() || g.failed || g.suspended) return;
        MTLScissorRect const rect = clipRect(state);
        if (!rect.width || !rect.height) return;
        bool anyColor = false, allColor = true;
        for (bool enabled : state.colorWrite) { anyColor |= enabled; allColor &= enabled; }
        if (!anyColor) mask &= ~GL_COLOR_BUFFER_BIT;
        if (!state.depthWrite) mask &= ~GL_DEPTH_BUFFER_BIT;
        if (!(state.stencilWriteMask & 255u)) mask &= ~GL_STENCIL_BUFFER_BIT;
        mask &= GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT;
        if (!mask) return;
        bool const fullRect = rect.x == 0 && rect.y == 0 && rect.width == static_cast<NSUInteger>(g.width) && rect.height == static_cast<NSUInteger>(g.height);
        if (fullRect && (!(mask & GL_COLOR_BUFFER_BIT) || allColor) &&
            (!(mask & GL_STENCIL_BUFFER_BIT) || (state.stencilWriteMask & 255u) == 255u))
        {
            endEncoding();
            if (!beginCommands()) return;
            MTLRenderPassDescriptor *pass = scenePass();
            if (mask & GL_COLOR_BUFFER_BIT)
            {
                pass.colorAttachments[0].loadAction = MTLLoadActionClear;
                pass.colorAttachments[0].clearColor = MTLClearColorMake(color[0], color[1], color[2], color[3]);
            }
            if (mask & GL_DEPTH_BUFFER_BIT)
            {
                pass.depthAttachment.loadAction = MTLLoadActionClear;
                pass.depthAttachment.clearDepth = std::max(0.f, std::min(1.f, depth));
            }
            if (mask & GL_STENCIL_BUFFER_BIT)
            {
                pass.stencilAttachment.loadAction = MTLLoadActionClear;
                pass.stencilAttachment.clearStencil = stencil & 255u;
            }
            g.render = [g.command renderCommandEncoderWithDescriptor:pass];
            if (!g.render) { fail("Cannot create the clear encoder"); return; }
            g.initialized = true;
            return;
        }
        // Metal attachment clears cannot apply a scissor or channel/bit masks.
        // An ordinary native draw preserves untouched color, depth and stencil.
        NativeDrawState clear = {};
        clear.viewport[2] = g.width;
        clear.viewport[3] = g.height;
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
        for (unsigned i = 0; i < 4; ++i)
        {
            clear.colorWrite[i] = (mask & GL_COLOR_BUFFER_BIT) && state.colorWrite[i];
            clear.uniforms.modelView[i * 5] = clear.uniforms.projection[i * 5] = 1.f;
        }
        clear.uniforms.effects[0] = clear.uniforms.misc[0] = 1.f;
        clear.uniforms.correction[0] = clear.uniforms.correction[1] = 1.f;
        for (auto &sample : clear.samplers)
        {
            sample.minFilter = sample.magFilter = GL_NEAREST;
            sample.wrapS = sample.wrapT = GL_CLAMP_TO_EDGE;
            sample.anisotropy = 1.f;
        }
        NativeVertex vertices[3] = {};
        float const positions[3][2] = {{-1.f, -1.f}, {3.f, -1.f}, {-1.f, 3.f}};
        for (unsigned i = 0; i < 3; ++i)
        {
            vertices[i].position[0] = positions[i][0];
            vertices[i].position[1] = positions[i][1];
            vertices[i].position[2] = std::max(0.f, std::min(1.f, depth)) * 2.f - 1.f;
            vertices[i].position[3] = 1.f;
            std::copy(color, color + 4, vertices[i].color);
        }
        metalDraw(vertices, 3, clear);
    }
}

bool metalAmbientOcclusion(float const projection[16], int const worldViewport[4], float radius, float strength, float bias)
{
    @autoreleasepool
    {
        if (!metalActive() || g.failed) return false;
        if (radius <= 0.f || strength <= 0.f || g.suspended) return true;
        if (!projection || !worldViewport || !std::isfinite(radius) || !std::isfinite(strength) || !std::isfinite(bias)) return false;
        for (unsigned i = 0; i < 16; ++i) if (!std::isfinite(projection[i])) return false;
        if (worldViewport[2] <= 0 || worldViewport[3] <= 0) return true;
        if (projection[0] <= 0.f || projection[5] <= 0.f || std::fabs(projection[11] - 1.f) > .00001f || std::fabs(projection[15]) > .00001f) return false;
        NativeDrawState clipping = {};
        clipping.scissorTest = true;
        std::copy(worldViewport, worldViewport + 4, clipping.scissor);
        MTLScissorRect const scissor = clipRect(clipping);
        if (!scissor.width || !scissor.height) return true;
        if (!g.initialized && !beginRender()) return false;
        endEncoding();
        StreamAllocation allocation;
        if (!reserveStream(sizeof(AmbientOcclusionUniforms), 256, allocation)) return false;
        AmbientOcclusionUniforms uniforms = {};
        uniforms.projection[0] = projection[0]; uniforms.projection[1] = projection[5];
        uniforms.projection[2] = projection[8]; uniforms.projection[3] = projection[9];
        uniforms.depth[0] = projection[10]; uniforms.depth[1] = projection[14];
        uniforms.depth[2] = radius; uniforms.depth[3] = std::max(0.f, std::min(1.f, bias));
        for (unsigned i = 0; i < 4; ++i) uniforms.viewport[i] = float(worldViewport[i]);
        uniforms.target[0] = 1.f / g.width; uniforms.target[1] = 1.f / g.height; uniforms.target[2] = strength;
        std::memcpy(allocation.data, &uniforms, sizeof(uniforms));
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = g.color;
        pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> encoder = [g.command renderCommandEncoderWithDescriptor:pass];
        if (!encoder) return fail("Cannot create the ambient-occlusion encoder");
        [encoder setRenderPipelineState:g.ambientPipeline];
        [encoder setDepthStencilState:g.noDepth];
        [encoder setFragmentBuffer:allocation.buffer offset:allocation.offset atIndex:0];
        [encoder setFragmentTexture:g.depth atIndex:0];
        [encoder setFragmentTexture:g.stencil atIndex:1];
        [encoder setFragmentSamplerState:g.nearestSampler atIndex:0];
        [encoder setViewport:MTLViewport{0, 0, double(g.width), double(g.height), 0, 1}];
        [encoder setScissorRect:scissor];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [encoder endEncoding];
        return true;
    }
}

bool metalRealtimeLighting(float const projection[16], float const eyeToWorld[16], int const worldViewport[4],
                           RealtimeLight const *lights, unsigned lightCount,
                           RealtimeShadowVertex const *shadowVertices, unsigned shadowVertexCount, bool shadows)
{
    @autoreleasepool
    {
        if (!metalActive() || g.failed) return false;
        // Off and zero-light paths leave scene encoding and submission alone.
        if (!lightCount || g.suspended) return true;
        if (!projection || !eyeToWorld || !worldViewport || !lights || lightCount > kMaxRealtimeLights) return false;
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
            std::fabs(projection[11] - 1.f) > .00001f || std::fabs(projection[15]) > .00001f) return false;
        NativeDrawState clipping = {};
        clipping.scissorTest = true;
        std::copy(worldViewport, worldViewport + 4, clipping.scissor);
        MTLScissorRect const scissor = clipRect(clipping);
        if (!scissor.width || !scissor.height) return true;
        unsigned const vertexCount = shadows ? shadowVertexCount : 0;
        // Keep optional oversized work from failing the otherwise usable scene
        // renderer. The map frontend supplies a much smaller bounded mesh.
        if (vertexCount > (kSubmissionBytes - sizeof(RealtimeLightingUniforms)) / sizeof(RealtimeShadowVertex)) return false;
        if (!createRealtimeLightingResources()) return false;
        if (!g.initialized && !beginRender()) return false;
        endEncoding();
        NSUInteger const vertexBytes = NSUInteger(vertexCount) * sizeof(RealtimeShadowVertex);
        StreamAllocation allocation;
        // A reservation may submit and advance the ring. Reserving the complete
        // call at once keeps its mesh and uniforms in the same owned stream page.
        if (!reserveStream(sizeof(RealtimeLightingUniforms) + vertexBytes, 256, allocation)) return false;
        RealtimeLightingUniforms uniforms = {};
        uniforms.ao.projection[0] = projection[0]; uniforms.ao.projection[1] = projection[5];
        uniforms.ao.projection[2] = projection[8]; uniforms.ao.projection[3] = projection[9];
        uniforms.ao.depth[0] = projection[10]; uniforms.ao.depth[1] = projection[14];
        for (unsigned i = 0; i < 4; ++i) uniforms.ao.viewport[i] = float(worldViewport[i]);
        uniforms.ao.target[0] = 1.f / g.width; uniforms.ao.target[1] = 1.f / g.height;
        std::memcpy(uniforms.eyeToWorld, eyeToWorld, sizeof(uniforms.eyeToWorld));
        uniforms.params[0] = float(lightCount);
        uniforms.params[1] = shadows ? 1.f : 0.f;
        uniforms.params[2] = float(kShadowFaceSize);
        uniforms.params[3] = float(kMaxRealtimeLights);
        std::memcpy(uniforms.lights, lights, lightCount * sizeof(RealtimeLight));
        std::memcpy(allocation.data, &uniforms, sizeof(uniforms));
        if (vertexBytes) std::memcpy(allocation.data + sizeof(uniforms), shadowVertices, vertexBytes);

        if (shadows || !g.lighting.initialized)
        {
            MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.depthAttachment.texture = g.lighting.atlas;
            pass.depthAttachment.loadAction = MTLLoadActionClear;
            pass.depthAttachment.storeAction = MTLStoreActionStore;
            pass.depthAttachment.clearDepth = 1.;
            id<MTLRenderCommandEncoder> encoder = [g.command renderCommandEncoderWithDescriptor:pass];
            if (!encoder) return disableRealtimeLighting("cannot create the shadow encoder");
            encoder.label = @"NetBlood point-light world geometry shadows";
            [encoder setRenderPipelineState:g.lighting.shadowPipeline];
            [encoder setDepthStencilState:g.lighting.shadowDepth];
            [encoder setCullMode:MTLCullModeNone];
            [encoder setDepthClipMode:MTLDepthClipModeClip];
            [encoder setTriangleFillMode:MTLTriangleFillModeFill];
            [encoder setDepthBias:0.f slopeScale:0.f clamp:0.f];
            if (vertexCount)
            {
                [encoder setVertexBuffer:allocation.buffer offset:allocation.offset + sizeof(uniforms) atIndex:0];
                for (unsigned light = 0; light < lightCount; ++light)
                    for (NSUInteger face = 0; face < kShadowFaces; ++face)
                    {
                        ShadowUniforms shadow = {};
                        std::memcpy(shadow.light, lights[light].positionRadius, sizeof(shadow.light));
                        shadow.face[0] = int32_t(face);
                        // Metal copies these small per-face constants into the
                        // encoded command; stack reuse cannot alter GPU work.
                        [encoder setVertexBytes:&shadow length:sizeof(shadow) atIndex:1];
                        [encoder setFragmentBytes:&shadow length:sizeof(shadow) atIndex:1];
                        [encoder setViewport:MTLViewport{double(light * kShadowFaceSize), double(face * kShadowFaceSize),
                            double(kShadowFaceSize), double(kShadowFaceSize), 0., 1.}];
                        [encoder setScissorRect:MTLScissorRect{light * kShadowFaceSize, face * kShadowFaceSize,
                            kShadowFaceSize, kShadowFaceSize}];
                        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:vertexCount];
                    }
            }
            [encoder endEncoding];
            g.lighting.initialized = true;
        }

        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = g.color;
        pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> encoder = [g.command renderCommandEncoderWithDescriptor:pass];
        if (!encoder) return disableRealtimeLighting("cannot create the lighting encoder");
        encoder.label = @"NetBlood realtime world point lighting";
        [encoder setRenderPipelineState:g.lighting.pipeline];
        [encoder setDepthStencilState:g.noDepth];
        [encoder setFragmentBuffer:allocation.buffer offset:allocation.offset atIndex:0];
        [encoder setFragmentTexture:g.depth atIndex:0];
        [encoder setFragmentTexture:g.stencil atIndex:1];
        [encoder setFragmentTexture:g.lighting.atlas atIndex:2];
        [encoder setFragmentSamplerState:g.nearestSampler atIndex:0];
        [encoder setViewport:MTLViewport{0, 0, double(g.width), double(g.height), 0, 1}];
        [encoder setScissorRect:scissor];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [encoder endEncoding];
        return true;
    }
}

void metalUploadTexture(uint32_t texture, int level, int width, int height, uint8_t const *rgba)
{
    @autoreleasepool
    {
        if (!metalActive() || g.failed || level < 0 || level >= 31 || width <= 0 || height <= 0) return;
        auto existing = g.textures.find(texture);
        bool const fits = existing != g.textures.end() && static_cast<uint32_t>(level) < existing->second.levels &&
            std::max(1u, existing->second.width >> level) == static_cast<uint32_t>(width) &&
            std::max(1u, existing->second.height >> level) == static_cast<uint32_t>(height);
        if (!fits)
        {
            uint64_t const baseWidth = uint64_t(width) << level, baseHeight = uint64_t(height) << level;
            if (baseWidth > 16384 || baseHeight > 16384) { fail("Texture dimensions exceed the Metal device limit"); return; }
            Texture image;
            image.width = static_cast<uint32_t>(baseWidth);
            image.height = static_cast<uint32_t>(baseHeight);
            MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                width:image.width height:image.height mipmapped:YES];
            descriptor.storageMode = MTLStorageModePrivate;
            descriptor.usage = MTLTextureUsageShaderRead;
            image.image = [g.device newTextureWithDescriptor:descriptor];
            image.levels = static_cast<uint32_t>(descriptor.mipmapLevelCount);
            if (!image.image) { fail("Cannot allocate a Metal material texture"); return; }
            // Blits order updates after prior draws, without CPU/GPU idle waits.
            // Command-buffer retained references keep an old image alive when a
            // virtual texture ID changes dimensions or gets reused immediately.
            g.textures[texture] = image;
            existing = g.textures.find(texture);
            for (uint32_t mip = 0; mip < image.levels; ++mip)
                if (!copyPixels(image, mip, 0, 0, std::max(1u, image.width >> mip), std::max(1u, image.height >> mip), nullptr)) return;
        }
        if (rgba) metalUploadTextureRegion(texture, level, 0, 0, width, height, rgba);
    }
}

void metalUploadTextureRegion(uint32_t texture, int level, int x, int y, int width, int height, uint8_t const *rgba)
{
    @autoreleasepool
    {
        if (!metalActive() || g.failed || !rgba || width <= 0 || height <= 0) return;
        auto existing = g.textures.find(texture);
        if (existing == g.textures.end() || level < 0 || static_cast<uint32_t>(level) >= existing->second.levels || x < 0 || y < 0)
        { fail("Invalid texture subimage destination"); return; }
        Texture const &image = existing->second;
        if (uint64_t(x) + width > std::max(1u, image.width >> level) || uint64_t(y) + height > std::max(1u, image.height >> level))
        { fail("Texture subimage extends beyond its mip level"); return; }
        copyPixels(image, level, x, y, width, height, rgba);
    }
}

void metalDeleteTexture(uint32_t texture)
{
    @autoreleasepool { if (metalActive() && texture) g.textures.erase(texture); }
}

bool metalReadPixels(int x, int y, int width, int height, uint8_t *rgba)
{
    @autoreleasepool
    {
        if (!metalActive() || g.failed || g.suspended || !rgba || width <= 0 || height <= 0) return false;
        if (uint64_t(width) * uint64_t(height) > std::numeric_limits<size_t>::max() / 4) return false;
        std::memset(rgba, 0, static_cast<size_t>(width) * height * 4);
        int64_t const x0 = std::max<int64_t>(0, x), y0 = std::max<int64_t>(0, y);
        int64_t const x1 = std::min<int64_t>(g.width, int64_t(x) + width), y1 = std::min<int64_t>(g.height, int64_t(y) + height);
        if (x1 <= x0 || y1 <= y0) return true;
        if (!g.initialized && !beginRender()) return false;
        NSUInteger const clippedWidth = x1 - x0, clippedHeight = y1 - y0;
        NSUInteger const stride = (clippedWidth * 4 + 255) & ~NSUInteger(255);
        id<MTLBuffer> readback = [g.device newBufferWithLength:stride * clippedHeight options:MTLResourceStorageModeShared];
        if (!readback) return fail("Cannot allocate screenshot readback buffer");
        if (!beginBlit()) return false;
        [g.blit copyFromTexture:g.color sourceSlice:0 sourceLevel:0
            sourceOrigin:MTLOriginMake(x0, g.height - y1, 0) sourceSize:MTLSizeMake(clippedWidth, clippedHeight, 1)
            toBuffer:readback destinationOffset:0 destinationBytesPerRow:stride destinationBytesPerImage:stride * clippedHeight];
        id<MTLCommandBuffer> copied = g.command;
        if (!submit() || !waitCommand(copied)) return false;
        uint8_t const *source = static_cast<uint8_t const *>(readback.contents);
        for (NSUInteger row = 0; row < clippedHeight; ++row)
        {
            uint8_t const *input = source + (clippedHeight - 1 - row) * stride;
            uint8_t *output = rgba + (static_cast<size_t>(y0 - y + row) * width + x0 - x) * 4;
            for (NSUInteger column = 0; column < clippedWidth; ++column)
            {
                output[column * 4] = input[column * 4 + 2];
                output[column * 4 + 1] = input[column * 4 + 1];
                output[column * 4 + 2] = input[column * 4];
                output[column * 4 + 3] = input[column * 4 + 3];
            }
        }
        return true;
    }
}

char const *metalDeviceName() { return g.name.c_str(); }
int metalMaxTextureSize() { return 16384; }
float metalMaxAnisotropy() { return 16.f; }
bool metalDepthClampSupported() { return true; }
#endif // USE_METAL
