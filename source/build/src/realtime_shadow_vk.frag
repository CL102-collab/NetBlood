#version 450
// SPDX-License-Identifier: GPL-2.0-only
layout(location = 0) in vec3 v_lightPosition;
layout(push_constant) uniform ShadowPush
{
    vec4 u_light;
    ivec4 u_face;
};

void main()
{
    // Radial depth makes comparisons identical on all six cubemap faces.
    gl_FragDepth = min(length(v_lightPosition) / u_light.w, 1.0);
}
