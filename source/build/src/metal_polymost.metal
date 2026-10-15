// Native Metal shaders for the shared Polymost scene frontend.
// Derived from polymost1Vert.glsl, polymost1Frag.glsl, ambient_occlusion.glsl,
// realtime_lighting.glsl, and realtime_shadow_vk.vert/.frag.
// Copyright (C) 2026 NetBlood contributors. SPDX-License-Identifier: GPL-2.0-only
#include <metal_stdlib>
using namespace metal;

// Keep this layout identical to NativeUniforms. Matrix storage is column-major
// in both the shared frontend and MSL; each member starts at a 16-byte boundary.
struct MetalUniforms
{
    float4x4 modelView;
    float4x4 projection;
    float4x4 textureMatrix[3];
    float4x4 rotation;
    float4 texturePosSize;
    float4 halfTexelPalswapPos;
    float4 palswapSizeClamp;
    float4 shadeNumVis;
    float4 fog;
    float4 fogColor;
    float4 effects;
    float4 npot;
    float4 correction;
    float4 misc;
    float4 fixedState;
};

struct MetalVertex
{
    float4 position [[attribute(0)]];
    float4 color [[attribute(1)]];
    float4 texcoord0 [[attribute(2)]];
    float4 texcoord3 [[attribute(3)]];
    float4 texcoord4 [[attribute(4)]];
};

// point_size belongs to the vertex stage only. Keep it out of the fragment
// stage_in structure; the ordinary varyings link by matching names and types.
struct MetalVertexOutput
{
    float4 position [[position]];
    float pointSize [[point_size]];
    float4 color;
    float distance;
    float fogDistance;
    float4 texcoord0;
    float4 texcoord3;
    float4 texcoord4;
};

struct MetalVaryings
{
    float4 position [[position]];
    float4 color;
    float distance;
    float fogDistance;
    float4 texcoord0;
    float4 texcoord3;
    float4 texcoord4;
};

vertex MetalVertexOutput metalSceneVertex(MetalVertex input [[stage_in]],
                                          constant MetalUniforms &u [[buffer(1)]],
                                          constant float4 &viewportState [[buffer(2)]])
{
    MetalVertexOutput output;
    bool polymost = u.misc.w == 1.0f;
    float4 position = polymost ? u.rotation * input.position : input.position;
    float4 eyePosition = u.modelView * position;
    // Polymost supplies a precomposed projection * modelView. Preserve the
    // multiplication order at coplanar surfaces, including models and voxels.
    float4 clipPosition = u.projection * ((polymost || u.misc.w == 4.0f) ? position : eyePosition);
    if (u.misc.w == 3.0f)
        clipPosition = input.position; // glsurface submits clip coordinates.

    // Metal's NDC Y points up, as in OpenGL, while the viewport origin is at
    // the top left. Only depth needs conversion from GL's -w..w to Metal's 0..w.
    output.position = float4(clipPosition.xy, (clipPosition.z + clipPosition.w) * 0.5f,
                             clipPosition.w);
    // Metal requires znear <= zfar. For a reversed OpenGL depth range the
    // backend swaps the viewport limits and passes x=1 in this Metal-only
    // buffer, preserving the shared 560-byte NativeUniforms layout.
    if (viewportState.x != 0.0f)
        output.position.z = output.position.w - output.position.z;
    output.pointSize = 1.0f;
    output.texcoord0 = u.textureMatrix[0] * input.texcoord0;
    if (polymost)
        output.texcoord0 = mix(output.texcoord0.xyzw, output.texcoord0.yxzw, u.effects.y);
    output.texcoord3 = u.textureMatrix[1] * input.texcoord3;
    output.texcoord4 = u.textureMatrix[2] * input.texcoord4;
    if (u.misc.w == 3.0f || u.misc.w == 4.0f)
        output.texcoord0 = input.texcoord0;
    output.fogDistance = abs(eyePosition.z / eyePosition.w);
    output.distance = input.position.z;
    output.color = input.color;
    return output;
}

