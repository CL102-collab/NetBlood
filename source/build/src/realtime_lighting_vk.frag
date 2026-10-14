#version 450
#extension GL_GOOGLE_include_directive : require
// SPDX-License-Identifier: GPL-2.0-only
struct Light
{
    vec4 positionRadius;
    vec4 color;
};
layout(set = 0, binding = 0, std140) uniform RealtimeLightingUniforms
{
    vec4 u_aoProjection;
    vec4 u_aoDepth;
    vec4 u_aoViewport;
    vec4 u_aoTarget;
    mat4 u_eyeToWorld;
    vec4 u_lightParams; // count, shadows enabled, face resolution, atlas columns
    Light u_lights[8];
};
layout(set = 0, binding = 1) uniform sampler2D s_depth;
layout(set = 0, binding = 2) uniform usampler2D s_stencil;
layout(set = 0, binding = 3) uniform sampler2D s_shadow;
layout(location = 0) out vec4 fragColor;

float lightingDepth(vec2 uv)
{
    vec2 coordinate = vec2(uv.x, 1.0 - uv.y);
    return (texture(s_stencil, coordinate).r & 128u) != 0u
        ? texture(s_depth, coordinate).r : 1.0;
}
// Reuse the exact positive-Z reconstruction and silhouette-aware derivatives
// used by AO. The AO evaluation function itself is eliminated as unused.
#define AO_DEPTH(uv) lightingDepth(uv)
#include "ambient_occlusion.glsl"

#define LIGHTING_SHADOW(uv) texture(s_shadow, uv).r
#define LIGHT_POSITION(i) u_lights[i].positionRadius
#define LIGHT_COLOR(i) u_lights[i].color
#include "realtime_lighting.glsl"

void main()
{
    vec2 pixel = vec2(gl_FragCoord.x, 1.0 / u_aoTarget.y - gl_FragCoord.y);
    // DST_COLOR + ONE blending computes base * (1 + illumination), including
    // a strict identity outside the lights, and leaves destination alpha alone.
    fragColor = vec4(realtimeLighting(pixel), 0.0);
}
