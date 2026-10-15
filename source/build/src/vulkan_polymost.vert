#version 450
// SPDX-License-Identifier: GPL-2.0-only
// Derived from NBlood's polymost1Vert.glsl.
// Vulkan adaptation Copyright (C) 2026 NBlood contributors.

// The backend enables this only with matching VK_EXT_depth_clip_control
// viewport state, preserving OpenGL depth rounding where the device supports it.
layout(constant_id = 0) const bool negativeOneToOneDepth = false;

layout(location = 0) in vec4 a_position;
layout(location = 1) in vec4 a_color;
layout(location = 2) in vec4 a_texcoord0;
layout(location = 3) in vec4 a_texcoord3;
layout(location = 4) in vec4 a_texcoord4;

// Keep the member order and types identical to VulkanUniforms.
layout(set = 0, binding = 0, std140) uniform VulkanUniforms
{
    mat4 modelView;
    mat4 projection;
    mat4 textureMatrix[3];
    mat4 rotation;
    vec4 texturePosSize;
    vec4 halfTexelPalswapPos;
    vec4 palswapSizeClamp;
    vec4 shadeNumVis;
    vec4 fog;
    vec4 fogColor;
    vec4 effects;
    vec4 npot;
    vec4 correction;
    vec4 misc;
    vec4 fixedState;
} u;

layout(location = 0) out vec4 v_color;
layout(location = 1) out float v_distance;
layout(location = 2) out float v_fogDistance;
layout(location = 3) out vec4 v_texcoord0;
layout(location = 4) out vec4 v_texcoord3;
layout(location = 5) out vec4 v_texcoord4;

void main()
{
    bool polymost = u.misc.w == 1.0;
    vec4 vertex = polymost ? u.rotation * a_position : a_position;
    vec4 eyePosition = u.modelView * vertex;
    // Polymost's GL shader uses the precomposed ModelViewProjection matrix.
    // Preserve that multiplication order, including its depth rounding at
    // coplanar model/floor surfaces. Other modes receive plain projection.
    vec4 clipPosition = u.projection * ((polymost || u.misc.w == 4.0) ? vertex : eyePosition);
    if (u.misc.w == 3.0)
        clipPosition = a_position; // glsurface already submits clip coordinates.

    // Preserve OpenGL's -w..w clip depth when the extension is active; otherwise
    // convert to Vulkan's default 0..w range. Framebuffer Y points downwards.
    gl_Position = vec4(clipPosition.x, -clipPosition.y,
                       negativeOneToOneDepth ? clipPosition.z : (clipPosition.z + clipPosition.w) * 0.5,
                       clipPosition.w);
    gl_PointSize = 1.0;

    v_texcoord0 = u.textureMatrix[0] * a_texcoord0;
    if (polymost)
        v_texcoord0 = mix(v_texcoord0.xyzw, v_texcoord0.yxzw, u.effects.y);
    v_texcoord3 = u.textureMatrix[1] * a_texcoord3;
    v_texcoord4 = u.textureMatrix[2] * a_texcoord4;
    if (u.misc.w == 3.0 || u.misc.w == 4.0)
        v_texcoord0 = a_texcoord0;

    v_fogDistance = abs(eyePosition.z / eyePosition.w);
    v_distance = a_position.z;
    v_color = a_color;
}