// GLSL mod is floor-based. MSL fmod truncates, which breaks negative Build
// texture coordinates and the wrap seam on non-power-of-two art textures.
static float glslMod(float x, float y) { return x - y * floor(x / y); }

static float4 colorCorrection(float4 color, constant MetalUniforms &u)
{
    const float2 zeroOne = float2(0.0f, 1.0f);
    const float4 luma = float4(0.2126f, 0.7152f, 0.0722f, 0.0f);
    float4 correction = u.correction;
    float4 cc = float4(correction.x - 1.0f, 0.5f * -(correction.y - 1.0f),
                      -(correction.z - 1.0f), 1.0f);
    return float4x4(zeroOne.yxxx, zeroOne.xyxx, zeroOne.xxyx, cc.xxxw)
         * float4x4(correction.ywww, correction.wyww, correction.wwyw, cc.yyyw)
         * float4x4((luma.xxxw * cc.z) + correction.zwww,
                    (luma.yyyw * cc.z) + correction.wzww,
                    (luma.zzzw * cc.z) + correction.wwzw, zeroOne.xxxy)
         * color;
}

static float2 npotCoordinate(float2 original, constant MetalUniforms &u)
{
    float2 coordinate = mix(original.xy, original.yx, u.effects.y);
    float modY = glslMod(coordinate.y, u.npot.y);
    coordinate = float2(floor(modY) * u.npot.x + coordinate.x,
                        floor(coordinate.y * u.npot.w) + modY);
    return mix(original, mix(coordinate.xy, coordinate.yx, u.effects.y), u.npot.z);
}

static float4 polymostColor(MetalVaryings input, constant MetalUniforms &u,
                            texture2d<float> image, sampler imageSampler,
                            texture2d<float> palswap, sampler palswapSampler,
                            texture2d<float> palette, sampler paletteSampler,
                            texture2d<float> detail, sampler detailSampler,
                            texture2d<float> glow, sampler glowSampler)
{
    float2 newCoordinate = npotCoordinate(input.texcoord0.xy, u);
    float2 coordinate = mix(fract(newCoordinate), clamp(newCoordinate, 0.0f, 1.0f),
                            u.palswapSizeClamp.zw);
    coordinate = clamp(u.texturePosSize.zw * coordinate, u.halfTexelPalswapPos.xy,
                       u.texturePosSize.zw - u.halfTexelPalswapPos.xy);
    float4 color = image.sample(imageSampler, u.texturePosSize.xy + coordinate,
                               gradient2d(dfdx(input.texcoord0.xy), dfdy(input.texcoord0.xy)));
    float shade = clamp(u.shadeNumVis.x + clamp(u.shadeNumVis.w * input.distance
                        - 0.5f * u.misc.y, 0.0f, u.shadeNumVis.y), 0.0f, u.shadeNumVis.y - 1.0f);
    float shadeFraction = glslMod(shade, 1.0f);
    const float basepalScale = 255.0f / 256.0f;
    const float basepalOffset = 0.5f / 256.0f;
    float index = palswap.sample(palswapSampler,
        float2(color.r, floor(shade) * u.shadeNumVis.z) * u.palswapSizeClamp.xy
        + u.halfTexelPalswapPos.zw).r * basepalScale + basepalOffset;
    float nextIndex = palswap.sample(palswapSampler,
        u.palswapSizeClamp.xy * float2(color.r, (floor(shade) + 1.0f) * u.shadeNumVis.z)
        + u.halfTexelPalswapPos.zw).r * basepalScale + basepalOffset;
    float4 palettedColor = palette.sample(paletteSampler, float2(index, 0.0f));
    float4 nextPalettedColor = palette.sample(paletteSampler, float2(nextIndex, 0.0f));
    palettedColor.rgb = mix(palettedColor.rgb, nextPalettedColor.rgb, shadeFraction * u.misc.y);
    float fullbright = mix(u.effects.y * palettedColor.a, 0.0f, u.effects.x);
    palettedColor.a = 1.0f - floor(color.r);
    color = mix(color, palettedColor, u.effects.y);

    float4 detailColor = detail.sample(detailSampler, input.texcoord3.xy);
    detailColor = mix(float4(1.0f), 2.0f * detailColor, u.effects.z * detailColor.a);
    color.rgb *= detailColor.rgb;
    color = mix(color, float4(1.0f), u.effects.x);
    color.rgb = mix(input.color.rgb * color.rgb, color.rgb, fullbright);
    fullbright = clamp(-(u.fog.x - 1.0f), fullbright, 1.0f);
    float fogFactor = clamp((u.fog.y - input.fogDistance) * u.fog.z, fullbright, 1.0f);
    color.rgb = mix(u.fogColor.rgb, color.rgb, fogFactor);

    float4 glowColor = glow.sample(glowSampler, npotCoordinate(input.texcoord4.xy, u));
    color.rgb = mix(color.rgb, glowColor.rgb, u.effects.w * glowColor.a * -(u.effects.x - 1.0f));
    color.a *= input.color.a;
    color.rgb = pow(color.rgb, float3(u.misc.x));
    return colorCorrection(color, u);
}

