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

float shadowVisibility(int lightIndex, vec3 fromLight, float radius, float diffuse)
{
    if (u_lightParams.y < 0.5) return 1.0;
    vec3 a = abs(fromLight);
    vec3 face;
    int faceIndex;
    if (a.x >= a.y && a.x >= a.z)
    {
        if (fromLight.x > 0.0) { faceIndex = 0; face = vec3(-fromLight.z, -fromLight.y, a.x); }
        else { faceIndex = 1; face = vec3(fromLight.z, -fromLight.y, a.x); }
    }
    else if (a.y >= a.z)
    {
        if (fromLight.y > 0.0) { faceIndex = 2; face = vec3(fromLight.x, fromLight.z, a.y); }
        else { faceIndex = 3; face = vec3(fromLight.x, -fromLight.z, a.y); }
    }
    else
    {
        if (fromLight.z > 0.0) { faceIndex = 4; face = vec3(fromLight.x, -fromLight.y, a.z); }
        else { faceIndex = 5; face = vec3(-fromLight.x, -fromLight.y, a.z); }
    }
    vec2 cell = vec2(lightIndex, faceIndex);
    vec2 dimensions = vec2(u_lightParams.w, 6.0);
    vec2 facePixel = (face.xy / max(face.z, 0.000001) * 0.5 + 0.5) * u_lightParams.z;
    float receiver = length(fromLight) / radius;
    // Bias scales with the light's radius and atlas texel footprint. Grazing
    // surfaces need more bias, without fixed Build-unit constants in this API.
    float bias = (0.35 + 1.25 * (1.0 - diffuse)) / u_lightParams.z;
    float visibility = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
        {
            vec2 pixel = clamp(facePixel + vec2(x, y), vec2(0.5), vec2(u_lightParams.z - 0.5));
            vec2 uv = (cell + pixel / u_lightParams.z) / dimensions;
            visibility += receiver - bias <= texture(s_shadow, uv).r ? 1.0 : 0.0;
        }
    return visibility / 9.0;
}

void main()
{
    fragColor = vec4(0.0);
    vec2 pixel = vec2(gl_FragCoord.x, 1.0 / u_aoTarget.y - gl_FragCoord.y);
    float depth = aoReadDepth(pixel);
    if (depth >= 0.999999 || depth <= 0.0) return;
    vec3 eye = aoPosition(pixel, depth);
    vec3 position = (u_eyeToWorld * vec4(eye, 1.0)).xyz;
    vec3 dx = mat3(u_eyeToWorld) * aoDerivative(pixel, vec2(1.0, 0.0), eye);
    vec3 dy = mat3(u_eyeToWorld) * aoDerivative(pixel, vec2(0.0, 1.0), eye);
    if (length(dx) < 1e-20 || length(dy) < 1e-20) return;
    vec3 normal = cross(normalize(dx), normalize(dy));
    if (length(normal) < 0.00001) return;
    normal = normalize(normal);
    if (dot(normal, position - u_eyeToWorld[3].xyz) > 0.0) normal = -normal;
    vec3 illumination = vec3(0.0);
    for (int i = 0; i < int(u_lightParams.x); ++i)
    {
        vec3 toLight = u_lights[i].positionRadius.xyz - position;
        float distanceToLight = length(toLight);
        float radius = u_lights[i].positionRadius.w;
        if (distanceToLight >= radius || distanceToLight < 0.000001) continue;
        float diffuse = max(dot(normal, toLight / distanceToLight), 0.0);
        if (diffuse <= 0.0) continue;
        float attenuation = 1.0 - distanceToLight / radius;
        float visibility = shadowVisibility(i, -toLight, radius, diffuse);
        illumination += u_lights[i].color.rgb * u_lights[i].color.a *
            (attenuation * attenuation * diffuse * visibility);
    }
    // DST_COLOR + ONE blending computes base * (1 + illumination), including
    // a strict identity outside the lights, and leaves destination alpha alone.
    fragColor = vec4(clamp(illumination, 0.0, 1.0), 0.0);
}
