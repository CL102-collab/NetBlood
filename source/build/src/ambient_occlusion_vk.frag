#version 450
#extension GL_GOOGLE_include_directive : require
// SPDX-License-Identifier: GPL-2.0-only

layout(set = 0, binding = 0, std140) uniform AmbientOcclusionUniforms
{
    vec4 u_aoProjection;
    vec4 u_aoDepth;
    vec4 u_aoViewport;
    vec4 u_aoTarget;
};
layout(set = 0, binding = 1) uniform sampler2D s_depth;
layout(set = 0, binding = 2) uniform usampler2D s_stencil;
layout(location = 0) out vec4 fragColor;

float aoSampleDepth(vec2 uv)
{
    // The shared algorithm uses OpenGL's bottom-left coordinates; native
    // Vulkan image rows, and gl_FragCoord, have their origin at the top left.
    vec2 coordinate = vec2(uv.x, 1.0 - uv.y);
    return (texture(s_stencil, coordinate).r & 128u) != 0u
        ? texture(s_depth, coordinate).r : 1.0;
}
#define AO_DEPTH(uv) aoSampleDepth(uv)
#include "ambient_occlusion.glsl"

void main()
{
    float factor = ambientOcclusion(vec2(gl_FragCoord.x, 1.0 / u_aoTarget.y - gl_FragCoord.y));
    fragColor = vec4(vec3(factor), 1.0);
}