static float4 fixedColor(MetalVaryings input, constant MetalUniforms &u,
                         texture2d<float> image, sampler imageSampler)
{
    float4 color = input.color;
    if (u.fixedState.x != 0.0f)
    {
        float4 texel = image.sample(imageSampler, input.texcoord0.xy / input.texcoord0.w);
        color = u.fixedState.x == 2.0f ? texel : color * texel;
    }
    if (u.fixedState.y != 0.0f)
    {
        float fogFactor = clamp((u.fog.y - input.fogDistance) * u.fog.z, 0.0f, 1.0f);
        if (int(u.fixedState.w) == 0x0800) // GL_EXP
            fogFactor = clamp(exp(-u.fog.x * input.fogDistance), 0.0f, 1.0f);
        else if (int(u.fixedState.w) == 0x0801) // GL_EXP2
        {
            float densityDistance = u.fog.x * input.fogDistance;
            fogFactor = clamp(exp(-densityDistance * densityDistance), 0.0f, 1.0f);
        }
        color.rgb = mix(u.fogColor.rgb, color.rgb, fogFactor);
    }
    return color;
}

static bool alphaTest(float alpha, constant MetalUniforms &u)
{
    float value = clamp(alpha, 0.0f, 1.0f);
    float reference = clamp(u.fog.w, 0.0f, 1.0f);
    switch (int(u.fixedState.z))
    {
        case 0x0200: return false; // GL_NEVER
        case 0x0201: return value < reference;
        case 0x0202: return value == reference;
        case 0x0203: return value <= reference;
        case 0x0204: return value > reference;
        case 0x0205: return value != reference;
        case 0x0206: return value >= reference;
        default: return true; // GL_ALWAYS
    }
}

fragment float4 metalSceneFragment(MetalVaryings input [[stage_in]],
                                   constant MetalUniforms &u [[buffer(1)]],
                                   texture2d<float> image [[texture(0)]],
                                   texture2d<float> palswap [[texture(1)]],
                                   texture2d<float> palette [[texture(2)]],
                                   texture2d<float> detail [[texture(3)]],
                                   texture2d<float> glow [[texture(4)]],
                                   sampler imageSampler [[sampler(0)]],
                                   sampler palswapSampler [[sampler(1)]],
                                   sampler paletteSampler [[sampler(2)]],
                                   sampler detailSampler [[sampler(3)]],
                                   sampler glowSampler [[sampler(4)]])
{
    float4 color;
    switch (int(u.misc.w))
    {
        case 1:
            color = polymostColor(input, u, image, imageSampler, palswap, palswapSampler,
                                  palette, paletteSampler, detail, detailSampler, glow, glowSampler);
            break;
        case 2:
        {
            // animvpx.cpp submits packed YUV in an ordinary RGB texture.
            float3 yuv = image.sample(imageSampler, input.texcoord0.xy).rgb;
            float y = 1.1643f * (yuv.r - 0.0625f);
            float cb = yuv.g - 0.5f;
            float cr = yuv.b - 0.5f;
            color = float4(y + 1.5958f * cr, y - 0.39173f * cb - 0.81290f * cr,
                           y + 2.017f * cb, 1.0f);
            break;
        }
        case 3:
            color = image.sample(imageSampler, input.texcoord0.xy);
            color.r = (255.0f / 256.0f) * color.r + 0.5f / 256.0f;
            color.rgb = palette.sample(paletteSampler, color.rg).rgb;
            color = colorCorrection(color, u);
            break;
        case 4:
        {
            // Match Duke64's regular and two-color intensity materials.
            float2 coordinate = mix(input.texcoord0.xy, clamp(input.texcoord0.xy, 0.0f, 0.999f),
                                    u.palswapSizeClamp.zw);
            float4 texel = image.sample(imageSampler, coordinate, gradient2d(dfdx(coordinate), dfdy(coordinate)));
            float4 combined = float4(mix(u.effects.rgb, u.npot.rgb, texel.r), texel.a * input.color.a);
            color = mix(texel * input.color, combined, u.misc.x);
            break;
        }
        default:
            color = fixedColor(input, u, image, imageSampler);
            break;
    }
    if (u.misc.z != 0.0f && !alphaTest(color.a, u))
        discard_fragment();
    return color;
}

// The AO arithmetic mirrors ambient_occlusion.glsl. Its coordinate system is
// bottom-left OpenGL pixels; texture reads and fragment positions convert at
// the boundary. Stencil 0x80 excludes sky, portal masks, HUD, and other pixels
// that Polymost does not mark as opaque world geometry.
struct MetalAOUniforms
{
    float4 projection;
    float4 depth;
    float4 viewport;
    float4 target;
};

struct MetalAOVaryings { float4 position [[position]]; };

vertex MetalAOVaryings metalAOVertex(uint index [[vertex_id]])
{
    float2 corner = float2(float((index << 1) & 2u), float(index & 2u));
    MetalAOVaryings output;
    output.position = float4(corner * 2.0f - 1.0f, 0.0f, 1.0f);
    return output;
}

static bool aoInside(float2 pixel, constant MetalAOUniforms &u)
{
    return all(pixel >= max(u.viewport.xy, float2(0.5f))) &&
           all(pixel < min(u.viewport.xy + u.viewport.zw, float2(1.0f) / u.target.xy));
}

static float aoReadDepth(float2 pixel, constant MetalAOUniforms &u,
                          depth2d<float> depth, texture2d<uint, access::read> stencil,
                          sampler depthSampler)
{
    if (!aoInside(pixel, u)) return 1.0f;
    float2 coordinate = pixel * u.target.xy;
    coordinate.y = 1.0f - coordinate.y;
    uint2 texel = min(uint2(coordinate * float2(stencil.get_width(), stencil.get_height())),
                      uint2(stencil.get_width() - 1, stencil.get_height() - 1));
    return (stencil.read(texel).r & 128u) != 0u ? depth.sample(depthSampler, coordinate) : 1.0f;
}

static float3 aoPosition(float2 pixel, float depth, constant MetalAOUniforms &u)
{
    float2 ndc = (pixel - u.viewport.xy) / u.viewport.zw * 2.0f - 1.0f;
    float z = u.depth.y / (2.0f * depth - 1.0f - u.depth.x);
    return float3((ndc - u.projection.zw) * z / u.projection.xy, z);
}

static float3 aoDerivative(float2 pixel, float2 stepPixel, float3 position,
                            constant MetalAOUniforms &u, depth2d<float> depth,
                            texture2d<uint, access::read> stencil, sampler depthSampler)
{
    float beforeDepth = aoReadDepth(pixel - stepPixel, u, depth, stencil, depthSampler);
    float afterDepth = aoReadDepth(pixel + stepPixel, u, depth, stencil, depthSampler);
    if (beforeDepth >= 0.999999f && afterDepth >= 0.999999f) return float3(0.0f);
    if (beforeDepth >= 0.999999f) return aoPosition(pixel + stepPixel, afterDepth, u) - position;
    if (afterDepth >= 0.999999f) return position - aoPosition(pixel - stepPixel, beforeDepth, u);
    float3 before = position - aoPosition(pixel - stepPixel, beforeDepth, u);
    float3 after = aoPosition(pixel + stepPixel, afterDepth, u) - position;
    return abs(before.z) < abs(after.z) ? before : after;
}

static float ambientOcclusion(float2 pixel, constant MetalAOUniforms &u,
                               depth2d<float> depthImage, texture2d<uint, access::read> stencil,
                               sampler depthSampler)
{
    float depth = aoReadDepth(pixel, u, depthImage, stencil, depthSampler);
    if (depth >= 0.999999f || depth <= 0.0f) return 1.0f;
    float3 position = aoPosition(pixel, depth, u);
    float3 dx = aoDerivative(pixel, float2(1.0f, 0.0f), position, u, depthImage, stencil, depthSampler);
    float3 dy = aoDerivative(pixel, float2(0.0f, 1.0f), position, u, depthImage, stencil, depthSampler);
    float dxLength = length(dx), dyLength = length(dy);
    if (dxLength < 1.0e-20f || dyLength < 1.0e-20f) return 1.0f;
    float3 normal = cross(dx / dxLength, dy / dyLength);
    float normalLength = length(normal);
    if (normalLength < 0.00001f) return 1.0f;
    normal /= normalLength;
    if (dot(normal, position) > 0.0f) normal = -normal;

    float radius = u.depth.z;
    float2 radiusPixels = 0.5f * u.viewport.zw * u.projection.xy * radius / position.z;
    radiusPixels *= min(1.0f, 64.0f / max(radiusPixels.x, radiusPixels.y));
    float occlusion = 0.0f;
    for (int direction = 0; direction < 8; ++direction)
    {
        float angle = float(direction) * 0.78539816339f;
        float2 ray = float2(cos(angle), sin(angle)) * radiusPixels;
        for (int ring = 0; ring < 2; ++ring)
        {
            float scale = ring == 0 ? 0.35f : 0.80f;
            float2 samplePixel = floor(pixel + ray * scale) + 0.5f;
            float sampleDepth = aoReadDepth(samplePixel, u, depthImage, stencil, depthSampler);
            if (sampleDepth >= 0.999999f || sampleDepth <= 0.0f) continue;
            float3 delta = aoPosition(samplePixel, sampleDepth, u) - position;
            float distanceToSample = length(delta);
            if (distanceToSample <= radius * 0.001f || distanceToSample >= radius) continue;
            float falloff = 1.0f - distanceToSample / radius;
            float horizon = max(dot(normal, delta / distanceToSample) - u.depth.w, 0.0f);
            occlusion += horizon * falloff * falloff;
        }
    }
    return clamp(1.0f - u.target.z * occlusion / 16.0f, 0.35f, 1.0f);
}

fragment float4 metalAOFragment(MetalAOVaryings input [[stage_in]],
                                constant MetalAOUniforms &u [[buffer(0)]],
                                depth2d<float> depth [[texture(0)]],
                                texture2d<uint, access::read> stencil [[texture(1)]],
                                sampler depthSampler [[sampler(0)]])
{
    float factor = ambientOcclusion(float2(input.position.x, 1.0f / u.target.y - input.position.y),
                                     u, depth, stencil, depthSampler);
    return float4(float3(factor), 1.0f);
}

// Keep the 400-byte layout identical to the backend's RealtimeLightingUniforms:
// AO occupies bytes 0..63, eyeToWorld 64..127, params 128..143, and lights 144..399.
struct MetalPointLight
{
    float4 positionRadius;
    float4 color;
};

struct MetalLightingUniforms
{
    MetalAOUniforms ao;
    float4x4 eyeToWorld;
    float4 params; // count, shadows enabled, face resolution, atlas columns
    MetalPointLight lights[8];
};

// Arithmetic mirrors realtime_lighting.glsl. The atlas uses top-origin Metal
// texture coordinates; metalShadowVertex flips clip Y so the common face UV
// formula still addresses the same world-space direction without a sample flip.
static float shadowVisibility(int lightIndex, float3 fromLight, float radius, float diffuse,
                               constant MetalLightingUniforms &u,
                               depth2d<float> shadow, sampler depthSampler)
{
    if (u.params.y < 0.5f) return 1.0f;
    float3 a = abs(fromLight);
    float3 face;
    int faceIndex;
    if (a.x >= a.y && a.x >= a.z)
    {
        if (fromLight.x > 0.0f) { faceIndex = 0; face = float3(-fromLight.z, -fromLight.y, a.x); }
        else { faceIndex = 1; face = float3(fromLight.z, -fromLight.y, a.x); }
    }
    else if (a.y >= a.z)
    {
        if (fromLight.y > 0.0f) { faceIndex = 2; face = float3(fromLight.x, fromLight.z, a.y); }
        else { faceIndex = 3; face = float3(fromLight.x, -fromLight.z, a.y); }
    }
    else
    {
        if (fromLight.z > 0.0f) { faceIndex = 4; face = float3(fromLight.x, -fromLight.y, a.z); }
        else { faceIndex = 5; face = float3(-fromLight.x, -fromLight.y, a.z); }
    }
    float2 cell = float2(lightIndex, faceIndex);
    float2 dimensions = float2(u.params.w, 6.0f);
    float2 facePixel = (face.xy / max(face.z, 0.000001f) * 0.5f + 0.5f) * u.params.z;
    float receiver = length(fromLight) / radius;
    float bias = (0.35f + 1.25f * (1.0f - diffuse)) / u.params.z;
    float visibility = 0.0f;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
        {
            float2 pixel = clamp(facePixel + float2(x, y), float2(0.5f), float2(u.params.z - 0.5f));
            float2 uv = (cell + pixel / u.params.z) / dimensions;
            visibility += receiver - bias <= shadow.sample(depthSampler, uv) ? 1.0f : 0.0f;
        }
    return visibility / 9.0f;
}

static float3 realtimeLighting(float2 pixel, constant MetalLightingUniforms &u,
                                depth2d<float> depthImage, texture2d<uint, access::read> stencil,
                                depth2d<float> shadow, sampler depthSampler)
{
    float depth = aoReadDepth(pixel, u.ao, depthImage, stencil, depthSampler);
    if (depth >= 0.999999f || depth <= 0.0f) return float3(0.0f);
    float3 eye = aoPosition(pixel, depth, u.ao);
    float3 position = (u.eyeToWorld * float4(eye, 1.0f)).xyz;
    float3x3 directionToWorld = float3x3(u.eyeToWorld[0].xyz, u.eyeToWorld[1].xyz, u.eyeToWorld[2].xyz);
    float3 dx = directionToWorld * aoDerivative(pixel, float2(1.0f, 0.0f), eye,
                                                u.ao, depthImage, stencil, depthSampler);
    float3 dy = directionToWorld * aoDerivative(pixel, float2(0.0f, 1.0f), eye,
                                                u.ao, depthImage, stencil, depthSampler);
    if (length(dx) < 1.0e-20f || length(dy) < 1.0e-20f) return float3(0.0f);
    float3 normal = cross(normalize(dx), normalize(dy));
    if (length(normal) < 0.00001f) return float3(0.0f);
    normal = normalize(normal);
    if (dot(normal, position - u.eyeToWorld[3].xyz) > 0.0f) normal = -normal;
    float3 illumination = float3(0.0f);
    for (int i = 0; i < int(u.params.x); ++i)
    {
        float3 toLight = u.lights[i].positionRadius.xyz - position;
        float distanceToLight = length(toLight);
        float radius = u.lights[i].positionRadius.w;
        if (distanceToLight >= radius || distanceToLight < 0.000001f) continue;
        float diffuse = max(dot(normal, toLight / distanceToLight), 0.0f);
        if (diffuse <= 0.0f) continue;
        float attenuation = 1.0f - distanceToLight / radius;
        float visibility = shadowVisibility(i, -toLight, radius, diffuse, u, shadow, depthSampler);
        illumination += u.lights[i].color.rgb * u.lights[i].color.a *
            (attenuation * attenuation * diffuse * visibility);
    }
    return clamp(illumination, 0.0f, 1.0f);
}

fragment float4 metalLightingFragment(MetalAOVaryings input [[stage_in]],
                                      constant MetalLightingUniforms &u [[buffer(0)]],
                                      depth2d<float> depth [[texture(0)]],
                                      texture2d<uint, access::read> stencil [[texture(1)]],
                                      depth2d<float> shadow [[texture(2)]],
                                      sampler depthSampler [[sampler(0)]])
{
    // Scene reconstruction, unlike atlas sampling, uses bottom-left pixels.
    float2 pixel = float2(input.position.x, 1.0f / u.ao.target.y - input.position.y);
    // DST_COLOR + ONE blends base * (1 + illumination); RGB-only writes keep
    // destination alpha unchanged, including the strict identity outside lights.
    return float4(realtimeLighting(pixel, u, depth, stencil, shadow, depthSampler), 0.0f);
}

struct MetalShadowUniforms
{
    float4 light;
    int4 face;
};

struct MetalShadowVaryings
{
    float4 position [[position]];
    float3 lightPosition;
};

vertex MetalShadowVaryings metalShadowVertex(uint index [[vertex_id]],
                                             device const packed_float3 *positions [[buffer(0)]],
                                             constant MetalShadowUniforms &u [[buffer(1)]])
{
    float3 p = float3(positions[index]) - u.light.xyz;
    MetalShadowVaryings output;
    output.lightPosition = p;
    float3 face;
    if (u.face.x == 0) face = float3(-p.z, -p.y, p.x);
    else if (u.face.x == 1) face = float3(p.z, -p.y, -p.x);
    else if (u.face.x == 2) face = float3(p.x, p.z, p.y);
    else if (u.face.x == 3) face = float3(p.x, -p.z, -p.y);
    else if (u.face.x == 4) face = float3(p.x, -p.y, p.z);
    else face = float3(-p.x, -p.y, -p.z);
    float nearPlane = max(0.001f, u.light.w / 4096.0f);
    float farPlane = u.light.w;
    // Metal NDC Y points up, framebuffer/texture Y down. Negating clip Y makes
    // a top-origin atlas cell project to face.xy / face.z * 0.5 + 0.5, exactly
    // the shared cubemap lookup above. Metal's depth interval is already 0..w.
    output.position = float4(face.x, -face.y,
                              farPlane * (face.z - nearPlane) / (farPlane - nearPlane), face.z);
    return output;
}

struct MetalShadowDepth { float depth [[depth(any)]]; };

fragment MetalShadowDepth metalShadowFragment(MetalShadowVaryings input [[stage_in]],
                                               constant MetalShadowUniforms &u [[buffer(1)]])
{
    MetalShadowDepth output;
    // Radial depth keeps receiver comparisons identical on all six faces.
    output.depth = min(length(input.lightPosition) / u.light.w, 1.0f);
    return output;
}

// The game's logical framebuffer is independent of a Retina drawable's size.
// Presenting it through a native Metal pass scales the completed image without
// changing screenshots, save thumbnails, or viewport-dependent scene math.
struct MetalPresentVaryings
{
    float4 position [[position]];
    float2 coordinate;
};

vertex MetalPresentVaryings metalPresentVertex(uint index [[vertex_id]])
{
    float2 corner = float2(float((index << 1) & 2u), float(index & 2u));
    MetalPresentVaryings output;
    output.position = float4(corner * 2.0f - 1.0f, 0.0f, 1.0f);
    output.coordinate = float2(corner.x, 1.0f - corner.y);
    return output;
}

fragment float4 metalPresentFragment(MetalPresentVaryings input [[stage_in]],
                                     texture2d<float> image [[texture(0)]],
                                     sampler imageSampler [[sampler(0)]])
{
    return image.sample(imageSampler, input.coordinate);
}
